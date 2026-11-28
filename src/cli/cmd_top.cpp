#include <curses.h>

#include <cstdio>
#include <string>
#include <vector>

#include "cli.hpp"
#include "pw/core/snapshot.hpp"

namespace pw {
namespace {

const char* proc_state(uint8_t s) {
    switch (static_cast<ProcState>(s)) {
        case ProcState::Running: return "R";
        case ProcState::Sleeping: return "S";
        case ProcState::Stopped: return "T";
        case ProcState::Zombie: return "Z";
        case ProcState::Other: break;
    }
    return "?";
}

}  // namespace

int cmd_top(const std::vector<std::string>& args) {
    (void)args;

    auto client = Client::connect(true);
    if (!client.ok()) {
        std::fprintf(stderr, "procwarden top: %s\n", client.error().context.c_str());
        return 3;
    }

    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    curs_set(0);

    StatusTable t {};
    bool running = true;
    while (running) {
        const int ch = getch();
        if (ch == 'q' || ch == 'Q') break;

        erase();
        if (!client.value().refresh(t)) {
            mvprintw(0, 0, "waiting for procwardend...");
            refresh();
            napms(500);
            continue;
        }

        const double total_gb =
            static_cast<double>(t.sys.total) / (1024.0 * 1024.0 * 1024.0);
        const double used_gb =
            static_cast<double>(t.sys.used) / (1024.0 * 1024.0 * 1024.0);
        const double swap_gb =
            static_cast<double>(t.sys.swap_used) / (1024.0 * 1024.0 * 1024.0);

        int row = 0;
        mvprintw(row++, 0,
                 "ProcWarden  %-6s  %-7s  used %.1f/%.1f GiB  swap %.1f GiB  procs %u  "
                 "pressure %s   (q to quit)",
                 band_name(static_cast<Band>(t.sys.band)),
                 t.sys.enforce ? "enforce" : "observe", used_gb, total_gb, swap_gb,
                 t.sys.processes, pressure_name(t.sys.pressure_level));
        mvprintw(row++, 0, "");
        mvprintw(row++, 0, "%-7s %-7s %-5s %-6s %-6s %-11s %-11s %s", "PID", "PPID", "SESS",
                 "PHASE", "STATE", "FOOTPRINT", "RSS", "ESC");

        const int proc_rows = LINES - row - 8;
        for (int i = 0; i < kMaxProcs && i < proc_rows; ++i) {
            const auto& p = t.procs[i];
            if (p.pid == 0) break;
            mvprintw(row++, 0, "%-7d %-7d %-5d %-6s %-6s %-11s %-11s %s", p.pid, p.ppid,
                     p.sess_id, phase_name(static_cast<Phase>(p.phase)), proc_state(p.state),
                     fmt_bytes(p.footprint).c_str(), fmt_bytes(p.rss).c_str(),
                     p.escaped ? "escaped" : "");
        }

        int footer = LINES - 7;
        if (footer < row) footer = row;
        mvprintw(footer++, 0, "Sessions:");
        int shown = 0;
        for (const auto& s : t.sessions) {
            if (s.id == 0) continue;
            mvprintw(footer++, 0, "  #%-2d %-9s %-16s %-8s %10s  usual %.1f GB  ratio %.2f",
                     s.id, s.tool, s.project,
                     session_state_name(static_cast<SessionState>(s.state)),
                     fmt_bytes(s.footprint).c_str(), s.usual_gb, s.ratio);
            if (++shown >= 4) break;
        }
        if (shown == 0) mvprintw(footer++, 0, "  (none)");

        refresh();
        napms(500);
    }

    endwin();
    return 0;
}

}  // namespace pw
