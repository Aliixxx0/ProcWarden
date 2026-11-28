#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cli.hpp"

namespace pw {

int cmd_ctl(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr,
                     "usage: procwarden ctl <kill|pause|resume|renice|budget|adopt> <session>\n");
        return 2;
    }

    const std::string op = args[0];
    bool confirm = false;
    int nice_value = 10;
    long budget_mb = 0;
    std::string tool = "opencode";
    std::string project;
    std::string path;
    std::string pgid;
    std::string target;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--confirm") {
            confirm = true;
        } else if (arg == "--nice" && i + 1 < args.size()) {
            nice_value = std::atoi(args[++i].c_str());
        } else if (arg == "--mb" && i + 1 < args.size()) {
            budget_mb = std::atol(args[++i].c_str());
        } else if (arg == "--tool" && i + 1 < args.size()) {
            tool = args[++i];
        } else if (arg == "--project" && i + 1 < args.size()) {
            project = args[++i];
        } else if (arg == "--path" && i + 1 < args.size()) {
            path = args[++i];
        } else if (arg == "--pgid" && i + 1 < args.size()) {
            pgid = args[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "procwarden ctl: unknown option %s\n", arg.c_str());
            return 2;
        } else {
            target = arg;
        }
    }

    auto client = Client::connect(false);
    if (!client.ok()) {
        std::fprintf(stderr, "procwarden ctl: %s\n", client.error().context.c_str());
        return 3;
    }

    PwMsg msg = msg_make(0, kMtypeNormal, 0);

    if (op == "adopt") {
        if (pgid.empty()) {
            std::fprintf(stderr, "procwarden ctl adopt: --pgid is required\n");
            return 2;
        }
        if (path.empty()) path = ".";
        if (project.empty()) project = path;
        msg = msg_make(OP_ADOPT, kMtypeNormal, 0);
        msg_set_text(msg, tool + "|" + project + "|" + path + "|" + pgid);
    } else {
        if (target.empty()) {
            std::fprintf(stderr, "procwarden ctl: session id required\n");
            return 2;
        }
        const int32_t sess = static_cast<int32_t>(std::atoi(target.c_str()));
        if (sess <= 0) {
            std::fprintf(stderr, "procwarden ctl: bad session id '%s'\n", target.c_str());
            return 2;
        }

        if (op == "kill") {
            if (!confirm) {
                std::fprintf(stderr,
                             "procwarden ctl kill: refusing without --confirm "
                             "(this terminates a session)\n");
                return 2;
            }
            msg = msg_make(OP_KILL, kMtypeNormal, 0);
        } else if (op == "pause") {
            msg = msg_make(OP_PAUSE, kMtypeNormal, 0);
        } else if (op == "resume") {
            msg = msg_make(OP_RESUME, kMtypeNormal, 0);
        } else if (op == "renice") {
            msg = msg_make(OP_RENICE, kMtypeNormal, 0);
            msg.a = nice_value;
        } else if (op == "budget") {
            if (budget_mb <= 0) {
                std::fprintf(stderr, "procwarden ctl budget: --mb N is required\n");
                return 2;
            }
            msg = msg_make(OP_SET_BUDGET, kMtypeNormal, 0);
            msg.c = static_cast<int64_t>(budget_mb) * 1024 * 1024;
        } else {
            std::fprintf(stderr, "procwarden ctl: unknown operation '%s'\n", op.c_str());
            return 2;
        }
        msg.sess_id = sess;
    }

    auto reply = client.value().request(msg, 2000);
    if (!reply.ok()) {
        std::fprintf(stderr, "procwarden ctl: %s\n", reply.error().context.c_str());
        return 1;
    }
    if (reply.value().a == kReplyOk) {
        std::printf("ok: %s\n", msg_text(reply.value()).c_str());
        return 0;
    }
    std::fprintf(stderr, "procwarden ctl: %s\n", msg_text(reply.value()).c_str());
    return 1;
}

}  // namespace pw
