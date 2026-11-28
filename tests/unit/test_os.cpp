#include "doctest.h"

#include <signal.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "pw/ipc/protocol.hpp"
#include "pw/os/msgq.hpp"
#include "pw/os/pipe.hpp"
#include "pw/os/shm.hpp"
#include "pw/os/signals.hpp"
#include "pw/os/sync.hpp"
#include "pw/os/thread.hpp"

namespace {

struct SumCtx {
    int a;
    int b;
};

// Lab 4: context through void*, result as a heap pointer.
void* sum_worker(void* arg) {
    auto* ctx = static_cast<SumCtx*>(arg);
    return new int(ctx->a + ctx->b);
}

struct WaitCtx {
    pw::Mutex* mu;
    pw::CondVar* cv;
    bool* ready;
};

void* wait_worker(void* arg) {
    auto* ctx = static_cast<WaitCtx*>(arg);
    pw::LockGuard guard(*ctx->mu);
    while (!*ctx->ready) ctx->cv->wait(*ctx->mu);
    return new int(7);
}

}  // namespace

TEST_CASE("Lab 4: pthread context struct and heap result") {
    pw::PosixThread t;
    SumCtx ctx{20, 22};
    REQUIRE(t.start(sum_worker, &ctx));
    CHECK(t.joinable());

    void* raw = t.join();
    REQUIRE(raw != nullptr);
    auto* result = static_cast<int*>(raw);
    CHECK(*result == 42);
    delete result;
}

TEST_CASE("Lab 4: mutex + condition variable handshake") {
    pw::Mutex mu;
    pw::CondVar cv;
    bool ready = false;

    pw::PosixThread t;
    WaitCtx ctx{&mu, &cv, &ready};
    REQUIRE(t.start(wait_worker, &ctx));

    {
        pw::LockGuard guard(mu);
        ready = true;
        cv.signal();
    }

    void* raw = t.join();
    REQUIRE(raw != nullptr);
    auto* result = static_cast<int*>(raw);
    CHECK(*result == 7);
    delete result;
}

TEST_CASE("Lab 5: pipe round trip") {
    auto p = pw::Pipe::create();
    REQUIRE(p.ok());

    const std::string msg = "procwarden";
    auto wrote = p.value().write_all(msg.data(), msg.size());
    REQUIRE(wrote.ok());
    CHECK(wrote.value() == msg.size());

    char buf[32] = {};
    auto got = p.value().read_some(buf, sizeof(buf));
    REQUIRE(got.ok());
    CHECK(got.value() == msg.size());
    CHECK(std::string(buf, got.value()) == msg);
}

TEST_CASE("Lab 5: pipe + fork, unused ends closed") {
    auto p = pw::Pipe::create();
    REQUIRE(p.ok());

    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        p.value().close_read();  // child writes only
        const char payload[] = "child";
        p.value().write_all(payload, sizeof(payload) - 1);
        _exit(0);
    }

    p.value().close_write();  // parent reads only
    char buf[16] = {};
    auto got = p.value().read_some(buf, sizeof(buf));
    REQUIRE(got.ok());
    CHECK(std::string(buf, got.value()) == "child");

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("Lab 5: signal flag only records the event") {
    pw::SignalFlag flag;
    REQUIRE(flag.install(SIGUSR1));
    CHECK_FALSE(flag.raised());

    kill(getpid(), SIGUSR1);
    CHECK(flag.raised());

    flag.clear();
    CHECK_FALSE(flag.raised());
}

TEST_CASE("Lab 6: message queue with mtype = pid") {
    auto q = pw::MsgQueue::create(IPC_PRIVATE, 0600);
    REQUIRE(q.ok());

    pw::PwMsg m = pw::msg_make(pw::OP_HEARTBEAT, static_cast<long>(getpid()), getpid());
    pw::msg_set_text(m, "ping");
    auto sent = q.value().send(&m, sizeof(m), m.mtype);
    REQUIRE(sent.ok());

    pw::PwMsg out {};
    auto got = q.value().recv(&out, sizeof(out), getpid(), IPC_NOWAIT);
    REQUIRE(got.ok());
    CHECK(out.mtype == getpid());
    CHECK(out.op == pw::OP_HEARTBEAT);
    CHECK(pw::msg_text(out) == "ping");

    q.value().remove();
}

TEST_CASE("Lab 6: negative msgtyp serves URGENT before NORMAL") {
    auto q = pw::MsgQueue::create(IPC_PRIVATE, 0600);
    REQUIRE(q.ok());

    pw::PwMsg normal = pw::msg_make(pw::OP_HEARTBEAT, pw::kMtypeNormal, getpid());
    pw::PwMsg urgent = pw::msg_make(pw::OP_KILL, pw::kMtypeUrgent, getpid());

    // Send NORMAL first, then URGENT.
    REQUIRE(q.value().send(&normal, sizeof(normal), normal.mtype).ok());
    REQUIRE(q.value().send(&urgent, sizeof(urgent), urgent.mtype).ok());

    pw::PwMsg out {};
    auto got = q.value().recv(&out, sizeof(out), -2, 0);
    REQUIRE(got.ok());
    CHECK(out.mtype == pw::kMtypeUrgent);
    CHECK(out.op == pw::OP_KILL);

    q.value().remove();
}

TEST_CASE("Lab 6: shared memory attach, detach, remove") {
    auto s = pw::ShmSegment::create(IPC_PRIVATE, 4096, 0600);
    REQUIRE(s.ok());
    CHECK(s.value().size() == 4096);

    auto mapped = s.value().attach(false);
    REQUIRE(mapped.ok());
    auto* bytes = static_cast<unsigned char*>(mapped.value());
    std::memset(bytes, 0xab, 4096);
    CHECK(bytes[0] == 0xab);
    CHECK(bytes[4095] == 0xab);

    s.value().detach();
    auto again = s.value().attach(true);
    REQUIRE(again.ok());
    const auto* ro = static_cast<const unsigned char*>(again.value());
    CHECK(ro[100] == 0xab);

    s.value().detach();
    s.value().remove();
}

TEST_CASE("macOS IPC limits are printed") {
    auto q = pw::MsgQueue::create(IPC_PRIVATE, 0600);
    REQUIRE(q.ok());
    MESSAGE("msg_qbytes = " << q.value().qbytes());
    CHECK(q.value().qbytes() > 0);
    q.value().remove();

    int shmmax = 0;
    std::size_t len = sizeof(shmmax);
    if (sysctlbyname("kern.sysv.shmmax", &shmmax, &len, nullptr, 0) == 0) {
        MESSAGE("kern.sysv.shmmax = " << shmmax);
        CHECK(shmmax > 0);
    }
}
