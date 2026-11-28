#pragma once

#include <cstdint>
#include <string>

#include "pw/result.hpp"

namespace pw {

// Lab-allowed fts walk: recursive byte size of regular files under a path.
// Missing/inaccessible entries are skipped (0 bytes), never invented.
Result<uint64_t> dir_size(const std::string& path);

}  // namespace pw
