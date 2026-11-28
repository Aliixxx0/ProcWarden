#pragma once

#include <cstddef>
#include <sys/ipc.h>
#include <sys/types.h>

#include "pw/result.hpp"

namespace pw {

// Lab 6: System V message queue. send/recv take the full buffer including the
// leading long mtype; recv passes msgtyp straight through (negative msgtyp =
// lowest type first, i.e. URGENT before NORMAL).
class MsgQueue {
public:
    MsgQueue() = default;

    static Result<MsgQueue> create(key_t key, int perm);
    static Result<MsgQueue> open(key_t key);

    int id() const { return id_; }
    bool valid() const { return id_ >= 0; }

    Result<long> send(const void* data, std::size_t size, long mtype, int flags = 0);
    Result<long> recv(void* buf, std::size_t size, long msgtyp, int flags);

    std::size_t qbytes() const;
    void remove();

private:
    int id_ = -1;
};

}  // namespace pw
