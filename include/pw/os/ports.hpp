#pragma once

#include <cstdint>
#include <vector>

#include "pw/result.hpp"

namespace pw {

struct PortOwner {
    uint16_t port = 0;
    int32_t pid = 0;
};

// libproc: enumerate this uid's listening TCP sockets (IPv4 + IPv6).
Result<std::vector<PortOwner>> listening_ports();

}  // namespace pw
