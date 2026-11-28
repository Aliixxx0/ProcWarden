#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "daemon.hpp"
#include "pw/log.hpp"
#include "pw/os/fsize.hpp"
#include "pw/os/pipe.hpp"
#include "pw/os/ports.hpp"
#include "pw/os/proc.hpp"

namespace pw {
namespace {

bool safe_pid(int32_t pid) {
    return pid > 1 && pid != static_cast<int32_t>(getpid()) &&
           pid != static_cast<int32_t>(getppid());
}

bool remove_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) return false;

    bool ok = true;
    while (struct dirent* ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        const std::string child = path + "/" + name;

        struct stat st {};
        if (lstat(child.c_str(), &st) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!remove_recursive(child)) ok = false;
            if (rmdir(child.c_str()) != 0) ok = false;
        } else if (unlink(child.c_str()) != 0) {
            ok = false;  // symlinks are unlinked, never followed
        }
    }
    closedir(dir);
    return ok;
}

bool remove_tree_safe(const std::string& path, const std::string& home) {
    char resolved[PATH_MAX] = {};
    if (realpath(path.c_str(), resolved) == nullptr) return false;
    const std::string r(resolved);
    const std::string h(home);
    if (r.size() <= h.size() || r.compare(0, h.size(), h) != 0 || r[h.size()] != '/')
        return false;
    if (!remove_recursive(r)) return false;
    return rmdir(r.c_str()) == 0;
}

int run_shell(const std::string& command, std::string& output) {
    auto pipe_r = Pipe::create();
    if (!pipe_r.ok()) return -1;

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        pipe_r.value().close_read();
        dup2(pipe_r.value().write_fd(), STDOUT_FILENO);
        dup2(pipe_r.value().write_fd(), STDERR_FILENO);
        pipe_r.value().close_write();
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    pipe_r.value().close_write();
    char buf[1024];
    for (;;) {
        auto got = pipe_r.value().read_some(buf, sizeof(buf));
        if (!got.ok() || got.value() == 0) break;
        const std::size_t room = 4096 - output.size();
        if (room > 0) output.append(buf, std::min(got.value(), room));
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

bool terminate_refs(DaemonState& st, const std::vector<ProcRef>& refs) {
    auto snap = snapshot();
    std::vector<int32_t> pids;
    for (const auto& ref : refs) {
        if (!safe_pid(ref.pid)) continue;
        if (ref.start_sec != 0) {
            if (!snap.ok() ||
                !identity_alive(snap.value(), ref.pid, ref.start_sec, ref.start_usec))
                continue;
        }
        pids.push_back(ref.pid);
        kill(ref.pid, SIGTERM);
    }
    if (pids.empty()) return true;

    const uint64_t grace =
        static_cast<uint64_t>(std::max(st.cfg.grace_ms, 0)) * 1000ull;
    const uint64_t deadline = now_usec() + grace;
    while (now_usec() < deadline) {
        bool any_alive = false;
        for (int32_t pid : pids)
            if (kill(pid, 0) == 0) any_alive = true;
        if (!any_alive) break;
        sleep_ms(100);
    }
    for (int32_t pid : pids)
        if (kill(pid, 0) == 0) kill(pid, SIGKILL);
    return true;
}

void set_item_status(DaemonState& st, std::size_t index, ItemStatus status) {
    LockGuard guard(st.mu);
    if (index >= st.table->cleanup.count) return;
    status_begin_write(st.table);
    st.table->cleanup.items[index].status = static_cast<uint8_t>(status);
    status_end_write(st.table);
}

}  // namespace

std::vector<CacheInfo> measure_caches(const Config& cfg) {
    (void)cfg;
    const std::string home = home_dir();
    std::vector<CacheInfo> out;

    struct Candidate {
        const char* label;
        std::string path;
        const char* command;
    };
    const std::vector<Candidate> candidates = {
        {"npm", home + "/.npm/_cacache", "npm cache clean --force"},
        {"pnpm", home + "/Library/pnpm/store", "pnpm store prune"},
        {"bun", home + "/.bun/install/cache", "bun pm cache rm"},
        {"biome", home + "/Library/Caches/com.biomejs.biome", ""},
    };

    for (const auto& c : candidates) {
        auto size = dir_size(c.path);
        if (!size.ok() || size.value() == 0) continue;
        out.push_back(CacheInfo{c.label, c.path, size.value(), c.command});
    }
    return out;
}

CleanupInput build_cleanup_input(DaemonState& st, bool include_ram, bool include_disk) {
    CleanupInput in;
    {
        LockGuard guard(st.mu);
        in.sessions = st.sessions;
        in.include = st.cfg.cleanup_include;
    }
    in.include_ram = include_ram;
    in.include_disk = include_disk;

    auto snap = snapshot();
    auto ports = listening_ports();
    if (ports.ok()) {
        for (const auto& p : ports.value()) {
            PortInfo info;
            info.port = p.port;
            info.pid = p.pid;
            for (const auto& s : in.sessions)
                for (const auto& m : s.members)
                    if (m.pid == p.pid) {
                        info.sess_id = s.id;
                        info.project = s.project;
                    }
            if (snap.ok())
                for (const auto& proc : snap.value().procs)
                    if (proc.pid == p.pid) info.bytes = proc.footprint;
            in.ports.push_back(std::move(info));
        }
    }

    in.caches = measure_caches(st.cfg);
    return in;
}

void run_cleanup(DaemonState& st, const CleanupPlan& plan) {
    {
        LockGuard guard(st.mu);
        status_begin_write(st.table);
        st.table->cleanup.state = static_cast<uint8_t>(CleanupState::Clearing);
        status_end_write(st.table);
    }

    bool all_ok = true;
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        const CleanupItem& item = plan.items[i];
        set_item_status(st, i, ItemStatus::Clearing);

        bool ok = false;
        if (item.kind == ItemKind::Ram) {
            ok = terminate_refs(st, item.pids);
        } else if (!item.command.empty()) {
            std::string output;
            ok = run_shell(item.command, output) == 0;
            if (!output.empty())
                PW_LOG(LogLevel::Info, "cleanup", item.label.c_str(), "command", "%s",
                       output.substr(0, 200).c_str());
        } else {
            ok = remove_tree_safe(item.detail, home_dir());
        }

        all_ok = all_ok && ok;
        set_item_status(st, i, ok ? ItemStatus::Freed : ItemStatus::Failed);
        PW_LOG(ok ? LogLevel::Info : LogLevel::Warn, "cleanup", item.label.c_str(),
               ok ? "freed" : "failed", "%llu bytes",
               static_cast<unsigned long long>(item.bytes));
    }

    {
        LockGuard guard(st.mu);
        status_begin_write(st.table);
        st.table->cleanup.state =
            static_cast<uint8_t>(all_ok ? CleanupState::Freed : CleanupState::Failed);
        status_end_write(st.table);
        st.pending = PendingPlan{};
    }
    PW_LOG(all_ok ? LogLevel::Info : LogLevel::Warn, "cleanup", "-", "done", "%s",
           plan_summary(plan).c_str());
}

}  // namespace pw
