#include "doctest.h"

#include <string>
#include <vector>

#include "pw/core/cleanup.hpp"
#include "pw/core/sha256.hpp"

namespace {

pw::Member member(int pid, pw::Phase phase, uint64_t footprint, bool idle) {
    pw::Member m;
    m.pid = pid;
    m.start_sec = 11;
    m.start_usec = 22;
    m.phase = phase;
    m.footprint = footprint;
    m.idle = idle;
    return m;
}

pw::Session done_session() {
    pw::Session s;
    s.id = 2;
    s.project = "demo";
    s.state = pw::SessionState::Done;
    s.members.push_back(member(201, pw::Phase::Test, 1ull << 30, true));
    s.members.push_back(member(202, pw::Phase::Test, 1ull << 30, true));
    return s;
}

pw::Session active_session() {
    pw::Session s;
    s.id = 1;
    s.project = "app";
    s.state = pw::SessionState::Active;
    s.members.push_back(member(101, pw::Phase::Test, 2ull << 30, true));
    s.members.push_back(member(102, pw::Phase::Build, 3ull << 30, false));
    return s;
}

}  // namespace

TEST_CASE("sha256 known vectors") {
    CHECK(pw::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(pw::sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("planner includes done sessions and idle lanes only") {
    pw::CleanupInput in;
    in.sessions = {active_session(), done_session()};
    in.include = {"sims", "idle_agents", "leftover_servers", "npm"};

    const auto plan = pw::plan_cleanup(in);

    bool saw_done = false;
    bool saw_idle_test = false;
    bool saw_build = false;
    for (const auto& item : plan.items) {
        if (item.kind != pw::ItemKind::Ram) continue;
        if (item.label.find("demo") != std::string::npos) saw_done = true;
        if (item.label.find("idle Test lane in app") != std::string::npos) saw_idle_test = true;
        if (item.label.find("Build") != std::string::npos) saw_build = true;
    }
    CHECK(saw_done);
    CHECK(saw_idle_test);
    CHECK_FALSE(saw_build);

    // Done session item has both pids; idle lane has only the idle one.
    for (const auto& item : plan.items) {
        if (item.label.find("idle Test") != std::string::npos) {
            REQUIRE(item.pids.size() == 1);
            CHECK(item.pids[0].pid == 101);
        }
    }
}

TEST_CASE("leftover servers are included, managed ports are not") {
    pw::CleanupInput in;
    in.sessions = {active_session()};
    in.include = {"leftover_servers"};
    in.include_ram = true;
    in.include_disk = false;

    pw::PortInfo leftover;
    leftover.port = 3128;
    leftover.pid = 300;
    leftover.sess_id = 0;
    leftover.bytes = 5000;
    in.ports.push_back(leftover);

    pw::PortInfo managed;
    managed.port = 5173;
    managed.pid = 101;
    managed.sess_id = 1;
    in.ports.push_back(managed);

    const auto plan = pw::plan_cleanup(in);
    bool has_leftover = false;
    bool has_managed = false;
    for (const auto& item : plan.items) {
        if (item.label.find(":3128") != std::string::npos) has_leftover = true;
        if (item.label.find(":5173") != std::string::npos) has_managed = true;
    }
    CHECK(has_leftover);
    CHECK_FALSE(has_managed);
}

TEST_CASE("cache classes follow cleanup.include") {
    pw::CleanupInput in;
    in.include = {"npm"};
    in.include_ram = false;
    in.include_disk = true;
    in.caches.push_back({"npm", "/home/u/.npm/_cacache", 1000, "npm cache clean --force"});
    in.caches.push_back({"biome", "/home/u/Library/Caches/biome", 500, ""});
    in.caches.push_back({"pnpm", "/home/u/Library/pnpm/store", 700, "pnpm store prune"});

    const auto plan = pw::plan_cleanup(in);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items[0].label == "npm cache");
    CHECK(plan.disk_bytes == 1000);
    CHECK(plan.ram_bytes == 0);
}

TEST_CASE("plan hash is stable across input order") {
    pw::CleanupInput a;
    a.sessions = {active_session(), done_session()};
    a.include = {"sims", "npm"};
    a.caches.push_back({"npm", "/c", 10, "x"});
    a.ports.push_back({3128, 300, 0, 5, "p"});
    a.ports.push_back({3129, 301, 0, 6, "p"});

    pw::CleanupInput b;
    b.sessions = {done_session(), active_session()};
    b.include = a.include;
    b.caches.push_back({"npm", "/c", 10, "x"});
    b.ports.push_back({3129, 301, 0, 6, "p"});
    b.ports.push_back({3128, 300, 0, 5, "p"});

    const auto pa = pw::plan_cleanup(a);
    const auto pb = pw::plan_cleanup(b);
    CHECK(pa.hash == pb.hash);
    CHECK(pa.ram_bytes == pb.ram_bytes);

    // Changing one byte of the plan changes the hash.
    pw::CleanupInput c = a;
    c.caches[0].bytes = 11;
    CHECK(pw::plan_cleanup(c).hash != pa.hash);
}

TEST_CASE("status cleanup block mirrors the plan") {
    pw::CleanupInput in;
    in.sessions = {done_session()};
    in.include = {"sims"};

    const auto plan = pw::plan_cleanup(in);
    pw::StatusCleanup table;
    pw::fill_status_cleanup(table, plan);

    CHECK(table.plan_hash == plan.hash);
    CHECK(table.state == static_cast<uint8_t>(pw::CleanupState::Ready));
    REQUIRE(table.count >= 1);
    CHECK(table.items[0].kind == static_cast<uint8_t>(pw::ItemKind::Ram));
    CHECK(table.items[0].status == static_cast<uint8_t>(pw::ItemStatus::Ready));
    CHECK(std::string(table.items[0].label).find("demo") != std::string::npos);
}
