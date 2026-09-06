/*
 * mDNS Service Responder for nsbackend-pico.
 * Registers hostname.local and _nscon._tcp service discovery via lwIP.
 */

#pragma once

#if defined(NSBACKEND_HAS_WIFI) && NSBACKEND_HAS_WIFI

#include <string>

class MdnsService {
public:
    MdnsService();
    ~MdnsService();

    bool init(const std::string& hostname, int tcp_port);
    void stop();

private:
    bool initialized_;
    std::string hostname_;
    int tcp_port_;
};

#endif // NSBACKEND_HAS_WIFI
