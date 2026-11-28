#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <new>
#include <string>

#include <nlohmann/json.hpp>

#include "daemon.hpp"
#include "pw/core/membership.hpp"
#include "pw/log.hpp"
#include "pw/os/proc.hpp"

namespace pw {

namespace {

std::string state_to_string(SessionState s) { return session_state_name(s); }

}  // namespace

Session* find_session(DaemonState& st, int32_t id) {
    for (auto& s : st.sessions)
        if (s.id == id) return &s;
    return nullptr;
}

const Session* find_session(const DaemonState& st, int32_t id) {
    for (const auto& s : st.sessions)
        if (s.id == id) return &s;
    return nullptr;
}

std::string base_name(const std::string& path) {
    if (path.empty()) return std::string("project");
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const auto pos = p.find_last_of('/');
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

DetachPolicy parse_detach(const std::string& s) {
    if (s == "stop") return DetachPolicy::Stop;
    if (s == "stop10m") return DetachPolicy::StopAfter10m;
    return DetachPolicy::Keep;
}

const char* detach_name(DetachPolicy p) {
    switch (p) {
        case DetachPolicy::Stop: return "stop";
        case DetachPolicy::StopAfter10m: return "stop10m";
        case DetachPolicy::Keep: break;
    }
    return "keep";
}

void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts {};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

uint64_t now_usec() {
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ull +
           static_cast<uint64_t>(tv.tv_usec);
}

void sync_registry_locked(DaemonState& st) {
    Registry reg;
    for (const auto& s : st.sessions) {
        if (s.state == SessionState::Done) continue;
        RegistryEntry e;
        e.sess_id = s.id;
        e.pgid = s.pgid;
        e.root_pid = s.root_pid;
        e.owner_pid = s.owner_pid;
        e.root_start_sec = s.root_start_sec;
        e.root_start_usec = s.root_start_usec;
        e.owner_start_sec = s.owner_start_sec;
        e.owner_start_usec = s.owner_start_usec;
        e.last_seen_usec = s.last_seen_usec;
        e.budget_bytes = s.budget_bytes;
        e.tool = s.tool;
        e.project = s.project;
        e.project_path = s.project_path;
        e.state = state_to_string(s.state);
        e.on_detach = detach_name(s.on_detach);
        reg.entries.push_back(std::move(e));
    }
    st.registry = std::move(reg);
}

void save_registry(DaemonState& st) {
    {
        LockGuard guard(st.mu);
        sync_registry_locked(st);
    }
    if (auto e = st.registry.save(st.registry_path))
        PW_LOG(LogLevel::Warn, "registry", "-", "-", "save failed: %s", e->context.c_str());
}

void request_stop(DaemonState& st) {
    {
        LockGuard guard(st.mu);
        st.stop = true;
        st.job_cv.broadcast();
    }
    // Wake the command thread out of its blocking msgrcv(-2, 0).
    PwMsg wake = msg_make(OP_SHUTDOWN, kMtypeUrgent, static_cast<int32_t>(getpid()));
    st.queue.send(&wake, sizeof(wake), wake.mtype, IPC_NOWAIT);
}

}  // namespace pw

int main(int argc, char** argv) {
    using namespace pw;

    std::string log_path = procwarden_home() + "/warden.log";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--log" && i + 1 < argc) log_path = argv[++i];
    }

    log_open(log_path);
    PW_LOG(LogLevel::Info, "daemon", "-", "-", "starting pid=%d", getpid());

    std::error_code ec;
    std::filesystem::create_directories(procwarden_home(), ec);
    std::filesystem::create_directories(procwarden_home() + "/history", ec);

