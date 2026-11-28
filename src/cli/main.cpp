#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pw/os/signals.hpp"

namespace {

void usage() {
    std::printf(
        "ProcWarden - memory warden for coding agents\n"
        "\n"
        "usage: procwarden <command> [options]\n"
        "\n"
        "  run -- <command> [args...]   run a command inside a tracked session\n"
        "      --project DIR            project root (default: cwd)\n"
        "      --phase Plan|Build|Test|Review\n"
        "      --tool NAME              tool tag (default: opencode)\n"
        "      --budget-mb N            soft memory budget for the session\n"
        "\n"
        "  init [--shared] [--force]    create .procwarden/ in this project\n"
        "  install                      install the LaunchAgent and detect editors\n"
        "\n"
        "  watch [--json] [--interval MS]   live dashboard from the daemon\n"
        "  status [--json]                  one-line status (works without daemon)\n"
        "  top                              ncurses process view\n"
        "\n"
        "  ctl kill <session> --confirm     terminate a session\n"
        "  ctl pause|resume <session>       SIGSTOP / SIGCONT\n"
        "  ctl renice <session> --nice N\n"
        "  ctl budget <session> --mb N      set a soft budget\n"
        "  ctl adopt --tool T --project P --path DIR --pgid N\n"
        "\n"
        "  cleanup plan [--ram-only|--disk-only]\n"
        "  cleanup apply <plan-hash>\n");
}

}  // namespace

int main(int argc, char** argv) {
    pw::SignalFlag::ignore(SIGPIPE);

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        usage();
        return args.empty() ? 2 : 0;
    }

    const std::string command = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (command == "run") return pw::cmd_run(rest);
    if (command == "init") return pw::cmd_init(rest);
    if (command == "watch") return pw::cmd_watch(rest);
    if (command == "status") return pw::cmd_status(rest);
    if (command == "ctl") return pw::cmd_ctl(rest);
    if (command == "cleanup") return pw::cmd_cleanup(rest);
    if (command == "top") return pw::cmd_top(rest);
    if (command == "install") return pw::cmd_install(rest, argv[0]);

    std::fprintf(stderr, "procwarden: unknown command '%s'\n", command.c_str());
    usage();
    return 2;
}
