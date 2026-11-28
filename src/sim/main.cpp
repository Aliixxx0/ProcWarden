#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_leak = 0;

void on_stop(int) { g_stop = 1; }
void on_leak(int) { g_leak = 1; }

constexpr long kMiB = 1024L * 1024L;

struct Report {
    int32_t pid;
    int32_t mb;
    int32_t ok;
    int32_t port;
};

struct WorkerSpec {
    long mb = 0;
    std::string phase;
    int leak_mb = 1;
    bool escape = false;
    int port = -1;  // -1 = no listener, 0 = ephemeral
};

void sleep_ms(int ms) {
    struct timespec ts {};
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

uint64_t now_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}

int make_listener(int requested_port, int& actual_port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(requested_port));
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0)
        actual_port = ntohs(addr.sin_port);
    return fd;
}

void worker(const WorkerSpec& spec, int report_fd) {
    if (spec.escape) setsid();
    if (!spec.phase.empty()) setenv("PW_PHASE", spec.phase.c_str(), 1);

    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGUSR1, on_leak);

    const std::size_t bytes = static_cast<std::size_t>(spec.mb) * static_cast<std::size_t>(kMiB);
    char* mem = static_cast<char*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    bool ok = mem != MAP_FAILED;
    if (ok) {
        for (std::size_t off = 0; off < bytes; off += 4096) mem[off] = 1;
    }

    int port = 0;
    int listen_fd = -1;
    if (spec.port >= 0) {
        listen_fd = make_listener(spec.port, port);
        if (listen_fd >= 0) {
            const int flags = fcntl(listen_fd, F_GETFL, 0);
            fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    Report report {static_cast<int32_t>(getpid()), static_cast<int32_t>(spec.mb), ok ? 1 : 0,
                   port};
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(sizeof(report))) {
        const ssize_t n = write(report_fd, reinterpret_cast<char*>(&report) + written,
                                sizeof(report) - static_cast<std::size_t>(written));
        if (n <= 0) break;
        written += n;
    }
    close(report_fd);

    std::vector<void*> leaks;
    uint64_t last_leak = now_ms();
    while (!g_stop) {
        if (g_leak && now_ms() - last_leak >= 1000) {
            last_leak = now_ms();
            const std::size_t chunk = static_cast<std::size_t>(spec.leak_mb) *
                                      static_cast<std::size_t>(kMiB);
            void* p = mmap(nullptr, chunk, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                for (std::size_t off = 0; off < chunk; off += 4096)
                    static_cast<char*>(p)[off] = 1;
                leaks.push_back(p);
            }
        }
        if (listen_fd >= 0) {
            for (;;) {
                const int client = accept(listen_fd, nullptr, nullptr);
                if (client < 0) break;
                close(client);
            }
        }
        sleep_ms(100);
    }

    if (listen_fd >= 0) close(listen_fd);
    for (void* p : leaks) munmap(p, static_cast<std::size_t>(spec.leak_mb) *
                                       static_cast<std::size_t>(kMiB));
    if (ok) munmap(mem, bytes);
    _exit(0);
}

struct Options {
    long mb = 0;
    double scale = 1.0;
    int fillers = -1;
    int leak_mb = 1;
    int leak_after_s = 0;
    bool escape = false;
    bool linger = true;
    int port = -1;
    std::string phase;
    std::string profile;
};

void usage() {
    std::printf(
        "pw-sim - memory workload simulator\n"
        "\n"
        "  pw-sim app [--mb N] [--leak-mb N] [--leak-after S] [--escape] [--exit]\n"
        "  pw-sim agent --phase Plan|Build|Test|Review [--mb N] ...\n"
        "  pw-sim devserver [--port N] [--mb N] ...\n"
        "  pw-sim watcher [--mb N] ...\n"
        "  pw-sim demo [--scale F] [--fillers N] [--leak-after S] [--exit]\n"
        "\n"
        "SIGUSR1 starts the leak, SIGTERM makes the whole tree join.\n");
}

bool parse(const std::vector<std::string>& args, Options& opts) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "pw-sim: %s needs a value\n", name);
                return nullptr;
            }
            return args[++i].c_str();
        };
        if (arg == "--mb") {
            const char* v = next("--mb");
            if (v == nullptr) return false;
            opts.mb = std::atol(v);
        } else if (arg == "--scale") {
            const char* v = next("--scale");
            if (v == nullptr) return false;
            opts.scale = std::atof(v);
        } else if (arg == "--fillers") {
            const char* v = next("--fillers");
            if (v == nullptr) return false;
            opts.fillers = std::atoi(v);
        } else if (arg == "--leak-mb") {
            const char* v = next("--leak-mb");
            if (v == nullptr) return false;
            opts.leak_mb = std::atoi(v);
        } else if (arg == "--leak-after") {
            const char* v = next("--leak-after");
            if (v == nullptr) return false;
            opts.leak_after_s = std::atoi(v);
        } else if (arg == "--port") {
            const char* v = next("--port");
            if (v == nullptr) return false;
            opts.port = std::atoi(v);
        } else if (arg == "--phase") {
            const char* v = next("--phase");
            if (v == nullptr) return false;
            opts.phase = v;
        } else if (arg == "--escape") {
            opts.escape = true;
        } else if (arg == "--exit") {
            opts.linger = false;
        } else if (arg == "--threads") {
            const char* v = next("--threads");
            if (v == nullptr) return false;
            (void)v;  // accepted for compatibility; workers are processes
        } else if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "pw-sim: unknown option %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

