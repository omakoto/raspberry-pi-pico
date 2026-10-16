#pragma once

#include <string>

class MdnsService {
public:
    MdnsService();
    ~MdnsService();

    bool init(const std::string& hostname = "wol", int http_port = 80);
    void stop();

private:
    bool initialized_;
    std::string hostname_;
    int http_port_;
};
