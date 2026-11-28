#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <sys/ipc.h>
#include <sys/types.h>
#include <type_traits>

#include "pw/result.hpp"

namespace pw {

// Lab 6: one System V message queue, one message type per priority lane.
inline constexpr std::size_t kMsgText = 168;

struct PwMsg {
    long mtype;        // 1 URGENT -> daemon, 2 NORMAL -> daemon, >2 reply to client pid
    int32_t op;        // Op below
    int32_t client_pid;
    int32_t sess_id;
    int32_t a;
    int32_t b;
    int64_t c;         // bytes, start-time usec, or plan hash
    char text[kMsgText];
};

// macOS default msg_qbytes is small (2048 bytes on the lab Macs), so the
// payload must stay well under that.
static_assert(sizeof(PwMsg) - sizeof(long) <= 248, "PwMsg payload exceeds 248 bytes");
static_assert(std::is_trivially_copyable_v<PwMsg>, "PwMsg must be trivially copyable");

enum Op : int32_t {
    OP_REGISTER = 1,
    OP_HEARTBEAT,
    OP_EXITED,
    OP_DETACHED,
    OP_ADMIT_REQ,
    OP_CANCEL,
    OP_KILL,
    OP_PAUSE,
    OP_RESUME,
    OP_RENICE,
    OP_SET_BUDGET,
    OP_ADOPT,
    OP_PLAN_CLEANUP,
    OP_APPROVE_CLEANUP,
    OP_PORT_LOOKUP,
    OP_SETTINGS_RELOAD,
    OP_REPLY,
    OP_SHUTDOWN,   // internal: wakes the command-server thread on daemon stop
};

enum MsgType : long {
    kMtypeUrgent = 1,
    kMtypeNormal = 2,
};

enum ReplyCode : int32_t {
    kReplyOk = 0,
    kReplyAdmit = 1,
    kReplyQueued = 2,
    kReplyRefused = 3,
    kReplyTimeout = 4,
    kReplyError = 5,
    kReplyStaleHash = 6,
    kReplyBusy = 7,
};

struct IpcKeys {
    key_t queue_key = -1;
    key_t shm_key = -1;
};

// ftok(~/.procwarden/key, 'Q'/'S'). Creates the key file if missing.
std::optional<Error> ensure_ipc_key_file();
Result<IpcKeys> ipc_keys();

void msg_set_text(PwMsg& m, const std::string& s);
std::string msg_text(const PwMsg& m);

// Fills the common header fields and zeroes the rest.
PwMsg msg_make(int32_t op, long mtype, int32_t client_pid);

}  // namespace pw
