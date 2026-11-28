#include "pw/os/ports.hpp"

#include <libproc.h>
#include <netinet/in.h>
#include <sys/proc_info.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

namespace pw {

Result<std::vector<PortOwner>> listening_ports() {
    int bytes = proc_listpids(PROC_UID_ONLY, static_cast<uint32_t>(getuid()), nullptr, 0);
    if (bytes < 0) return Result<std::vector<PortOwner>>::err(errno, "proc_listpids(size)");

    std::vector<pid_t> pids(static_cast<std::size_t>(bytes) / sizeof(pid_t) + 32);
    bytes = proc_listpids(PROC_UID_ONLY, static_cast<uint32_t>(getuid()), pids.data(),
                          static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytes < 0) return Result<std::vector<PortOwner>>::err(errno, "proc_listpids");

    std::vector<PortOwner> out;
    const std::size_t count = static_cast<std::size_t>(bytes) / sizeof(pid_t);

    for (std::size_t i = 0; i < count; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0) continue;

        int fd_bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (fd_bytes <= 0) continue;
        std::vector<struct proc_fdinfo> fds(
            static_cast<std::size_t>(fd_bytes) / sizeof(struct proc_fdinfo) + 4);
        fd_bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(),
                                static_cast<int>(fds.size() * sizeof(struct proc_fdinfo)));
        if (fd_bytes <= 0) continue;

        const std::size_t fd_count =
            static_cast<std::size_t>(fd_bytes) / sizeof(struct proc_fdinfo);
        for (std::size_t k = 0; k < fd_count; ++k) {
            if (fds[k].proc_fdtype != PROX_FDTYPE_SOCKET) continue;

            struct socket_fdinfo si {};
            if (proc_pidfdinfo(pid, fds[k].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof(si)) !=
                static_cast<int>(sizeof(si)))
                continue;
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            if (si.psi.soi_proto.pri_tcp.tcpsi_state != TSI_S_LISTEN) continue;

            const uint16_t port = ntohs(static_cast<uint16_t>(
                si.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport));
            if (port == 0) continue;

            bool seen = false;
            for (const auto& p : out)
                if (p.port == port && p.pid == pid) seen = true;
            if (!seen) out.push_back(PortOwner{port, static_cast<int32_t>(pid)});
        }
    }
    return out;
}

}  // namespace pw