    // Single instance per user.
    const std::string lock_path = procwarden_home() + "/daemon.lock";
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd < 0) {
        std::fprintf(stderr, "procwardend: cannot open %s: %s\n", lock_path.c_str(),
                     std::strerror(errno));
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "procwardend: already running\n");
        close(lock_fd);
        return 0;
    }

    DaemonState st;
    auto cfg = Config::load("");
    if (!cfg.ok()) {
        std::fprintf(stderr, "procwardend: config: %s\n", cfg.error().context.c_str());
        return 1;
    }
    st.cfg = cfg.value();
    st.registry_path = procwarden_home() + "/registry.json";
    st.history_dir = procwarden_home() + "/history";

    auto keys = ipc_keys();
    if (!keys.ok()) {
        std::fprintf(stderr, "procwardend: ipc keys: %s\n", keys.error().context.c_str());
        return 1;
    }
    st.keys = keys.value();

    // Fresh queue and table each start; stale messages must not survive a
    // daemon restart.
    if (auto old = MsgQueue::open(st.keys.queue_key); old.ok()) old.value().remove();
    auto q = MsgQueue::create(st.keys.queue_key, 0600);
    if (!q.ok()) {
        std::fprintf(stderr, "procwardend: msgget: %s\n", q.error().context.c_str());
        return 1;
    }
    st.queue = std::move(q.value());

    if (auto old = ShmSegment::open(st.keys.shm_key); old.ok()) old.value().remove();
    auto shm = ShmSegment::create(st.keys.shm_key, sizeof(StatusTable), 0600);
    if (!shm.ok()) {
        std::fprintf(stderr, "procwardend: shmget: %s\n", shm.error().context.c_str());
        return 1;
    }
    st.shm = std::move(shm.value());
    auto mapped = st.shm.attach(false);
    if (!mapped.ok()) {
        std::fprintf(stderr, "procwardend: shmat: %s\n", mapped.error().context.c_str());
        return 1;
    }
    st.table = new (mapped.value()) StatusTable();
    st.table->header.magic = kStatusMagic;
    st.table->header.version = kStatusVersion;
    st.table->header.seq = 0;

    auto reg = Registry::load(st.registry_path);
    if (reg.ok()) {
        st.registry = reg.value();
    } else {
        PW_LOG(LogLevel::Warn, "registry", "-", "-", "load failed: %s",
               reg.error().context.c_str());
    }

    for (const auto& e : st.registry.entries) {
        Session s;
        s.id = e.sess_id;
        s.tool = e.tool;
        s.project = e.project;
        s.project_path = e.project_path;
        s.pgid = e.pgid;
        s.root_pid = e.root_pid;
        s.root_start_sec = e.root_start_sec;
        s.root_start_usec = e.root_start_usec;
        s.owner_pid = e.owner_pid;
        s.owner_start_sec = e.owner_start_sec;
        s.owner_start_usec = e.owner_start_usec;
        s.budget_bytes = e.budget_bytes;
        s.state = e.state == "detached" ? SessionState::Detached : SessionState::Active;
        s.on_detach = parse_detach(e.on_detach);
        s.pausable = s.on_detach != DetachPolicy::Keep;
        st.sessions.push_back(std::move(s));
        if (e.sess_id >= st.next_session_id) st.next_session_id = e.sess_id + 1;
    }

    load_history(st);

    st.sig_term.install(SIGTERM);
    st.sig_int.install(SIGINT);
    st.sig_hup.install(SIGHUP);
    SignalFlag::ignore(SIGPIPE);

    if (!st.monitor_thread.start(monitor_main, &st) ||
        !st.command_thread.start(command_main, &st) ||
        !st.enforcer_thread.start(enforcer_main, &st)) {
        std::fprintf(stderr, "procwardend: cannot start threads\n");
        request_stop(st);
        st.monitor_thread.join();
        st.command_thread.join();
        st.enforcer_thread.join();
        st.queue.remove();
        st.shm.remove();
        return 1;
    }

    PW_LOG(LogLevel::Info, "daemon", "-", "-", "monitoring %zu restored sessions",
           st.sessions.size());

    while (!st.sig_term.raised() && !st.sig_int.raised()) {
        if (st.sig_hup.raised()) {
            st.sig_hup.clear();
            auto reloaded = Config::load("");
            if (reloaded.ok()) {
                LockGuard guard(st.mu);
                st.cfg = reloaded.value();
                st.rules_cache.clear();
            }
            clear_cmdline_cache();
            PW_LOG(LogLevel::Info, "daemon", "-", "-", "settings reloaded");
        }
        sleep_ms(100);
    }

    PW_LOG(LogLevel::Info, "daemon", "-", "-", "stopping");
    request_stop(st);
    st.monitor_thread.join();
    st.command_thread.join();
    st.enforcer_thread.join();

    save_registry(st);
    st.shm.remove();
    st.queue.remove();
    log_close();
    close(lock_fd);
    return 0;
}
