/*
 * otg-lan-test: USB Ethernet gadget with DHCP + HTTP server.
 *
 * The Pico's native USB port enumerates as a USB Ethernet adapter (RNDIS or CDC-ECM,
 * see usb_descriptors.c). On the Pico side of that link:
 *   - lwIP owns a single network interface with the static address 192.168.7.1/24,
 *   - a DHCP server hands the host PC an address in 192.168.7.0/24 (192.168.7.2 first),
 *   - lwIP's httpd serves the files baked in from fs/ (index.html says "Hello world!").
 *
 * A CDC ACM serial console rides along on the same USB device (pico_stdio_usb): printf
 * output goes there and to UART0, and the host can reboot the board into BOOTSEL by
 * setting 1200 baud or via `picotool reboot` (reset interface).
 *
 * Everything runs on the main loop without an RTOS: TinyUSB is polled with tud_task(),
 * frames it received are handed to lwIP, and lwIP's timers are run from the same loop.
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"

#include "dhserver.h"
#include "lwip/apps/httpd.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/etharp.h"

#define INIT_IP4(a, b, c, d) {PP_HTONL(LWIP_MAKEU32(a, b, c, d))}

// The host-side MAC lives in usb_descriptors.c; TinyUSB's net driver reports it to the host.
extern uint8_t tud_network_mac_address[6];

static struct netif netif_data;

// Frame received by TinyUSB (from its ISR-free tud_task() context) that the main loop has
// not yet pushed into lwIP. TinyUSB is told not to deliver another one until this is consumed.
static struct pbuf *received_frame;

// Our own address on the USB link.
static const ip4_addr_t ipaddr = INIT_IP4(192, 168, 7, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

// Addresses the DHCP server may hand out. Must live in RAM: the server records the
// client MAC that holds each lease in place.
static dhcp_entry_t dhcp_entries[] = {
    /* mac    ip address                  lease time */
    {{0}, INIT_IP4(192, 168, 7, 2), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 3), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 4), 24 * 60 * 60},
};

static const dhcp_config_t dhcp_config = {
    // Deliberately no router and no DNS server: the host must not route its default
    // traffic (or its DNS lookups) through this link, only 192.168.7.0/24.
    .router = INIT_IP4(0, 0, 0, 0),
    .port = 67,
    .dns = INIT_IP4(0, 0, 0, 0),
    .domain = "usb",
    .num_entry = sizeof(dhcp_entries) / sizeof(dhcp_entries[0]),
    .entries = dhcp_entries,
};

//--------------------------------------------------------------------+
// lwIP <-> TinyUSB glue
//--------------------------------------------------------------------+

// lwIP wants to send an Ethernet frame to the host.
static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void)netif;
    for (;;) {
        // If USB is not configured there is nobody to send to; tell lwIP so it drops the frame.
        if (!tud_ready()) return ERR_USE;

        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0 /* unused */);
            return ERR_OK;
        }

        // The previous frame is still in flight; let TinyUSB make progress and retry.
        tud_task();
    }
}

static err_t ip4_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
    return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif) {
    netif->mtu = CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    netif->state = NULL;
    netif->name[0] = 'u';
    netif->name[1] = 's';
    netif->linkoutput = linkoutput_fn;
    netif->output = ip4_output_fn;
    return ERR_OK;
}

// TinyUSB copies an incoming frame into a pbuf for us. Returning false makes TinyUSB
// retry later without dropping the frame.
bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (received_frame) return false;

    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            memcpy(p->payload, src, size);
            received_frame = p;
        }
    }
    return true;
}

// TinyUSB asks us to serialize the pbuf we queued with tud_network_xmit() into its buffer.
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)arg;
    struct pbuf *p = (struct pbuf *)ref;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

// Called when the host (re)initializes the network function, e.g. RNDIS INITIALIZE.
void tud_network_init_cb(void) {
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
}

static void service_traffic(void) {
    if (received_frame) {
        // ethernet_input() takes ownership of the pbuf on success; on failure it is ours to free.
        if (ethernet_input(received_frame, &netif_data) != ERR_OK) {
            pbuf_free(received_frame);
        }
        received_frame = NULL;
        tud_network_recv_renew();
    }
    sys_check_timeouts();
}

//--------------------------------------------------------------------+
// lwIP NO_SYS platform hooks (normally supplied by an OS port)
//--------------------------------------------------------------------+

sys_prot_t sys_arch_protect(void) {
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
}

uint32_t sys_now(void) {
    return to_ms_since_boot(get_absolute_time());
}

//--------------------------------------------------------------------+
// Setup
//--------------------------------------------------------------------+

static void init_mac_addresses(void) {
    // Locally administered (bit 1 of the first byte set), unicast, derived from the
    // flash unique ID so every board gets a stable and distinct pair of addresses.
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    tud_network_mac_address[0] = 0x02;
    memcpy(&tud_network_mac_address[1], &id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 5], 5);
}

static void init_lwip(void) {
    struct netif *netif = &netif_data;

    lwip_init();

    // Both ends of the link need distinct MACs; the Pico side flips the lowest bit of the
    // MAC the host was given.
    netif->hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
    netif->hwaddr[5] ^= 0x01;

    netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ethernet_input);
    netif_set_default(netif);
    netif_set_link_up(netif);
    netif_set_up(netif);
}

int main(void) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    // The MAC must be final before the host can read the descriptors.
    init_mac_addresses();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(0, &dev_init);

    // After tusb_init(): the USB stdio driver expects TinyUSB to already be initialized
    // when the application owns it, and only attaches to the CDC interface.
    stdio_init_all();
    printf("\notg-lan-test starting\n");
    printf("Host-side MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", tud_network_mac_address[0], tud_network_mac_address[1],
           tud_network_mac_address[2], tud_network_mac_address[3], tud_network_mac_address[4],
           tud_network_mac_address[5]);

    init_lwip();
    while (dhserv_init(&dhcp_config) != ERR_OK) {
        // Only fails if lwIP cannot allocate the UDP PCB, which cannot happen this early.
    }
    httpd_init();

    printf("lwIP up: %s/%s, DHCP server on UDP 67, HTTP server on TCP 80\n", ip4addr_ntoa(&ipaddr),
           ip4addr_ntoa(&netmask));

    absolute_time_t next_blink = make_timeout_time_ms(500);
    bool was_mounted = false;

    while (true) {
        tud_task();
        service_traffic();

        bool mounted = tud_mounted();
        if (mounted != was_mounted) {
            printf("USB %s\n", mounted ? "configured by host" : "disconnected");
            was_mounted = mounted;
        }

#ifdef PICO_DEFAULT_LED_PIN
        // Heartbeat: slow blink while waiting for the host, fast blink once configured.
        if (time_reached(next_blink)) {
            gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
            next_blink = make_timeout_time_ms(mounted ? 150 : 500);
        }
#endif
    }
}
