#include "pw/os/mem.hpp"

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>

namespace pw {

// mach + sysctl: total RAM, used/compressed memory, swap, pressure, disk.
Result<MemSample> mem_sample() {
    MemSample s;

    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    s.t_usec = static_cast<uint64_t>(tv.tv_sec) * 1000000ull +
               static_cast<uint64_t>(tv.tv_usec);

    std::size_t len = sizeof(s.total);
    if (sysctlbyname("hw.memsize", &s.total, &len, nullptr, 0) != 0)
        return Result<MemSample>::err(errno, "sysctl(hw.memsize)");

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS)
        return Result<MemSample>::err(EIO, "host_page_size");
    s.page_size = page;

    vm_statistics64_data_t vm {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS)
        return Result<MemSample>::err(EIO, "host_statistics64");

    s.active = static_cast<uint64_t>(vm.active_count) * page;
    s.wired = static_cast<uint64_t>(vm.wire_count) * page;
    s.compressed = static_cast<uint64_t>(vm.compressor_page_count) * page;
    s.phys_free = static_cast<uint64_t>(vm.free_count) * page;
    s.swapouts = vm.swapouts;

    // Estimate used RAM: active + wired + compressor pages.
    s.used = s.active + s.wired + s.compressed;
    if (s.used > s.total) s.used = s.total;

    struct xsw_usage swap {};
    std::size_t swap_len = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &swap_len, nullptr, 0) == 0) {
        s.swap_total = swap.xsu_total;
        s.swap_used = swap.xsu_used;
    }

    int pressure = 1;
    std::size_t pressure_len = sizeof(pressure);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &pressure_len,
                     nullptr, 0) == 0)
        s.pressure_level = static_cast<uint32_t>(pressure);

    struct statfs fs {};
    if (statfs("/", &fs) == 0) {
        s.disk_total = static_cast<uint64_t>(fs.f_blocks) * fs.f_bsize;
        s.disk_free = static_cast<uint64_t>(fs.f_bavail) * fs.f_bsize;
    }

    return s;
}

}  // namespace pw
