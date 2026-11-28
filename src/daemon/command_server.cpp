#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

#include "daemon.hpp"
#include "pw/log.hpp"
#include "pw/os/ports.hpp"

namespace pw {
namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

void reply(DaemonState& st, const PwMsg& req, int32_t code, const std::string& text,
           int64_t c, int32_t extra) {
    PwMsg r = msg_make(OP_REPLY, static_cast<long>(req.client_pid),
                       static_cast<int32_t>(getpid()));
    r.sess_id = req.sess_id;
    r.a = code;
    r.b = extra;
    r.c = c;
    msg_set_text(r, text);
    auto sent = st.queue.send(&r, sizeof(r), r.mtype, IPC_NOWAIT);
    if (!sent.ok())
        PW_LOG(LogLevel::Warn, "ipc", "-", "reply", "msgsnd: %s",
               sent.error().context.c_str());
}

void handle_message(DaemonState& st, const PwMsg& msg) {
    switch (msg.op) {
        case OP_ADMIT_REQ: {
            LockGuard guard(st.mu);
            const std::string path = msg_text(msg);
            const std::string project = base_name(path);

            int active = 0;
            for (const auto& s : st.sessions)
                if (s.state != SessionState::Done) ++active;

            auto it = std::find(st.admit_queue.begin(), st.admit_queue.end(), msg.client_pid);
            if (it == st.admit_queue.end()) {
                st.admit_queue.push_back(msg.client_pid);
                it = st.admit_queue.end() - 1;
            }
            const int position =
                static_cast<int>(std::distance(st.admit_queue.begin(), it)) + 1;

            if (active >= st.cfg.max_active_sessions || position != 1) {
                reply(st, msg, kReplyQueued, "queued", 0, position);
                return;
            }

            st.admit_queue.erase(st.admit_queue.begin());
            Session s;
            s.id = st.next_session_id++;
            s.tool = msg.b == 1 ? "sim" : "opencode";
            s.project = project;
            s.project_path = path;
            s.owner_pid = msg.client_pid;
            s.state = SessionState::Active;
            st.sessions.push_back(s);
            sync_registry_locked(st);
            PW_LOG(LogLevel::Info, "admit", project.c_str(), "admit", "session %d", s.id);
            reply(st, msg, kReplyAdmit, "admitted", 0, s.id);
            return;
        }

        case OP_REGISTER: {
            LockGuard guard(st.mu);
            Session* s = find_session(st, msg.sess_id);
            if (s == nullptr) {
                reply(st, msg, kReplyRefused, "unknown session");
                return;
            }
            s->pgid = msg.a;
            s->root_pid = msg.root_pid;
            s->root_start_sec = static_cast<uint64_t>(msg.c);
            s->root_start_usec = static_cast<uint64_t>(std::max(msg.b, 0));
            s->owner_pid = msg.client_pid;
            s->last_seen_usec = now_usec();

            const std::string text = msg_text(msg);
            const auto comma = text.find(',');
            if (comma != std::string::npos) {
                s->owner_start_sec = std::strtoull(text.substr(0, comma).c_str(), nullptr, 10);
                s->owner_start_usec =
                    std::strtoull(text.substr(comma + 1).c_str(), nullptr, 10);
            }
            sync_registry_locked(st);
            PW_LOG(LogLevel::Info, "register", s->project.c_str(), "register",
                   "session %d pgid %d root %d", s->id, s->pgid, s->root_pid);
            reply(st, msg, kReplyOk, "registered");
            return;
        }

        case OP_HEARTBEAT: {
            LockGuard guard(st.mu);
            Session* s = find_session(st, msg.sess_id);
            if (s == nullptr) {
                reply(st, msg, kReplyRefused, "unknown session");
                return;
            }
            s->last_seen_usec = now_usec();
            reply(st, msg, kReplyOk, "ok");
            return;
        }

        case OP_EXITED: {
            LockGuard guard(st.mu);
            Session* s = find_session(st, msg.sess_id);
            if (s != nullptr) {
                s->state = SessionState::Done;
                s->owner_pid = 0;
                s->pausable = false;
                sync_registry_locked(st);
                PW_LOG(LogLevel::Info, "exit", s->project.c_str(), "exit", "session %d done",
                       s->id);
            }
            reply(st, msg, kReplyOk, "done");
            return;
        }

        case OP_DETACHED: {
            LockGuard guard(st.mu);
            Session* s = find_session(st, msg.sess_id);
            if (s == nullptr) {
                reply(st, msg, kReplyRefused, "unknown session");
                return;
            }
            s->on_detach = parse_detach(msg_text(msg));
            s->state = SessionState::Detached;
            s->pausable = s->on_detach != DetachPolicy::Keep;
            if (s->on_detach == DetachPolicy::Stop) {
                Job job;
                job.kind = Job::Kind::Pause;
                job.sess_id = s->id;
                enqueue_locked(st, job);
            } else if (s->on_detach == DetachPolicy::StopAfter10m) {
                st.detach_deadline[s->id] = now_usec() + 600000000ull;
            }
            sync_registry_locked(st);
            reply(st, msg, kReplyOk, "detached");
            return;
        }

        case OP_KILL:
        case OP_PAUSE:
        case OP_RESUME:
        case OP_RENICE: {
            LockGuard guard(st.mu);
            Session* s = find_session(st, msg.sess_id);
            if (s == nullptr) {
                reply(st, msg, kReplyRefused, "unknown session");
                return;
            }
            Job job;
            job.sess_id = s->id;
            if (msg.op == OP_KILL)
                job.kind = Job::Kind::Terminate;
            else if (msg.op == OP_PAUSE)
                job.kind = Job::Kind::Pause;
            else if (msg.op == OP_RESUME)
                job.kind = Job::Kind::Resume;
            else {
                job.kind = Job::Kind::Renice;
                job.nice_value = msg.a;
            }
            enqueue_locked(st, job);
            reply(st, msg, kReplyOk, "queued");
            return;
        }

        case OP_SET_BUDGET: {
            LockGuard guard(st.mu);
            Session* s = find_session(st, msg.sess_id);
            if (s == nullptr) {
                reply(st, msg, kReplyRefused, "unknown session");
                return;
            }
            s->budget_bytes = static_cast<uint64_t>(msg.c);
            sync_registry_locked(st);
            reply(st, msg, kReplyOk, "budget set");
            return;
        }

        case OP_ADOPT: {
            LockGuard guard(st.mu);
            const auto parts = split(msg_text(msg), '|');
            if (parts.size() < 4) {
                reply(st, msg, kReplyError, "adopt: tool|project|path|pgid");
                return;
            }
            Session s;
            s.id = st.next_session_id++;
            s.tool = parts[0].empty() ? "opencode" : parts[0];
            s.project = parts[1].empty() ? base_name(parts[2]) : parts[1];
            s.project_path = parts[2];
            s.pgid = std::atoi(parts[3].c_str());
            s.state = SessionState::Detached;
            s.pausable = false;
            st.sessions.push_back(s);
            sync_registry_locked(st);
            reply(st, msg, kReplyAdmit, "adopted", 0, s.id);
            return;
        }

        case OP_PLAN_CLEANUP: {
            const bool all = msg.a == 0;
            const bool ram = all || (msg.a & 1) != 0;
            const bool disk = all || (msg.a & 2) != 0;
            CleanupInput input = build_cleanup_input(st, ram, disk);
            CleanupPlan plan = plan_cleanup(input);
            {
                LockGuard guard(st.mu);
                st.pending.plan = plan;
                st.pending.created_usec = now_usec();
                st.cleanup_visible_hash = plan.hash;
                status_begin_write(st.table);
                fill_status_cleanup(st.table->cleanup, plan);
                status_end_write(st.table);
            }
            PW_LOG(LogLevel::Info, "cleanup", "-", "plan", "%s", plan_summary(plan).c_str());
            reply(st, msg, kReplyOk, plan_summary(plan), static_cast<int64_t>(plan.hash),
                  static_cast<int32_t>(plan.items.size()));
            return;
        }

        case OP_APPROVE_CLEANUP: {
            LockGuard guard(st.mu);
            const uint64_t hash = static_cast<uint64_t>(msg.c);
            if (st.pending.plan.items.empty() || st.pending.plan.hash != hash) {
                reply(st, msg, kReplyStaleHash, "plan changed; replan");
                return;
            }
            status_begin_write(st.table);
            st.table->cleanup.state = static_cast<uint8_t>(CleanupState::Clearing);
            status_end_write(st.table);

            Job job;
            job.kind = Job::Kind::Cleanup;
            job.plan = st.pending.plan;
            enqueue_locked(st, job);
            PW_LOG(LogLevel::Warn, "cleanup", "-", "approve", "hash %016llx approved",
                   static_cast<unsigned long long>(hash));
            reply(st, msg, kReplyOk, "cleanup started");
            return;
        }

        case OP_PORT_LOOKUP: {
            const int port = msg.a;
            auto ports = listening_ports();
            int32_t pid = 0;
            if (ports.ok())
                for (const auto& p : ports.value())
                    if (p.port == port) pid = p.pid;
            if (pid == 0) {
                reply(st, msg, kReplyError, "port not listening");
                return;
            }
            LockGuard guard(st.mu);
            int32_t sess = 0;
            for (const auto& s : st.sessions)
                for (const auto& m : s.members)
                    if (m.pid == pid) sess = s.id;
            reply(st, msg, kReplyOk,
                  "port=" + std::to_string(port) + " pid=" + std::to_string(pid) +
                      " sess=" + std::to_string(sess),
                  pid, sess);
            return;
        }

        case OP_SETTINGS_RELOAD: {
            auto reloaded = Config::load("");
            if (!reloaded.ok()) {
                reply(st, msg, kReplyError, reloaded.error().context);
                return;
            }
            {
                LockGuard guard(st.mu);
                st.cfg = reloaded.value();
                st.rules_cache.clear();
            }
            reply(st, msg, kReplyOk, "reloaded");
            return;
        }

        default:
            reply(st, msg, kReplyError, "unknown op");
            return;
    }
}

void* command_main(void* arg) {
    auto& st = *static_cast<DaemonState*>(arg);
    while (true) {
        {
            LockGuard guard(st.mu);
            if (st.stop) break;
        }

        PwMsg msg {};
        auto got = st.queue.recv(&msg, sizeof(msg), -2, 0);
        if (!got.ok()) {
            if (got.error().code == EINTR) continue;
            {
                LockGuard guard(st.mu);
                if (st.stop) break;
            }
            PW_LOG(LogLevel::Warn, "ipc", "-", "-", "msgrcv: %s",
                   got.error().context.c_str());
            sleep_ms(100);
            continue;
        }
        if (msg.op == OP_SHUTDOWN) break;
        handle_message(st, msg);
    }
    return nullptr;
}

}  // namespace pw
