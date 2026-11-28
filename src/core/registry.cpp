#include "pw/core/registry.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

namespace pw {

Result<Registry> Registry::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return Registry{};  // first run

    try {
        nlohmann::json j;
        in >> j;
        Registry r;
        auto it = j.find("sessions");
        if (it == j.end() || !it->is_array()) return r;
        for (const auto& e : *it) {
            RegistryEntry entry;
            entry.sess_id = e.value("sess_id", 0);
            entry.pgid = e.value("pgid", 0);
            entry.root_pid = e.value("root_pid", 0);
            entry.owner_pid = e.value("owner_pid", 0);
            entry.root_start_sec = e.value("root_start_sec", 0ull);
            entry.root_start_usec = e.value("root_start_usec", 0ull);
            entry.owner_start_sec = e.value("owner_start_sec", 0ull);
            entry.owner_start_usec = e.value("owner_start_usec", 0ull);
            entry.last_seen_usec = e.value("last_seen_usec", 0ull);
            entry.budget_bytes = e.value("budget_bytes", 0ull);
            entry.tool = e.value("tool", std::string("opencode"));
            entry.project = e.value("project", std::string());
            entry.project_path = e.value("project_path", std::string());
            entry.state = e.value("state", std::string("active"));
            entry.on_detach = e.value("on_detach", std::string("keep"));
            r.entries.push_back(std::move(entry));
        }
        return r;
    } catch (const std::exception& e) {
        return Result<Registry>::err(EINVAL, path + ": " + e.what());
    }
}

std::optional<Error> Registry::save(const std::string& path) const {
    nlohmann::json j;
    j["version"] = 1;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) {
        nlohmann::json o;
        o["sess_id"] = e.sess_id;
        o["pgid"] = e.pgid;
        o["root_pid"] = e.root_pid;
        o["owner_pid"] = e.owner_pid;
        o["root_start_sec"] = e.root_start_sec;
        o["root_start_usec"] = e.root_start_usec;
        o["owner_start_sec"] = e.owner_start_sec;
        o["owner_start_usec"] = e.owner_start_usec;
        o["last_seen_usec"] = e.last_seen_usec;
        o["budget_bytes"] = e.budget_bytes;
        o["tool"] = e.tool;
        o["project"] = e.project;
        o["project_path"] = e.project_path;
        o["state"] = e.state;
        o["on_detach"] = e.on_detach;
        arr.push_back(std::move(o));
    }
    j["sessions"] = std::move(arr);

    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return Error{errno, "open " + tmp};
        out << j.dump(2) << "\n";
        if (!out.good()) return Error{EIO, "write " + tmp};
    }
    chmod(tmp.c_str(), 0600);
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return Error{errno, "rename " + tmp};
    return std::nullopt;
}

bool identity_alive(const Snapshot& snap, int32_t pid, uint64_t start_sec,
                    uint64_t start_usec) {
    for (const auto& p : snap.procs)
        if (p.pid == pid && p.start_sec == start_sec && p.start_usec == start_usec)
            return true;
    return false;
}

}  // namespace pw
