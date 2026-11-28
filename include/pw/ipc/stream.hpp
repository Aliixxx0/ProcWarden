#pragma once

#include <string>

#include "pw/ipc/status_table.hpp"

namespace pw {

// One JSON object per line, mirroring the shared-memory table (spec 3.4).
std::string status_to_json(const StatusTable& t);

}  // namespace pw
