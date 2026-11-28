#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pw/core/snapshot.hpp"
#include "pw/result.hpp"

namespace pw {

// Lab 6 / libproc: process snapshot and per-process command line + env tags.
// These are the only places the daemon touches libproc/sysctl for processes.

Result<Snapshot> snapshot();

struct CmdlineEnv {
    std::vector<std::string> argv;
    std::unordered_map<std::string, std::string> env;

    std::string get(const std::string& key) const {
        auto it = env.find(key);
        return it == env.end() ? std::string() : it->second;
    }
    std::string argv_joined() const {
        std::string out;
        for (const auto& a : argv) {
            if (!out.empty()) out.push_back(' ');
            out += a;
        }
        return out;
    }
};

// Cached by (pid, start time) so a re-used pid never reuses stale argv.
Result<CmdlineEnv> cmdline_env(int32_t pid, uint64_t start_sec, uint64_t start_usec);
void clear_cmdline_cache();

// Working directory via PROC_PIDVNODEPATHINFO (session discovery by project).
Result<std::string> cwd_of(int32_t pid);

}  // namespace pw
