#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pw/core/snapshot.hpp"
#include "pw/os/pipe.hpp"
#include "pw/os/proc.hpp"
#include "pw/os/signals.hpp"

namespace pw {
namespace {

void sleep_ms(int ms) {
    struct timespec ts {};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

bool find_start(int32_t pid, uint64_t& sec, uint64_t& usec) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        auto snap = snapshot();
        if (snap.ok()) {
            for (const auto& p : snap.value().procs) {
                if (p.pid != pid) continue;
                sec = p.start_sec;
                usec = p.start_usec;
                return true;
            }
        }
        sleep_ms(50);
    }
    return false;
}

}  // namespace

int cmd_run(const std::vector<std::string>& args) {
    namespace fs = std::filesystem;

    std::string project = fs::current_path().string();
    std::string phase;
    std::string tool = "opencode";
    long budget_mb = 0;
    std::vector<std::string> command;
    bool have_command = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (!have_command) {
            if (arg == "--") {
                have_command = true;
                continue;
            }
            if (arg == "--project" && i + 1 < args.size()) {
                project = args[++i];
                continue;
            }
            if (arg == "--phase" && i + 1 < args.size()) {
                phase = args[++i];
                continue;
            }
            if (arg == "--tool" && i + 1 < args.size()) {
                tool = args[++i];
                continue;
            }
            if (arg == "--budget-mb" && i + 1 < args.size()) {
                budget_mb = std::atol(args[++i].c_str());
                continue;
            }
            if (!arg.empty() && arg[0] == '-') {
                std::fprintf(stderr, "procwarden run: unknown option %s\n", arg.c_str());
                return 2;
            }
            have_command = true;
        }
        command.push_back(arg);
    }
    if (command.empty()) {
        std::fprintf(stderr, "procwarden run: missing command (try: procwarden run -- make test)\n");
        return 2;
    }

    auto cfg_r = Config::load(project);
    const Config cfg = cfg_r.ok() ? cfg_r.value() : Config::defaults();
    const int reply_timeout = cfg.ipc_reply_timeout_ms;

    auto client_r = Client::connect(false);
    if (!client_r.ok()) {
        std::fprintf(stderr,
                     "procwarden run: %s\n"
                     "start the daemon first: procwardend --log ~/.procwarden/warden.log &\n"
                     "or install it: procwarden install\n",
                     client_r.error().context.c_str());
        return 3;
    }
    Client client = std::move(client_r.value());

    PwMsg admit = msg_make(OP_ADMIT_REQ, kMtypeNormal, static_cast<int32_t>(getpid()));
    admit.b = tool == "sim" ? 1 : 0;
    msg_set_text(admit, project);

    int32_t sess_id = 0;
    for (int attempt = 0; attempt < 600 && sess_id == 0; ++attempt) {
        auto reply = client.request(admit, reply_timeout);
        if (!reply.ok()) {
            std::fprintf(stderr, "procwarden run: %s\n", reply.error().context.c_str());
            return 3;
        }
        if (reply.value().a == kReplyAdmit) {
            sess_id = reply.value().b;
            break;
        }
        if (reply.value().a == kReplyQueued) {
            if (attempt == 0)
                std::fprintf(stderr, "procwarden: queued behind active sessions (position %d)\n",
                             reply.value().b);
            sleep_ms(1000);
            continue;
        }
        std::fprintf(stderr, "procwarden run: refused: %s\n", msg_text(reply.value()).c_str());
        return 3;
    }
    if (sess_id == 0) {
        std::fprintf(stderr, "procwarden run: timed out waiting for a session slot\n");
        return 3;
    }

    auto errp = Pipe::create();
    if (!errp.ok()) {
        std::fprintf(stderr, "procwarden run: pipe: %s\n", errp.error().context.c_str());
        return 1;
    }

    const pid_t child = fork();
    if (child < 0) {
        std::fprintf(stderr, "procwarden run: fork: %s\n", std::strerror(errno));
        return 1;
    }

    if (child == 0) {
        setpgid(0, 0);
        errp.value().close_read();

        // The launcher owns the tracking; the child gets a clean signal slate.
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        setenv("PW_SESSION", std::to_string(sess_id).c_str(), 1);
        if (!phase.empty()) setenv("PW_PHASE", phase.c_str(), 1);
        else unsetenv("PW_PHASE");

        std::vector<char*> argv;
        argv.reserve(command.size() + 1);
        for (const auto& part : command) argv.push_back(const_cast<char*>(part.c_str()));
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        const int err = errno;
        errp.value().write_all(&err, sizeof(err));
        _exit(127);
    }

    errp.value().close_write();
    setpgid(child, child);  // both sides set it; EACCES if already done

    int exec_errno = 0;
    auto read_result = errp.value().read_some(&exec_errno, sizeof(exec_errno));
    if (!read_result.ok() || read_result.value() != 0) {
        std::fprintf(stderr, "procwarden run: cannot exec %s: %s\n", command[0].c_str(),
                     std::strerror(exec_errno != 0 ? exec_errno : EIO));
        int status = 0;
        waitpid(child, &status, 0);
        PwMsg exited = msg_make(OP_EXITED, kMtypeNormal, static_cast<int32_t>(getpid()));
        exited.sess_id = sess_id;
        client.request(exited, reply_timeout);
        return 127;
    }

    uint64_t child_sec = 0;
    uint64_t child_usec = 0;
    find_start(child, child_sec, child_usec);

    uint64_t owner_sec = 0;
    uint64_t owner_usec = 0;
    find_start(getpid(), owner_sec, owner_usec);

    PwMsg reg = msg_make(OP_REGISTER, kMtypeNormal, static_cast<int32_t>(getpid()));
    reg.sess_id = sess_id;
    reg.a = static_cast<int32_t>(child);
    reg.root_pid = static_cast<int32_t>(child);
    reg.b = static_cast<int32_t>(child_usec);
    reg.c = static_cast<int64_t>(child_sec);
    msg_set_text(reg, std::to_string(owner_sec) + "," + std::to_string(owner_usec));

    auto registered = client.request(reg, reply_timeout);
    if (!registered.ok() || registered.value().a != kReplyOk) {
        std::fprintf(stderr, "procwarden run: registration failed\n");
        kill(-child, SIGTERM);
        int status = 0;
        waitpid(child, &status, 0);
        return 4;
    }

    if (budget_mb > 0) {
        PwMsg budget = msg_make(OP_SET_BUDGET, kMtypeNormal, static_cast<int32_t>(getpid()));
        budget.sess_id = sess_id;
        budget.c = static_cast<int64_t>(budget_mb) * 1024 * 1024;
        client.request(budget, reply_timeout);
    }

    SignalFlag sig_int;
    SignalFlag sig_term;
    SignalFlag sig_hup;
    SignalFlag sig_quit;
    sig_int.install(SIGINT);
    sig_term.install(SIGTERM);
    sig_hup.install(SIGHUP);
    sig_quit.install(SIGQUIT);

    bool terminating = false;
    uint64_t terminate_at = 0;
    uint64_t last_heartbeat = 0;

    auto now = [] {
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
               static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
    };

    int status = 0;
    while (true) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) break;

        if (sig_int.raised()) {
            sig_int.clear();
            kill(-child, SIGINT);
        }
        if (sig_quit.raised()) {
            sig_quit.clear();
            kill(-child, SIGQUIT);
        }
        if (sig_hup.raised()) {
            sig_hup.clear();
            kill(-child, SIGHUP);
        }
        if (sig_term.raised()) {
            sig_term.clear();
            if (!terminating) {
                terminating = true;
                terminate_at = now();
                kill(-child, SIGTERM);
            }
        }

        const uint64_t t = now();
        if (terminating && t - terminate_at > static_cast<uint64_t>(cfg.grace_ms)) {
            kill(-child, SIGKILL);
            terminating = false;
        }

        if (t - last_heartbeat >= static_cast<uint64_t>(cfg.heartbeat_ms)) {
            last_heartbeat = t;
            PwMsg hb = msg_make(OP_HEARTBEAT, kMtypeNormal, static_cast<int32_t>(getpid()));
            hb.sess_id = sess_id;
            auto reply = client.request(hb, 500);
            if (reply.ok() && reply.value().a == kReplyRefused)
                std::fprintf(stderr, "procwarden: daemon lost session %d\n", sess_id);
        }

        sleep_ms(cfg.poll_ms);
    }

    PwMsg exited = msg_make(OP_EXITED, kMtypeNormal, static_cast<int32_t>(getpid()));
    exited.sess_id = sess_id;
    client.request(exited, reply_timeout);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

}  // namespace pw
