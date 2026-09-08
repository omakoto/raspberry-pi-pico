/*
 * lwIP configuration for otg-lan-test.
 *
 * lwIP runs in NO_SYS mode: there is no RTOS, main() polls TinyUSB, feeds received
 * Ethernet frames into lwIP, and runs lwIP's timers. Only the raw API is used
 * (dhserver and httpd are both raw-API apps), so sockets and netconn are disabled.
 */

#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_NETIF_API                  0
#define SYS_LIGHTWEIGHT_PROT            0

#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (8 * 1024)
#define PBUF_POOL_SIZE                  8
#define PBUF_POOL_BUFSIZE               1536
#define MEMP_NUM_TCP_PCB                4
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_SYS_TIMEOUT            8

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_RAW                        0
#define LWIP_DNS                        0
#define LWIP_IGMP                       0

// We are the DHCP *server*, not a client. A DHCPDISCOVER arrives with source IP 0.0.0.0
// and destination 255.255.255.255; lwIP only lets such packets through to UDP when the
// DHCP client is enabled or when this hook accepts the destination port.
#define LWIP_DHCP                       0
#define LWIP_IP_ACCEPT_UDP_PORT(p)      ((p) == PP_NTOHS(67))

// There is exactly one interface (the USB link), which lets lwIP skip netif lookups.
#define LWIP_SINGLE_NETIF               1

#define TCP_MSS                         (1500 /*mtu*/ - 20 /*iphdr*/ - 20 /*tcphdr*/)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_BROADCAST_PING             1
#define LWIP_MULTICAST_PING             1

// httpd: serve the static content generated from fs/ by pico_set_lwip_httpd_content().
#define HTTPD_FSDATA_FILE               "pico_fsdata.inc"
#define LWIP_HTTPD_CGI                  0
#define LWIP_HTTPD_SSI                  0
#define LWIP_HTTPD_SSI_INCLUDE_TAG      0
#define LWIP_HTTPD_SUPPORT_11_KEEPALIVE 0
#define LWIP_HTTPD_DYNAMIC_HEADERS      0

#endif /* _LWIPOPTS_H */
