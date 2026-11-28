#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <vector>

#include "daemon.hpp"
#include "pw/log.hpp"
#include "pw/os/proc.hpp"

namespace pw {
namespace {

bool safe_pid(int32_t pid) {
    return pid > 1 && pid != static_cast<int32_t>(getpid()) &&
           pid != static_cast<int32_t>(getppid());
}

void signal_session(const Session& s, int sig) {
    const bool can_group = s.pgid > 1 && s.pgid != static_cast<int32_t>(getpgrp()) &&
                           s.pgid != static_cast<int32_t>(getpid());
    if (can_group) kill(-s.pgid, sig);

    for (const auto& m : s.members)
        if (safe_pid(m.pid)) kill(m.pid, sig);
}

void terminate_session(DaemonState& st, const Session& s) {
    auto snap = snapshot();
    std::vector<int32_t> pids;
    if (snap.ok()) {
        for (const auto& m : s.members) {
            if (!safe_pid(m.pid)) continue;
            if (identity_alive(snap.value(), m.pid, m.start_sec, m.start_usec))
                pids.push_back(m.pid);
        }
    }

    signal_session(s, SIGTERM);
    PW_LOG(LogLevel::Warn, "enforce", s.project.c_str(), "SIGTERM",
           "session %d: %zu members (pgid %d)", s.id, pids.size(), s.pgid);

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

    signal_session(s, SIGKILL);
    PW_LOG(LogLevel::Warn, "enforce", s.project.c_str(), "SIGKILL",
           "session %d after %d ms", s.id, st.cfg.grace_ms);
}

void renice_session(const Session& s, int nice_value) {
    for (const auto& m : s.members) {
        if (!safe_pid(m.pid)) continue;
        setpriority(PRIO_PROCESS, static_cast<id_t>(m.pid), nice_value);
    }
}

}  // namespace

void enqueue_locked(DaemonState& st, Job job) {
    st.jobs.push_back(std::move(job));
    st.job_cv.signal();
}

void execute_job(DaemonState& st, Job& job) {
    if (job.kind == Job::Kind::Cleanup) {
        run_cleanup(st, job.plan);
        return;
    }

    Session copy;
    {
        LockGuard guard(st.mu);
        const Session* s = find_session(st, job.sess_id);
        if (s == nullptr) return;
        copy = *s;
    }

    switch (job.kind) {
        case Job::Kind::Terminate:
            terminate_session(st, copy);
            {
                LockGuard guard(st.mu);
                Session* s = find_session(st, job.sess_id);
                if (s != nullptr) {
                    s->state = SessionState::Done;
                    s->owner_pid = 0;
                    s->pausable = false;
                    sync_registry_locked(st);
                }
            }
            break;
        case Job::Kind::Pause:
            signal_session(copy, SIGSTOP);
            PW_LOG(LogLevel::Info, "enforce", copy.project.c_str(), "SIGSTOP",
                   "session %d paused", copy.id);
            break;
        case Job::Kind::Resume:
            signal_session(copy, SIGCONT);
            PW_LOG(LogLevel::Info, "enforce", copy.project.c_str(), "SIGCONT",
                   "session %d resumed", copy.id);
            break;
        case Job::Kind::Renice:
            renice_session(copy, job.nice_value);
            PW_LOG(LogLevel::Info, "enforce", copy.project.c_str(), "renice",
                   "session %d nice=%d", copy.id, job.nice_value);
            break;
        case Job::Kind::Cleanup:
            break;
    }
}

void* enforcer_main(void* arg) {
    auto& st = *static_cast<DaemonState*>(arg);
    while (true) {
        Job job;
        {
            LockGuard guard(st.mu);
            while (st.jobs.empty() && !st.stop) st.job_cv.wait(st.mu);
            if (st.stop) break;
            job = std::move(st.jobs.front());
            st.jobs.pop_front();
        }
        execute_job(st, job);
    }
    return nullptr;
}

}  // namespace pw
