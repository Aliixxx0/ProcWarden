#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pw/config.hpp"
#include "pw/ipc/protocol.hpp"
#include "pw/ipc/status_table.hpp"
#include "pw/os/msgq.hpp"
#include "pw/os/shm.hpp"

namespace pw {

// Thin IPC client: one request/reply over the System V queue plus a read-only
// mapping of the status table.
struct Client {
    MsgQueue queue;
    ShmSegment shm;
    StatusTable* table = nullptr;

    Client() = default;
    ~Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = default;
    Client& operator=(Client&&) = default;

    static Result<Client> connect(bool need_shm);
    Result<PwMsg> request(PwMsg msg, int timeout_ms);
    bool refresh(StatusTable& out) const;
};

std::string fmt_bytes(uint64_t bytes);
std::string fmt_gb(uint64_t bytes);
std::string hash_hex(uint64_t hash);
void cli_sleep_ms(int ms);
std::string status_to_json(const StatusTable& t);
void draw_dashboard(const StatusTable& t);
void print_status_line(const StatusTable& t);

int cmd_run(const std::vector<std::string>& args);
int cmd_init(const std::vector<std::string>& args);
int cmd_watch(const std::vector<std::string>& args);
int cmd_status(const std::vector<std::string>& args);
int cmd_ctl(const std::vector<std::string>& args);
int cmd_cleanup(const std::vector<std::string>& args);
int cmd_top(const std::vector<std::string>& args);
int cmd_install(const std::vector<std::string>& args, const std::string& self_path);

}  // namespace pw
