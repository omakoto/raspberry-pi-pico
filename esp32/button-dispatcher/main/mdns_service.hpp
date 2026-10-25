#pragma once

#include <string>

class MdnsService {
public:
    MdnsService();
    ~MdnsService();

    bool init(const std::string& hostname = "dispatcher", int http_port = 80);
    void stop();
    // Renames the board on the network ("<hostname>.local"); takes effect at once.
    void set_hostname(const std::string& hostname);

private:
    bool initialized_;
    std::string hostname_;
    int http_port_;
};
