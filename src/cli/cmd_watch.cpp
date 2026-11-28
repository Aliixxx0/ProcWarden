#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pw/os/mem.hpp"
#include "pw/os/proc.hpp"
#include "pw/os/signals.hpp"

namespace pw {

int cmd_watch(const std::vector<std::string>& args) {
    bool json = false;
    int interval = 500;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--interval" && i + 1 < args.size()) {
            interval = std::atoi(args[++i].c_str());
            if (interval < 100) interval = 100;
        } else {
            std::fprintf(stderr, "procwarden watch: unknown option %s\n", args[i].c_str());
            return 2;
        }
    }

    auto client = Client::connect(true);
    if (!client.ok()) {
        std::fprintf(stderr, "procwarden watch: %s\n", client.error().context.c_str());
        return 3;
    }

    SignalFlag sig_int;
    SignalFlag sig_term;
    sig_int.install(SIGINT);
    sig_term.install(SIGTERM);

    StatusTable copy {};
    while (!sig_int.raised() && !sig_term.raised()) {
        if (!client.value().refresh(copy)) {
            cli_sleep_ms(interval);
            continue;
        }
        if (json) {
            std::fputs(status_to_json(copy).c_str(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        } else {
            draw_dashboard(copy);
        }
        cli_sleep_ms(interval);
    }
    if (!json) std::printf("\n");
    return 0;
}

int cmd_status(const std::vector<std::string>& args) {
    bool json = false;
    for (const auto& arg : args) {
        if (arg == "--json")
            json = true;
        else {
            std::fprintf(stderr, "procwarden status: unknown option %s\n", arg.c_str());
            return 2;
        }
    }

    auto client = Client::connect(true);
    if (client.ok()) {
        StatusTable copy {};
        if (client.value().refresh(copy)) {
            if (json)
                std::printf("%s\n", status_to_json(copy).c_str());
            else
                print_status_line(copy);
            return 0;
        }
    }

    auto mem = mem_sample();
    auto snap = snapshot();
    if (!mem.ok()) {
        std::fprintf(stderr, "procwarden status: %s\n", mem.error().context.c_str());
        return 1;
    }
    const MemSample& m = mem.value();
    if (json) {
        std::printf(
            "{\"daemon\":false,\"total\":%llu,\"used\":%llu,\"swap_used\":%llu,"
            "\"processes\":%zu,\"pressure\":\"%s\"}\n",
            static_cast<unsigned long long>(m.total), static_cast<unsigned long long>(m.used),
            static_cast<unsigned long long>(m.swap_used),
            snap.ok() ? snap.value().procs.size() : 0, pressure_name(m.pressure_level));
    } else {
        std::printf("daemon: not running\n");
        std::printf("used=%s/%s swap=%s procs=%zu pressure=%s\n", fmt_bytes(m.used).c_str(),
                    fmt_bytes(m.total).c_str(), fmt_bytes(m.swap_used).c_str(),
                    snap.ok() ? snap.value().procs.size() : 0,
                    pressure_name(m.pressure_level));
    }
    return 0;
}

}  // namespace pw
