#include "pw/ipc/protocol.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

#include "pw/config.hpp"

namespace pw {

std::optional<Error> ensure_ipc_key_file() {
    const std::string dir = procwarden_home();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return Error{ec.value(), "create " + dir};

    const std::string path = dir + "/key";
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) return std::nullopt;

    int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    if (fd < 0) return Error{errno, "create " + path};
    const char payload[] = "procwarden-ipc";
    const ssize_t n = write(fd, payload, sizeof(payload));
    const int saved = errno;
    close(fd);
    if (n != static_cast<ssize_t>(sizeof(payload)))
        return Error{saved, "write " + path};
    return std::nullopt;
}

Result<IpcKeys> ipc_keys() {
    if (auto e = ensure_ipc_key_file()) return *e;
    const std::string path = procwarden_home() + "/key";

    IpcKeys keys;
    keys.queue_key = ftok(path.c_str(), 'Q');
    keys.shm_key = ftok(path.c_str(), 'S');
    if (keys.queue_key == -1 || keys.shm_key == -1)
        return Result<IpcKeys>::err(errno, "ftok(" + path + ")");
    return keys;
}

void msg_set_text(PwMsg& m, const std::string& s) {
    std::memset(m.text, 0, sizeof(m.text));
    const std::size_t n = s.size() < sizeof(m.text) - 1 ? s.size() : sizeof(m.text) - 1;
    std::memcpy(m.text, s.data(), n);
    m.text[n] = '\0';
}

std::string msg_text(const PwMsg& m) {
    return std::string(m.text, strnlen(m.text, sizeof(m.text)));
}

PwMsg msg_make(int32_t op, long mtype, int32_t client_pid) {
    PwMsg m {};
    m.mtype = mtype;
    m.op = op;
    m.client_pid = client_pid;
    return m;
}

}  // namespace pw
