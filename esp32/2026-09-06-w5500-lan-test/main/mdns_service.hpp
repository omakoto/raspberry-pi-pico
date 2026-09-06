#pragma once

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
