#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cli.hpp"

namespace pw {
namespace {

const char* cleanup_state_name(uint8_t s) {
    switch (static_cast<CleanupState>(s)) {
        case CleanupState::None: return "none";
        case CleanupState::Ready: return "ready";
        case CleanupState::Clearing: return "clearing";
        case CleanupState::Freed: return "freed";
        case CleanupState::Failed: return "failed";
    }
    return "?";
}

const char* item_status_name(uint8_t s) {
    switch (static_cast<ItemStatus>(s)) {
        case ItemStatus::Ready: return "ready";
        case ItemStatus::Clearing: return "clearing";
        case ItemStatus::Freed: return "freed";
        case ItemStatus::Failed: return "failed";
    }
    return "?";
}

void print_plan(const StatusTable& t) {
    if (t.cleanup.state == 0) {
        std::printf("no cleanup plan\n");
        return;
    }
    std::printf("plan %s  state=%s  items=%d\n", hash_hex(t.cleanup.plan_hash).c_str(),
                cleanup_state_name(t.cleanup.state), t.cleanup.count);
    uint64_t ram = 0;
    uint64_t disk = 0;
    for (int i = 0; i < t.cleanup.count; ++i) {
        const auto& item = t.cleanup.items[i];
        const bool is_ram = item.kind == static_cast<uint8_t>(ItemKind::Ram);
        if (is_ram)
            ram += item.bytes;
        else
            disk += item.bytes;
        std::printf("  %-4s %-40s %10s  %-8s %s\n", is_ram ? "RAM" : "DISK", item.label,
                    fmt_bytes(item.bytes).c_str(), item_status_name(item.status), item.detail);
    }
    std::printf("total: RAM %s, disk %s\n", fmt_bytes(ram).c_str(), fmt_bytes(disk).c_str());
    if (t.cleanup.state == static_cast<uint8_t>(CleanupState::Ready))
        std::printf("approve with: procwarden cleanup apply %s\n",
                    hash_hex(t.cleanup.plan_hash).c_str());
}

}  // namespace

int cmd_cleanup(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: procwarden cleanup <plan|apply|status> [options]\n");
        return 2;
    }

    auto client = Client::connect(true);
    if (!client.ok()) {
        std::fprintf(stderr, "procwarden cleanup: %s\n", client.error().context.c_str());
        return 3;
    }

    const std::string sub = args[0];

    if (sub == "plan") {
        int flags = 0;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--ram-only")
                flags |= 1;
            else if (args[i] == "--disk-only")
                flags |= 2;
            else {
                std::fprintf(stderr, "procwarden cleanup plan: unknown option %s\n",
                             args[i].c_str());
                return 2;
            }
        }
        PwMsg msg = msg_make(OP_PLAN_CLEANUP, kMtypeNormal, 0);
        msg.a = flags;
        auto reply = client.value().request(msg, 30000);
        if (!reply.ok()) {
            std::fprintf(stderr, "procwarden cleanup: %s\n", reply.error().context.c_str());
            return 1;
        }
        if (reply.value().a != kReplyOk) {
            std::fprintf(stderr, "procwarden cleanup: %s\n", msg_text(reply.value()).c_str());
            return 1;
        }
        StatusTable copy {};
        if (client.value().refresh(copy)) print_plan(copy);
        return 0;
    }

    if (sub == "apply") {
        if (args.size() < 2) {
            std::fprintf(stderr, "procwarden cleanup apply: plan hash required\n");
            return 2;
        }
        const uint64_t hash = std::strtoull(args[1].c_str(), nullptr, 16);
        PwMsg msg = msg_make(OP_APPROVE_CLEANUP, kMtypeNormal, 0);
        msg.c = static_cast<int64_t>(hash);
        auto reply = client.value().request(msg, 2000);
        if (!reply.ok()) {
            std::fprintf(stderr, "procwarden cleanup: %s\n", reply.error().context.c_str());
            return 1;
        }
        if (reply.value().a != kReplyOk) {
            std::fprintf(stderr, "procwarden cleanup: %s\n", msg_text(reply.value()).c_str());
            return 1;
        }
        std::printf("%s\n", msg_text(reply.value()).c_str());
        return 0;
    }

    if (sub == "status") {
        StatusTable copy {};
        if (!client.value().refresh(copy)) {
            std::fprintf(stderr, "procwarden cleanup: status table unavailable\n");
            return 1;
        }
        print_plan(copy);
        return 0;
    }

    std::fprintf(stderr, "procwarden cleanup: unknown subcommand '%s'\n", sub.c_str());
    return 2;
}

}  // namespace pw
