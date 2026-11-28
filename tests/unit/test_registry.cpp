#include "doctest.h"

#include <filesystem>
#include <fstream>

#include "pw/core/registry.hpp"

TEST_CASE("registry save/load round trip") {
    const auto path =
        (std::filesystem::temp_directory_path() / "pw_registry_test.json").string();
    std::filesystem::remove(path);

    pw::Registry reg;
    pw::RegistryEntry e;
    e.sess_id = 3;
    e.pgid = 900;
    e.root_pid = 901;
    e.owner_pid = 800;
    e.root_start_sec = 111;
    e.root_start_usec = 222;
    e.budget_bytes = 1ull << 30;
    e.tool = "opencode";
    e.project = "demo";
    e.project_path = "/tmp/demo";
    e.state = "active";
    e.on_detach = "keep";
    reg.entries.push_back(e);

    auto err = reg.save(path);
    REQUIRE_FALSE(err.has_value());

    auto loaded = pw::Registry::load(path);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().entries.size() == 1);
    const auto& got = loaded.value().entries[0];
    CHECK(got.sess_id == 3);
    CHECK(got.pgid == 900);
    CHECK(got.root_pid == 901);
    CHECK(got.root_start_sec == 111);
    CHECK(got.budget_bytes == 1ull << 30);
    CHECK(got.project == "demo");

    std::filesystem::remove(path);
}

TEST_CASE("missing registry is empty, not an error") {
    const auto path =
        (std::filesystem::temp_directory_path() / "pw_registry_missing.json").string();
    std::filesystem::remove(path);

    auto loaded = pw::Registry::load(path);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().entries.empty());
}

TEST_CASE("identity check compares pid and start time") {
    pw::Snapshot snap;
    pw::ProcInfo p;
    p.pid = 42;
    p.start_sec = 100;
    p.start_usec = 5;
    snap.procs.push_back(p);

    CHECK(pw::identity_alive(snap, 42, 100, 5));
    CHECK_FALSE(pw::identity_alive(snap, 42, 100, 6));  // pid reused
    CHECK_FALSE(pw::identity_alive(snap, 43, 100, 5));
}
