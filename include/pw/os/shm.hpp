#pragma once

#include <cstddef>
#include <sys/ipc.h>
#include <sys/types.h>

#include "pw/result.hpp"

namespace pw {

// Lab 6: System V shared memory. attach() maps it (optionally read-only);
// detach() in the destructor; remove() marks it for deletion with IPC_RMID.
class ShmSegment {
public:
    ShmSegment() = default;
    ~ShmSegment();
    ShmSegment(const ShmSegment&) = delete;
    ShmSegment& operator=(const ShmSegment&) = delete;
    ShmSegment(ShmSegment&& other) noexcept;
    ShmSegment& operator=(ShmSegment&& other) noexcept;

    static Result<ShmSegment> create(key_t key, std::size_t size, int perm);
    static Result<ShmSegment> open(key_t key);

    int id() const { return id_; }
    std::size_t size() const { return size_; }

    Result<void*> attach(bool readonly);
    void detach();
    void remove();

private:
    int id_ = -1;
    void* addr_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace pw
