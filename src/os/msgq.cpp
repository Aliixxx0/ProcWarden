#include "pw/os/msgq.hpp"

#include <sys/msg.h>

#include <cerrno>
#include <cstring>

namespace pw {

// Lab 6: msgget(IPC_CREAT).
Result<MsgQueue> MsgQueue::create(key_t key, int perm) {
    const int id = msgget(key, IPC_CREAT | perm);
    if (id < 0) return Result<MsgQueue>::err(errno, "msgget(create)");
    MsgQueue q;
    q.id_ = id;
    return q;
}

// Lab 6: msgget on an existing queue.
Result<MsgQueue> MsgQueue::open(key_t key) {
    const int id = msgget(key, 0);
    if (id < 0) return Result<MsgQueue>::err(errno, "msgget(open)");
    MsgQueue q;
    q.id_ = id;
    return q;
}

// Lab 6: msgsnd; size includes the leading long mtype.
Result<long> MsgQueue::send(const void* data, std::size_t size, long mtype, int flags) {
    if (id_ < 0) return Result<long>::err(EBADF, "msgsnd: closed");
    if (size < sizeof(long)) return Result<long>::err(EINVAL, "msgsnd: size < sizeof(long)");
    long stored = 0;
    std::memcpy(&stored, data, sizeof(stored));
    if (stored != mtype) return Result<long>::err(EINVAL, "msgsnd: mtype mismatch");
    for (;;) {
        const int rc = msgsnd(id_, data, size - sizeof(long), flags);
        if (rc == 0) return static_cast<long>(size - sizeof(long));
        if (errno == EINTR) continue;
        return Result<long>::err(errno, "msgsnd");
    }
}

// Lab 6: msgrcv. msgtyp semantics are passed through:
//   > 0  first message of exactly that type
//   = 0  first message on the queue
//   < 0  first message with type <= |msgtyp|  (lowest type first)
//
// macOS implements the negative case as an upper bound in FIFO order, not as
// "lowest type first" (see docs/DEVIATIONS.md DEV-001). To keep the brief's
// URGENT-before-NORMAL contract, probe exact types 1..|msgtyp| non-blocking
// first and only then fall back to the kernel's blocking behaviour.
Result<long> MsgQueue::recv(void* buf, std::size_t size, long msgtyp, int flags) {
    if (id_ < 0) return Result<long>::err(EBADF, "msgrcv: closed");
    if (size < sizeof(long)) return Result<long>::err(EINVAL, "msgrcv: size < sizeof(long)");

    if (msgtyp < 0) {
        const long limit = -msgtyp;
        for (long t = 1; t <= limit; ++t) {
            for (;;) {
                const ssize_t got =
                    msgrcv(id_, buf, size - sizeof(long), t, flags | IPC_NOWAIT);
                if (got >= 0) return static_cast<long>(got);
                if (errno == EINTR) continue;
                break;  // ENOMSG: try the next type
            }
        }
    }

    for (;;) {
        const ssize_t got = msgrcv(id_, buf, size - sizeof(long), msgtyp, flags);
        if (got >= 0) return static_cast<long>(got);
        if (errno == EINTR) continue;
        return Result<long>::err(errno, "msgrcv");
    }
}

std::size_t MsgQueue::qbytes() const {
    if (id_ < 0) return 0;
    struct msqid_ds ds {};
    if (msgctl(id_, IPC_STAT, &ds) != 0) return 0;
    return ds.msg_qbytes;
}

void MsgQueue::remove() {
    if (id_ >= 0) {
        msgctl(id_, IPC_RMID, nullptr);
        id_ = -1;
    }
}

}  // namespace pw
