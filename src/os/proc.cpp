#include "pw/os/proc.hpp"

#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

#include "pw/os/sync.hpp"

namespace pw {
namespace {

using CacheKey = std::tuple<int32_t, uint64_t, uint64_t>;
Mutex g_cache_mu;
std::map<CacheKey, CmdlineEnv> g_cache;

uint64_t now_usec() {
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ull +
           static_cast<uint64_t>(tv.tv_usec);
}

// KERN_PROCARGS2 layout: argc, exec path, NUL padding, argv[0..argc-1],
// then the environment ("KEY=VALUE") until the end of the buffer.
Result<CmdlineEnv> read_cmdline_env(int32_t pid) {
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    std::size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0)
        return Result<CmdlineEnv>::err(errno, "sysctl(KERN_PROCARGS2) size");
    if (size < sizeof(int)) return Result<CmdlineEnv>::err(EINVAL, "KERN_PROCARGS2 too small");

    std::vector<char> buf(size);
    if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0)
        return Result<CmdlineEnv>::err(errno, "sysctl(KERN_PROCARGS2)");

    int argc = 0;
    std::memcpy(&argc, buf.data(), sizeof(argc));
    if (argc < 0) argc = 0;

    const char* p = buf.data() + sizeof(int);
    const char* end = buf.data() + size;

    while (p < end && *p != '\0') ++p;  // exec path
    while (p < end && *p == '\0') ++p;

    CmdlineEnv out;
    for (int i = 0; i < argc && p < end; ++i) {
        const char* start = p;
        while (p < end && *p != '\0') ++p;
        out.argv.emplace_back(start, static_cast<std::size_t>(p - start));
        if (p < end) ++p;
    }

    while (p < end && *p == '\0') ++p;
    while (p < end) {
        const char* start = p;
        while (p < end && *p != '\0') ++p;
        std::string kv(start, static_cast<std::size_t>(p - start));
        if (p < end) ++p;
        const auto eq = kv.find('=');
        if (eq != std::string::npos) out.env.emplace(kv.substr(0, eq), kv.substr(eq + 1));
    }
    return out;
}

}  // namespace

// libproc: enumerate this uid's processes, then read BSD info, footprint and
// resident size. Processes that vanish mid-snapshot are skipped silently.
Result<Snapshot> snapshot() {
    int bytes = proc_listpids(PROC_UID_ONLY, static_cast<uint32_t>(getuid()), nullptr, 0);
    if (bytes < 0) return Result<Snapshot>::err(errno, "proc_listpids(size)");
    if (bytes == 0) return Snapshot{};

    std::vector<pid_t> pids(static_cast<std::size_t>(bytes) / sizeof(pid_t) + 32);
    bytes = proc_listpids(PROC_UID_ONLY, static_cast<uint32_t>(getuid()), pids.data(),
                          static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytes < 0) return Result<Snapshot>::err(errno, "proc_listpids");

    Snapshot snap;
    snap.t_usec = now_usec();
    const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(pid_t);
    snap.procs.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0) continue;

        struct proc_bsdinfo bsd {};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) !=
            static_cast<int>(sizeof(bsd)))
            continue;

        ProcInfo p;
        p.pid = static_cast<int32_t>(bsd.pbi_pid);
        p.ppid = static_cast<int32_t>(bsd.pbi_ppid);
        p.pgid = static_cast<int32_t>(bsd.pbi_pgid);
        p.start_sec = bsd.pbi_start_tvsec;
        p.start_usec = bsd.pbi_start_tvusec;
        p.status = bsd.pbi_status;
        p.tty = bsd.e_tdev;
        std::memcpy(p.comm, bsd.pbi_comm, sizeof(bsd.pbi_comm));
        p.comm[sizeof(p.comm) - 1] = '\0';

        struct rusage_info_v2 ri {};
        if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) == 0) {
            p.footprint = ri.ri_phys_footprint;
            p.cpu_time_ns = ri.ri_user_time + ri.ri_system_time;
        }

        struct proc_taskinfo ti {};
        if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) ==
            static_cast<int>(sizeof(ti)))
            p.rss = ti.pti_resident_size;

        snap.procs.push_back(p);
    }
    return snap;
}

Result<CmdlineEnv> cmdline_env(int32_t pid, uint64_t start_sec, uint64_t start_usec) {
    const CacheKey key{pid, start_sec, start_usec};
    {
        LockGuard guard(g_cache_mu);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it->second;
    }

    auto r = read_cmdline_env(pid);
    if (r.ok()) {
        LockGuard guard(g_cache_mu);
        g_cache[key] = r.value();
    }
    return r;
}

void clear_cmdline_cache() {
    LockGuard guard(g_cache_mu);
    g_cache.clear();
}

// libproc: current working directory (used to match sessions by project).
Result<std::string> cwd_of(int32_t pid) {
    struct proc_vnodepathinfo vpi {};
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) !=
        static_cast<int>(sizeof(vpi)))
        return Result<std::string>::err(errno, "proc_pidinfo(PROC_PIDVNODEPATHINFO)");
    return std::string(vpi.pvi_cdir.vip_path);
}

}  // namespace pw
