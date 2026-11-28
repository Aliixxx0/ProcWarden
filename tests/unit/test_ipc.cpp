#include "doctest.h"

#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "pw/ipc/protocol.hpp"
#include "pw/ipc/status_table.hpp"
#include "pw/ipc/stream.hpp"

TEST_CASE("IPC structs have fixed legal sizes") {
    static_assert(sizeof(pw::PwMsg) - sizeof(long) <= 248);
    static_assert(sizeof(pw::StatusTable) < 1024 * 1024);
    static_assert(std::is_trivially_copyable_v<pw::PwMsg>);
    static_assert(std::is_trivially_copyable_v<pw::StatusTable>);
    CHECK(sizeof(pw::PwMsg) <= 256);
}

TEST_CASE("message text helpers truncate safely") {
    pw::PwMsg m = pw::msg_make(pw::OP_REGISTER, pw::kMtypeNormal, 42);
    pw::msg_set_text(m, "hello");
    CHECK(pw::msg_text(m) == "hello");
    CHECK(m.client_pid == 42);

    std::string huge(pw::kMsgText + 100, 'x');
    pw::msg_set_text(m, huge);
    CHECK(std::strlen(m.text) == pw::kMsgText - 1);
}

TEST_CASE("seqlock gives consistent copies") {
    pw::StatusTable t {};
    t.header.magic = pw::kStatusMagic;

    pw::status_begin_write(&t);
    t.sys.used = 123;
    pw::status_end_write(&t);

    pw::StatusTable out {};
    REQUIRE(pw::status_copy(&t, &out));
    CHECK(out.sys.used == 123);

    // A write in progress (odd seq) must be retried/fail.
    pw::status_begin_write(&t);
    t.sys.used = 456;
    pw::StatusTable torn {};
    CHECK_FALSE(pw::status_copy(&t, &torn));
    pw::status_end_write(&t);

    REQUIRE(pw::status_copy(&t, &torn));
    CHECK(torn.sys.used == 456);
}

TEST_CASE("JSON stream mirrors the status table") {
    pw::StatusTable t {};
    t.header.t_usec = 1732712345500000LL;
    t.sys.total = 16ull * 1024 * 1024 * 1024;
    t.sys.used = 14ull * 1024 * 1024 * 1024;
    t.sys.swap_used = 3891ull * 1024 * 1024;
    t.sys.processes = 412;
    t.sys.pressure_level = 2;
    t.sys.band = static_cast<uint8_t>(pw::Band::Red);
    t.normal.low_gb = 5.0;
    t.normal.high_gb = 9.0;

    pw::StatusSession& s = t.sessions[0];
    s.id = 1;
    pw::status_set_str(s.tool, sizeof(s.tool), "opencode");
    pw::status_set_str(s.project, sizeof(s.project), "my-saas-app");
    s.footprint = static_cast<uint64_t>(7.7 * 1024 * 1024 * 1024);
    s.usual_gb = 4.5;
    s.ratio = 1.7;

    pw::StatusAgent& a = t.agents[0];
    a.sess_id = 1;
    a.phase = static_cast<uint8_t>(pw::Phase::Test);
    a.status = 2;
    a.footprint = static_cast<uint64_t>(1.4 * 1024 * 1024 * 1024);
    a.idle_bytes = static_cast<uint64_t>(2.1 * 1024 * 1024 * 1024);

    pw::StatusCleanupItem& it = t.cleanup.items[0];
    it.kind = static_cast<uint8_t>(pw::ItemKind::Ram);
    it.status = static_cast<uint8_t>(pw::ItemStatus::Ready);
    pw::status_set_str(it.label, sizeof(it.label), "6 app simulations");
    it.bytes = 4939212390ull;
    t.cleanup.count = 1;
    t.cleanup.plan_hash = 0xabcdefull;

    const std::string line = pw::status_to_json(t);
    const auto j = nlohmann::json::parse(line);

    CHECK(j["processes"] == 412);
    CHECK(j["band"] == "RED");
    CHECK(j["pressure"] == "warning");
    CHECK(j["normal"]["low_gb"] == doctest::Approx(5.0));
    CHECK(j["ram"]["total_gb"] == doctest::Approx(16.0));
    CHECK(j["swap"]["used_mb"] == doctest::Approx(3891.0));
    CHECK(j["sessions"][0]["tool"] == "opencode");
    CHECK(j["sessions"][0]["gb"] == doctest::Approx(7.7));
    CHECK(j["agents"][0]["phase"] == "Test");
    CHECK(j["agents"][0]["status"] == "done");
    CHECK(j["cleanup"]["ram_gb"] == doctest::Approx(4.6).epsilon(0.01));
    CHECK(j["cleanup"]["items"][0]["label"] == "6 app simulations");
}
