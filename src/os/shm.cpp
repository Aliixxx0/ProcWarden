#include "pw/os/shm.hpp"

#include <sys/shm.h>

#include <cerrno>
#include <utility>

namespace pw {

ShmSegment::~ShmSegment() {
    detach();
}

ShmSegment::ShmSegment(ShmSegment&& other) noexcept
    : id_(other.id_), addr_(other.addr_), size_(other.size_) {
    other.id_ = -1;
    other.addr_ = nullptr;
    other.size_ = 0;
}

ShmSegment& ShmSegment::operator=(ShmSegment&& other) noexcept {
    if (this != &other) {
        detach();
        id_ = other.id_;
        addr_ = other.addr_;
        size_ = other.size_;
        other.id_ = -1;
        other.addr_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

// Lab 6: shmget(IPC_CREAT).
Result<ShmSegment> ShmSegment::create(key_t key, std::size_t size, int perm) {
    const int id = shmget(key, size, IPC_CREAT | perm);
    if (id < 0) return Result<ShmSegment>::err(errno, "shmget(create)");
    ShmSegment s;
    s.id_ = id;
    s.size_ = size;
    return s;
}

// Lab 6: shmget on an existing segment; size from IPC_STAT.
Result<ShmSegment> ShmSegment::open(key_t key) {
    const int id = shmget(key, 0, 0);
    if (id < 0) return Result<ShmSegment>::err(errno, "shmget(open)");
    ShmSegment s;
    s.id_ = id;
    struct shmid_ds ds {};
    if (shmctl(id, IPC_STAT, &ds) == 0) s.size_ = ds.shm_segsz;
    return s;
}

// Lab 6: shmat, optionally SHM_RDONLY for readers.
Result<void*> ShmSegment::attach(bool readonly) {
    if (id_ < 0) return Result<void*>::err(EBADF, "shmat: closed");
    if (addr_ != nullptr) return addr_;
    void* p = shmat(id_, nullptr, readonly ? SHM_RDONLY : 0);
    if (p == reinterpret_cast<void*>(-1))
        return Result<void*>::err(errno, "shmat");
    addr_ = p;
    return p;
}

// Lab 6: shmdt.
void ShmSegment::detach() {
    if (addr_ != nullptr) {
        shmdt(addr_);
        addr_ = nullptr;
    }
}

// Lab 6: shmctl(IPC_RMID).
void ShmSegment::remove() {
    if (id_ >= 0) {
        shmctl(id_, IPC_RMID, nullptr);
        id_ = -1;
    }
}

}  // namespace pw
