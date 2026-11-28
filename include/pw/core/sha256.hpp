#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pw {

// Self-contained SHA-256 (FIPS 180-4). Used for cleanup plan hashes; the
// plan must be re-approved when its contents change.
std::array<uint8_t, 32> sha256(const void* data, std::size_t n);
std::string sha256_hex(const std::string& s);

}  // namespace pw
