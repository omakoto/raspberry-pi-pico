/*
 * mDNS Service Responder Implementation for nsbackend-pico.
 * Uses lwIP mDNS responder to advertise hostname.local and _nscon._tcp service.
 */

#include "mdns_service.hpp"

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mdns.h"
#include "dual_logger.hpp"

static const char* TAG = "MdnsService";
static bool s_mdns_initialized = false;

MdnsService::MdnsService() : initialized_(false), tcp_port_(10100) {}

MdnsService::~MdnsService() {
    stop();
}

bool MdnsService::init(const std::string& hostname, int tcp_port) {
    stop();

    hostname_ = hostname;
    tcp_port_ = tcp_port;

    cyw43_arch_lwip_begin();

    if (!s_mdns_initialized) {
        mdns_resp_init();
        s_mdns_initialized = true;
    }

    struct netif* nif = &cyw43_state.netif[CYW43_ITF_STA];
    err_t ret = mdns_resp_add_netif(nif, hostname_.c_str());
    if (ret != ERR_OK) {
        LOG_W(TAG, "mdns_resp_add_netif returned %d", ret);
    }

    s8_t slot = mdns_resp_add_service(nif, "Nintendo Switch Controller Backend",
                                      "_nscon", DNSSD_PROTO_TCP, static_cast<u16_t>(tcp_port_),
                                      nullptr, nullptr);
    if (slot < 0) {
        LOG_W(TAG, "mdns_resp_add_service returned slot %d", slot);
    }

    mdns_resp_announce(nif);

    cyw43_arch_lwip_end();

    initialized_ = true;
    LOG_I(TAG, "mDNS responder started: %s.local -> _nscon._tcp on port %d", hostname_.c_str(), tcp_port_);
    return true;
}

void MdnsService::stop() {
    if (initialized_) {
        cyw43_arch_lwip_begin();
        struct netif* nif = &cyw43_state.netif[CYW43_ITF_STA];
        mdns_resp_remove_netif(nif);
        cyw43_arch_lwip_end();
        initialized_ = false;
    }
}

#endif // NSBACKEND_HAS_WIFI
