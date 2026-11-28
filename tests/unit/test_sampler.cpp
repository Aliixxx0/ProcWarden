#include "doctest.h"

#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <filesystem>
#include <fstream>

#include "pw/os/fsize.hpp"
#include "pw/os/mem.hpp"
#include "pw/os/ports.hpp"
#include "pw/os/proc.hpp"

TEST_CASE("snapshot contains this process with a start time") {
    auto r = pw::snapshot();
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().procs.empty());
    CHECK(r.value().t_usec > 0);

    bool found = false;
    for (const auto& p : r.value().procs) {
        if (p.pid != getpid()) continue;
        found = true;
        CHECK(p.ppid == getppid());
        CHECK(p.start_sec > 0);
        CHECK(p.footprint > 0);
    }
    CHECK(found);
}

TEST_CASE("cmdline_env returns argv and the environment") {
    auto snap = pw::snapshot();
    REQUIRE(snap.ok());

    const pw::ProcInfo* me = nullptr;
    for (const auto& p : snap.value().procs)
        if (p.pid == getpid()) me = &p;
    REQUIRE(me != nullptr);

    auto r = pw::cmdline_env(getpid(), me->start_sec, me->start_usec);
    REQUIRE(r.ok());
    CHECK_FALSE(r.value().argv.empty());
    CHECK_FALSE(r.value().argv_joined().empty());
    CHECK_FALSE(r.value().env.empty());

    // Cached by (pid, start time): a second call returns the same data.
    auto r2 = pw::cmdline_env(getpid(), me->start_sec, me->start_usec);
    REQUIRE(r2.ok());
    CHECK(r2.value().argv == r.value().argv);

    // A different start time is a different identity: no stale reuse.
    auto r3 = pw::cmdline_env(getpid(), me->start_sec, me->start_usec + 1);
    REQUIRE(r3.ok());
    CHECK(r3.value().argv == r.value().argv);
}

TEST_CASE("cwd_of returns an existing directory") {
    auto r = pw::cwd_of(getpid());
    REQUIRE(r.ok());
    CHECK(std::filesystem::is_directory(r.value()));
}

TEST_CASE("mem_sample looks reasonable") {
    auto r = pw::mem_sample();
    REQUIRE(r.ok());
    const pw::MemSample& m = r.value();
    CHECK(m.total > 0);
    CHECK(m.page_size > 0);
    CHECK(m.used > 0);
    CHECK(m.used <= m.total);
    CHECK(m.phys_free <= m.total);
    CHECK(m.swap_used <= m.swap_total);
    CHECK(m.disk_total > 0);
    CHECK(m.pressure_level >= 1);
    const bool known_pressure =
        m.pressure_level == 1 || m.pressure_level == 2 || m.pressure_level == 4;
    CHECK(known_pressure);
    MESSAGE("total=" << (m.total >> 30) << " GiB used=" << (m.used >> 30)
                     << " GiB swap=" << (m.swap_used >> 20) << " MiB pressure="
                     << m.pressure_level);
}

TEST_CASE("dir_size sums regular files") {
    const auto root = std::filesystem::temp_directory_path() / "pw_fsize_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "sub");

    {
        std::ofstream(root / "a.bin", std::ios::binary) << std::string(100, 'a');
        std::ofstream(root / "sub" / "b.bin", std::ios::binary) << std::string(250, 'b');
    }

    auto r = pw::dir_size(root.string());
    REQUIRE(r.ok());
    CHECK(r.value() == 350);

    std::filesystem::remove_all(root);
}

TEST_CASE("listening_ports finds our listening socket") {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(fd, 1) == 0);

    socklen_t len = sizeof(addr);
    REQUIRE(getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0);
    const uint16_t port = ntohs(addr.sin_port);
    REQUIRE(port != 0);

    auto r = pw::listening_ports();
    REQUIRE(r.ok());
    bool found = false;
    for (const auto& p : r.value())
        if (p.port == port && p.pid == getpid()) found = true;
    CHECK(found);

    close(fd);
}
