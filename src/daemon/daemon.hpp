#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pw/config.hpp"
#include "pw/core/baseline.hpp"
#include "pw/core/cleanup.hpp"
#include "pw/core/decision.hpp"
#include "pw/core/membership.hpp"
#include "pw/core/registry.hpp"
#include "pw/core/session.hpp"
#include "pw/core/snapshot.hpp"
#include "pw/ipc/protocol.hpp"
#include "pw/ipc/status_table.hpp"
#include "pw/os/mem.hpp"
#include "pw/os/msgq.hpp"
#include "pw/os/shm.hpp"
#include "pw/os/signals.hpp"
#include "pw/os/sync.hpp"
#include "pw/os/thread.hpp"

namespace pw {

struct Job {
    enum class Kind { Terminate, Pause, Resume, Renice, Cleanup };
    Kind kind = Kind::Terminate;
    int32_t sess_id = 0;
    int32_t nice_value = 0;
    CleanupPlan plan;
};

struct PendingPlan {
    CleanupPlan plan;
    uint64_t created_usec = 0;
};

struct DaemonState {
    Config cfg;
    IpcKeys keys;
    MsgQueue queue;
    ShmSegment shm;
    StatusTable* table = nullptr;

    Mutex mu;
    CondVar job_cv;
    std::deque<Job> jobs;
    bool stop = false;

    std::vector<Session> sessions;
    int32_t next_session_id = 1;
    std::vector<int32_t> admit_queue;

    DecisionState decision;
    std::vector<MemSample> mem_ring;
    std::deque<std::pair<uint64_t, double>> system_history;
    std::map<int32_t, std::deque<std::pair<uint64_t, double>>> session_growth;
    std::map<std::string, std::vector<double>> usual_by_key;
    std::map<int32_t, double> usual_gb;
    std::map<int32_t, uint64_t> ratio_since;
    std::map<int32_t, uint64_t> detach_deadline;
    std::map<std::string, PhaseRules> rules_cache;
    NormalRange normal;

    PendingPlan pending;
    uint64_t cleanup_visible_hash = 0;
    Registry registry;

    SignalFlag sig_term;
    SignalFlag sig_int;
    SignalFlag sig_hup;

    PosixThread monitor_thread;
    PosixThread command_thread;
    PosixThread enforcer_thread;

    std::string registry_path;
    std::string history_dir;
    uint64_t last_history_write = 0;
    uint32_t sample_no = 0;
};

Session* find_session(DaemonState& st, int32_t id);
const Session* find_session(const DaemonState& st, int32_t id);
std::string base_name(const std::string& path);
DetachPolicy parse_detach(const std::string& s);
const char* detach_name(DetachPolicy p);

void reply(DaemonState& st, const PwMsg& req, int32_t code, const std::string& text,
           int64_t c = 0, int32_t extra = 0);
void request_stop(DaemonState& st);
void sleep_ms(int ms);
uint64_t now_usec();

void sync_registry_locked(DaemonState& st);
void save_registry(DaemonState& st);

void load_history(DaemonState& st);
void write_history_locked(DaemonState& st, const Snapshot& snap, const MemSample& mem);

PhaseRules rules_for_project(const std::string& project_path);
std::vector<CacheInfo> measure_caches(const Config& cfg);
CleanupInput build_cleanup_input(DaemonState& st, bool include_ram, bool include_disk);

void publish_locked(DaemonState& st, const Snapshot& snap, const MemSample& mem,
                    const Decision& decision);
void handle_message(DaemonState& st, const PwMsg& msg);
void enqueue_locked(DaemonState& st, Job job);
void execute_job(DaemonState& st, Job& job);
void run_cleanup(DaemonState& st, const CleanupPlan& plan);

void* monitor_main(void* arg);
void* command_main(void* arg);
void* enforcer_main(void* arg);

}  // namespace pw