long scaled(long mb, double scale) {
    const long v = static_cast<long>(static_cast<double>(mb) * scale);
    return v < 1 ? 1 : v;
}

std::vector<WorkerSpec> build_specs(const Options& opts) {
    std::vector<WorkerSpec> specs;
    auto add = [&](long mb, const std::string& phase, int port) {
        WorkerSpec s;
        s.mb = scaled(mb, opts.scale);
        s.phase = phase;
        s.leak_mb = opts.leak_mb;
        s.port = port;
        s.escape = false;
        specs.push_back(s);
    };

    if (opts.profile == "app") {
        add(opts.mb > 0 ? opts.mb : 1200, "", -1);
    } else if (opts.profile == "agent") {
        add(opts.mb > 0 ? opts.mb : 400, opts.phase.empty() ? "Build" : opts.phase, -1);
    } else if (opts.profile == "devserver") {
        add(opts.mb > 0 ? opts.mb : 80, "", opts.port < 0 ? 5173 : opts.port);
    } else if (opts.profile == "watcher") {
        add(opts.mb > 0 ? opts.mb : 40, "", -1);
    } else {
        // demo: four agent lanes, six apps, three dev servers, one watcher.
        add(1400, "Plan", -1);
        add(3200, "Build", -1);
        add(1600, "Test", -1);
        add(1000, "Review", -1);
        for (int i = 0; i < 6; ++i) add(900, "", -1);
        add(100, "", 5173);
        add(100, "", 3000);
        add(100, "", 8080);
        add(50, "", -1);

        const int fillers = opts.fillers >= 0
                                ? opts.fillers
                                : static_cast<int>(398.0 * opts.scale);
        for (int i = 0; i < fillers; ++i) add(1, "", -1);
    }

    if (opts.escape && !specs.empty()) specs.front().escape = true;
    return specs;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    Options opts;
    opts.profile = argv[1];
    if (opts.profile == "help" || opts.profile == "--help" || opts.profile == "-h") {
        usage();
        return 0;
    }
    const std::vector<std::string> args(argv + 2, argv + argc);
    if (!parse(args, opts)) return 2;

    if (opts.profile != "app" && opts.profile != "agent" && opts.profile != "devserver" &&
        opts.profile != "watcher" && opts.profile != "demo") {
        std::fprintf(stderr, "pw-sim: unknown profile '%s'\n", opts.profile.c_str());
        usage();
        return 2;
    }

    std::vector<WorkerSpec> specs = build_specs(opts);

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        std::fprintf(stderr, "pw-sim: pipe: %s\n", std::strerror(errno));
        return 1;
    }

    std::vector<pid_t> pids;
    pids.reserve(specs.size());
    for (const auto& spec : specs) {
        const pid_t pid = fork();
        if (pid < 0) {
            std::fprintf(stderr, "pw-sim: fork: %s\n", std::strerror(errno));
            break;
        }
        if (pid == 0) {
            close(pipe_fds[0]);
            worker(spec, pipe_fds[1]);
        }
        pids.push_back(pid);
    }
    close(pipe_fds[1]);

    long total_mb = 0;
    int ready = 0;
    std::string ports;
    Report report {};
    std::size_t have = 0;
    while (have < sizeof(report)) {
        const ssize_t n = read(pipe_fds[0], reinterpret_cast<char*>(&report) + have,
                               sizeof(report) - have);
        if (n <= 0) break;
        have += static_cast<std::size_t>(n);
        if (have == sizeof(report)) {
            have = 0;
            ++ready;
            if (report.ok) total_mb += report.mb;
            if (report.port > 0) {
                if (!ports.empty()) ports += ",";
                ports += std::to_string(report.port);
            }
        }
    }
    close(pipe_fds[0]);

    std::printf("READY pid=%d workers=%d mb=%ld procs=%zu", getpid(), ready, total_mb,
                pids.size());
    if (!ports.empty()) std::printf(" ports=%s", ports.c_str());
    std::printf("\n");
    std::fflush(stdout);

    if (!opts.linger) return 0;

    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGUSR1, on_leak);

    bool leak_sent = false;
    const uint64_t start = now_ms();
    while (!g_stop) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {
        }

        if (!leak_sent && opts.leak_after_s > 0 &&
            now_ms() - start >= static_cast<uint64_t>(opts.leak_after_s) * 1000ull) {
            leak_sent = true;
            for (pid_t pid : pids) kill(pid, SIGUSR1);
        }
        if (g_leak && !leak_sent) {
            leak_sent = true;
            for (pid_t pid : pids) kill(pid, SIGUSR1);
        }
        sleep_ms(100);
    }

    for (pid_t pid : pids) kill(pid, SIGTERM);
    for (pid_t pid : pids) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    std::printf("STOPPED\n");
    return 0;
}
