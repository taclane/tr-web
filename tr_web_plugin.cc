// tr-web: Web Status Plugin for Trunk-Recorder

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <string>
#include <tuple>
#include <time.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

// Trunk-Recorder headers
#include "../../lib/json.hpp"
#include "../../trunk-recorder/plugin_manager/plugin_api.h"
#include "../../trunk-recorder/source.h"
#include "../../trunk-recorder/systems/system_impl.h"

// System/library headers
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/dll/alias.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/trivial.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

// Plugin headers
#include "httplib.h"
#include "sqlite_db.h"
#include "web_assets.h"

using namespace std;
using json = nlohmann::json;
namespace logging = boost::log;

struct RatePoint
{
    time_t timestamp;
    double rate;
};

class Tr_Web : public Plugin_Api
{
    // HTTP Server
    httplib::Server server_;
    std::thread server_thread_;
    std::thread broadcast_thread_;
    std::atomic<bool> running_;
    bool started_ = false;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> shutdown_notice_{false};
    boost::shared_ptr<logging::sinks::sink> web_sink_;

    // ============================================================================
    // WEB-RELATED CODE
    // ============================================================================

    // Console log buffer. Every line carries a sequence number so a client that loaded the
    // history from /api/status can skip streamed lines it already has.
    struct ConsoleLine
    {
        uint64_t seq;
        std::string text;
    };
    mutable std::mutex console_mutex_;
    std::deque<ConsoleLine> console_logs_;
    uint64_t console_seq_ = 0;

    // Pending console lines for SSE (bounded, flushed from broadcast thread)
    mutable std::mutex console_pending_mutex_;
    std::deque<ConsoleLine> console_pending_;
    size_t console_pending_dropped_ = 0;

    // Discrete SSE events that should be delivered even if the periodic snapshots miss them
    mutable std::mutex event_queue_mutex_;
    std::deque<std::pair<std::string, std::string>> event_queue_;
    size_t event_queue_dropped_ = 0;

    // Graph streaming events for Gephi compatibility
    mutable std::mutex graph_event_queue_mutex_;
    std::deque<std::string> graph_event_queue_;
    size_t graph_event_queue_dropped_ = 0;

    void add_console_line(const std::string &line)
    {
        std::string timestamped_line;
        {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            localtime_r(&time, &tm);
            std::ostringstream oss;
            oss << std::put_time(&tm, "[%H:%M:%S] ") << line;
            timestamped_line = oss.str();
        }

        // Hard cap a single log line so pathological output can't stall the process.
        // This preserves the start of the line (most relevant content).
        static constexpr size_t MAX_CONSOLE_LINE_BYTES = 4096;
        if (timestamped_line.size() > MAX_CONSOLE_LINE_BYTES)
        {
            // Back up to a UTF-8 character boundary so the cut never leaves a partial sequence
            size_t cut = MAX_CONSOLE_LINE_BYTES;
            while (cut > 0 && (static_cast<unsigned char>(timestamped_line[cut]) & 0xC0) == 0x80)
            {
                --cut;
            }
            timestamped_line.resize(cut);
            timestamped_line += "…(truncated)";
        }

        // Lock order: console_mutex_ then console_pending_mutex_, so pending lines stay in
        // sequence order. Queued for the broadcast thread; never write sockets from here.
        std::lock_guard<std::mutex> lock(console_mutex_);
        ConsoleLine entry{++console_seq_, std::move(timestamped_line)};
        {
            std::lock_guard<std::mutex> pending_lock(console_pending_mutex_);
            static constexpr size_t MAX_PENDING = 2000;
            if (console_pending_.size() >= MAX_PENDING)
            {
                ++console_pending_dropped_;
            }
            else
            {
                console_pending_.push_back(entry);
            }
        }
        console_logs_.push_back(std::move(entry));
        while (console_logs_.size() > console_max_lines_)
        {
            console_logs_.pop_front();
        }
    }

    // Returns the buffered console lines; *last_seq receives the sequence number of the newest
    json get_console_logs(uint64_t *last_seq = nullptr) const
    {
        // Copy data while holding lock, build JSON after releasing
        std::deque<ConsoleLine> logs_copy;
        {
            std::lock_guard<std::mutex> lock(console_mutex_);
            logs_copy = console_logs_;
            if (last_seq)
            {
                *last_seq = console_seq_;
            }
        }
        
        json logs = json::array();
        for (const auto &line : logs_copy)
        {
            logs.push_back(line.text);
        }
        return logs;
    }

    void cache_call(const json &call_json)
    {
        std::lock_guard<std::mutex> lock(call_history_mutex_);
        call_history_.push_back(call_json);
        while (call_history_.size() > MAX_CALL_HISTORY)
        {
            call_history_.pop_front();
        }
    }

    json get_call_history() const
    {
        // Copy data while holding lock, build JSON after releasing
        std::deque<json> history_copy;
        {
            std::lock_guard<std::mutex> lock(call_history_mutex_);
            history_copy = call_history_;
        }
        
        json history = json::array();
        for (const auto &call : history_copy)
        {
            history.push_back(call);
        }
        return history;
    }

    void cache_trunk_message(const json &msg_json)
    {
        std::lock_guard<std::mutex> lock(trunk_messages_mutex_);
        trunk_messages_.push_back(msg_json);
        while (trunk_messages_.size() > MAX_TRUNK_MESSAGES)
        {
            trunk_messages_.pop_front();
        }
    }

    json get_trunk_messages() const
    {
        // Copy data while holding lock, build JSON after releasing
        std::deque<json> messages_copy;
        {
            std::lock_guard<std::mutex> lock(trunk_messages_mutex_);
            messages_copy = trunk_messages_;
        }
        
        json messages = json::array();
        for (const auto &msg : messages_copy)
        {
            messages.push_back(msg);
        }
        return messages;
    }

    json get_unit_affiliations() const
    {
        std::lock_guard<std::mutex> lock(unit_affiliations_mutex_);
        json affiliations = json::object();
        for (const auto &pair : unit_affiliations_)
        {
            affiliations[std::to_string(pair.first)] = pair.second;
        }
        return affiliations;
    }

    // ============================================================================
    // BASE PLUGIN / API CODE
    // ============================================================================

    // Configuration
    int port_ = 8080;
    std::string bind_address_ = "0.0.0.0";
    std::string username_;
    std::string password_;
    std::string admin_username_;
    std::string admin_password_;
    std::string ssl_cert_;
    std::string ssl_key_;
    std::string theme_ = "nostromo";
    std::string log_prefix_;
    size_t console_max_lines_ = 5000;
    int max_connections_ = 64;

    // Reverse proxies whose X-Forwarded-For / X-Real-IP headers are believed.
    // From any other peer these headers are client-controlled and ignored.
    std::vector<std::string> trusted_proxies_ = {"127.0.0.1"};

    // Pre-computed credentials for constant-time comparison
    std::string expected_user_creds_;
    std::string expected_admin_creds_;

    // Rate limiting for authentication attempts
    mutable std::mutex auth_rate_limit_mutex_;
    mutable std::map<std::string, std::vector<time_t>> auth_attempts_;

    // Login attempts waiting to be written to the database (bounded if the database is down)
    std::mutex pending_logins_mutex_;
    std::vector<httplib::Server::LoginAttempt> pending_logins_;
    static constexpr size_t MAX_PENDING_LOGINS = 10000;
    static constexpr size_t MAX_AUTH_ATTEMPTS = 10;
    static constexpr time_t AUTH_WINDOW_SECONDS = 60;

    // Session management
    struct Session {
        std::string token;          // SHA-256 of the token (the token itself is never kept)
        std::string username;
        bool is_admin;
        time_t created;
        time_t last_access;
        time_t persisted_access = 0; // last_access last scheduled for the database
    };
    mutable std::mutex sessions_mutex_;
    std::map<std::string, Session> sessions_; // SHA-256(token) -> Session
    // Sessions created, changed or removed since the last database flush (sessions_mutex_)
    std::unordered_set<std::string> dirty_sessions_;
    // last_access is written to the database at most this often per session
    static constexpr time_t SESSION_PERSIST_ACCESS_SECONDS = 300;
    static constexpr time_t SESSION_TIMEOUT_SECONDS = 2592000; // 30 days (updates on each request)

    // Trunk-Recorder references
    Config *tr_config_;
    std::vector<Source *> tr_sources_;
    std::vector<System *> tr_systems_;
    std::vector<Call *> tr_calls_;

    // Device frequency ranges (cached once at startup)
    struct DeviceRange
    {
        int num;
        double min_hz;
        double max_hz;
    };
    std::vector<DeviceRange> device_ranges_;

    // Thread-safe data cache
    mutable std::mutex data_mutex_;
    json cached_recorders_;
    json cached_calls_;
    json cached_systems_;
    json cached_devices_;
    json cached_rates_;

    // Parsed trunk-recorder config.json (best-effort)
    json tr_config_json_;

    // ============================================================================
    // PER-SYSTEM CALL STATISTICS (Systems tab), accumulated since plugin start
    // ============================================================================

    // Counts for one set of calls on a frequency
    struct QualityCounts
    {
        uint64_t calls = 0;
        uint64_t transmissions = 0;
        double seconds = 0;   // audio recorded
        uint64_t errors = 0;  // voice bits the decoder's error correction repaired
        uint64_t spikes = 0;  // frames with unusually many errors (kept, not shown)
        double voice_bits = 0; // coded voice bits received: the bit error rate's denominator
    };

    struct FreqStats
    {
        QualityCounts all;
        uint64_t phase2_calls = 0;
        double freq_error_sum = 0; // tuning error reported at call end, Hz
        uint64_t freq_error_count = 0;
        time_t last_seen = 0;
    };

    struct TalkgroupStats
    {
        uint64_t calls = 0;
        double seconds = 0;
        uint64_t encrypted = 0;
        uint64_t emergency = 0;
        uint64_t errors = 0;
        uint64_t spikes = 0;
        double voice_bits = 0;
        std::string alpha_tag;
        time_t last_seen = 0;
    };

    // One radio's transmissions
    struct UnitStats
    {
        uint64_t transmissions = 0;
        double seconds = 0;
        uint64_t errors = 0;
        uint64_t spikes = 0;
        double voice_bits = 0;
        std::string alias;
        time_t last_seen = 0;
    };

    struct SystemStats
    {
        std::map<long long, FreqStats> frequencies; // Hz
        std::map<long, TalkgroupStats> talkgroups;
        std::map<long, UnitStats> units;
    };

    mutable std::mutex system_stats_mutex_;
    std::map<int, SystemStats> system_stats_;   // sys_num -> stats since restart
    // This hour's additions not yet in the database, keyed (system short name, hour, freq / talkgroup)
    std::map<std::tuple<std::string, int64_t, long long>, FreqStats> pending_freq_hours_;
    std::map<std::tuple<std::string, int64_t, long>, TalkgroupStats> pending_tg_hours_;
    std::map<std::tuple<std::string, int64_t, long>, UnitStats> pending_unit_hours_;
    time_t stats_since_ = time(NULL);
    // Recent /api/system/stats responses for the database windows: (sys_num, window) -> (built, body)
    std::mutex stats_window_cache_mutex_;
    std::map<std::pair<int, std::string>, std::pair<time_t, std::string>> stats_window_cache_;
    static constexpr time_t STATS_WINDOW_CACHE_SECONDS = 30;

    // Serialized /api/system/* responses per sys_num (see snapshot_system_data())
    mutable std::mutex system_data_mutex_;
    std::map<int, std::string> talkgroups_json_;
    std::map<int, std::string> unit_tags_json_;
    std::map<int, std::string> ota_json_;
    time_t last_ota_snapshot_ = 0;

    // Chart samples waiting for the database (data_mutex_): raw rows restore the charts after a
    // restart, per-minute summaries are kept for good
    enum SampleKind
    {
        SAMPLE_DECODE_RATE = 1,
        SAMPLE_ACTIVE_CALLS = 2
    };
    struct PendingSample
    {
        std::string system; // database key (see db_system_key())
        int kind;
        time_t ts;
        double value;
    };
    std::vector<PendingSample> pending_samples_;

    // Database key per system: "<short name>#<service>", e.g. "wood#P25C" and "wood#C".
    // Repeats of the same name and service get ".1", ".2" in config order.
    std::map<int, std::string> db_system_keys_;

    static std::string service_code(const std::string &type)
    {
        static const std::map<std::string, std::string> codes = {
            {"p25", "P25"},
            {"conventionalP25", "P25C"},
            {"conventional", "C"},
            {"dmr", "DMR"},
            {"conventionalDMR", "DMRC"},
            {"smartnet", "SN"},
            {"conventionalSIGMF", "SIGMF"},
        };
        auto it = codes.find(type);
        if (it != codes.end())
            return it->second;
        std::string code = type;
        for (auto &ch : code)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return code.empty() ? "SYS" : code;
    }

    void build_db_system_keys()
    {
        std::map<std::string, int> seen; // "<name>#<code>" -> systems so far
        db_system_keys_.clear();
        for (auto *sys : tr_systems_)
        {
            const std::string base = sys->get_short_name() + "#" + service_code(sys->get_system_type());
            int n = seen[base]++;
            db_system_keys_[sys->get_sys_num()] = n == 0 ? base : base + "." + std::to_string(n);
        }
    }

    std::string db_system_key(int sys_num) const
    {
        auto it = db_system_keys_.find(sys_num);
        return it != db_system_keys_.end() ? it->second : "sys#" + std::to_string(sys_num + 1);
    }
    static constexpr size_t MAX_PENDING_SAMPLES = 100000;

    void queue_sample_locked(const std::string &short_name, int kind, const RatePoint &point)
    {
        if (pending_samples_.size() < MAX_PENDING_SAMPLES)
            pending_samples_.push_back({short_name, kind, point.timestamp, point.rate});
    }

    // Rate history per system (keeps 60 minutes of data)
    std::map<std::string, std::deque<RatePoint>> rate_history_;
    static const size_t MAX_RATE_HISTORY = 1200; // 60 min at 3 sec intervals

    // Call rate history per system (keeps 60 minutes of data)
    // Note: Call rate is sampled irregularly (on call state changes), so we use time-based trimming
    std::map<std::string, std::deque<RatePoint>> call_rate_history_;
    static constexpr time_t CALL_RATE_RETENTION_SECONDS = 3600; // 60 minutes

    // Recent call history cache (last N completed calls)
    mutable std::mutex call_history_mutex_;
    std::deque<json> call_history_;
    static const size_t MAX_CALL_HISTORY = 100;

    // Track previous calls to detect disappearances (encrypted calls)
    mutable std::mutex previous_calls_mutex_;
    std::map<long, json> previous_calls_map_; // call_num -> call_json

    // Trunking message buffer (for Omnitrunker tab)
    mutable std::mutex trunk_messages_mutex_;
    std::deque<json> trunk_messages_;
    static const size_t MAX_TRUNK_MESSAGES = 300;

    // Unit affiliation tracking (unit_id -> talkgroup)
    mutable std::mutex unit_affiliations_mutex_;
    std::map<long, long> unit_affiliations_;

    // ============================================================================
    // UNIT AND STATE TRACKING
    // ============================================================================

    // State tracking for units and talkgroups (for Gephi coloring and Affiliations UI)
    struct TxCount
    {
        int voice = 0;  // Voice transmissions (grants)
        int data = 0;   // Data only (affiliations, locations)
    };

    struct UnitState
    {
        long id = 0;
        int wacn = 0;
        int sysid = 0;
        std::string alias;
        bool encr_seen = false; // Has ever transmitted encrypted
        time_t last_active = 0;
        bool registered = false;
        TxCount tx_count;  // [voice, data] transmissions
        std::map<long, TxCount> tg_activity; // tg_id -> [voice, data] counts (heatmap data)
    };

    struct TalkgroupState
    {
        long id = 0;
        int wacn = 0;
        int sysid = 0;
        std::string alias;
        bool encr_seen = false; // Has ever had encrypted traffic
        time_t last_active = 0;
        TxCount tx_count;  // [voice, data] transmissions
        std::map<long, TxCount> unit_activity; // unit_id -> [voice, data] counts (heatmap data)
    };

    // Composite key for multi-system support: "wacn:sysid:id"
    std::string make_unit_key(int wacn, int sysid, long unit_id) const
    {
        return std::to_string(wacn) + ":" + std::to_string(sysid) + ":" + std::to_string(unit_id);
    }

    std::string make_tg_key(int wacn, int sysid, long tg_id) const
    {
        return std::to_string(wacn) + ":" + std::to_string(sysid) + ":" + std::to_string(tg_id);
    }

    mutable std::mutex affiliation_state_mutex_;
    std::map<std::string, UnitState> unit_states_;           // keyed by "wacn:sysid:unit_id"
    std::map<std::string, TalkgroupState> talkgroup_states_; // keyed by "wacn:sysid:tg_id"

    // Entries changed since the last database flush (guarded by affiliation_state_mutex_).
    // The flush copies just these, so trunk-recorder's thread never waits on a full copy.
    std::unordered_set<std::string> dirty_units_;
    std::unordered_set<std::string> dirty_talkgroups_;
    std::set<std::pair<std::string, long>> dirty_links_; // (unit key, talkgroup id)

    // Caller holds affiliation_state_mutex_
    void mark_affiliation_dirty_locked(const std::string &unit_key, const std::string &tg_key = std::string(), long tg_id = 0)
    {
        dirty_units_.insert(unit_key);
        if (!tg_key.empty())
        {
            dirty_talkgroups_.insert(tg_key);
            dirty_links_.emplace(unit_key, tg_id);
        }
    }

    // Configuration for affiliation tracking
    int affiliation_timeout_ = 12;
    std::string affiliation_cache_;
    std::string database_path_ = "tr-web.db";
    std::string affiliation_export_;          // optional periodic JSON export (never read back)
    int affiliation_export_interval_ = 3600;  // seconds

    // Database (see open_database()); db_ is used only by the database thread after start()
    std::unique_ptr<sqlite_db::Database> db_;
    std::unique_ptr<sqlite_db::Database> db_read_; // separate connection for web requests (WAL lets it read while db_ writes)
    std::thread db_thread_;
    std::mutex db_wake_mutex_;
    std::condition_variable db_wake_;
    bool db_stop_ = false;
    uint64_t db_failures_ = 0;
    time_t last_db_backup_ = 0;
    time_t last_sample_prune_ = 0;

    // Flag to trigger initial Gephi dump on next poll cycle
    std::atomic<bool> gephi_initial_dump_pending_{false};

    // Dirty flags for SSE broadcasts
    std::atomic<uint32_t> dirty_flags_{0};

    enum DirtyBits : uint32_t
    {
        DIRTY_SYSTEMS = 1u << 0,
        DIRTY_RECORDERS = 1u << 1,
        DIRTY_CALLS = 1u << 2,
        DIRTY_RATES = 1u << 3,
        DIRTY_TRUNK_MESSAGES = 1u << 4,
        DIRTY_DEVICES = 1u << 5
    };

    // Serializes only when someone is listening: runs on trunk-recorder's thread per event
    void enqueue_sse_event(const std::string &event, const json &payload)
    {
        // Only enqueue if there are connected SSE clients
        if (server_.sse_client_count() == 0)
        {
            return;
        }

        std::string data = payload.dump(-1, ' ', false, json::error_handler_t::replace);
        std::lock_guard<std::mutex> lock(event_queue_mutex_);
        static constexpr size_t MAX_EVENTS = 2000;
        if (event_queue_.size() >= MAX_EVENTS)
        {
            ++event_queue_dropped_;
            return;
        }
        event_queue_.emplace_back(event, std::move(data));
    }

    void enqueue_graph_event(std::string data)
    {
        // Only enqueue if there are connected raw stream (graphstream) clients
        if (server_.raw_stream_client_count() == 0)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(graph_event_queue_mutex_);
        static constexpr size_t MAX_GRAPH_EVENTS = 1000;
        if (graph_event_queue_.size() >= MAX_GRAPH_EVENTS)
        {
            ++graph_event_queue_dropped_;
            return;
        }
        graph_event_queue_.emplace_back(std::move(data));
    }

    // ============================================================================
    // GEPHI / GRAPHSTREAM MANAGEMENT
    // ============================================================================

    // Trigger initial Gephi dump (called from httplib when raw stream client connects)
    void request_gephi_initial_dump()
    {
        gephi_initial_dump_pending_.store(true, std::memory_order_release);
    }

    // Full affiliation graph for new Gephi clients. Built under the lock, sent by the caller.
    std::string build_gephi_initial_state()
    {
        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        std::string out;

        int node_count = 0;
        int edge_count = 0;

        json all_nodes;
        json all_edges;
        json edge_map;

        // Gather all unit nodes
        for (const auto &[key, unit] : unit_states_)
        {
            if (unit.id == 0 || unit.id == -1)
                continue;
            std::string node_id = std::to_string(unit.id);
            std::string label = unit.alias.empty() ? ("Unit " + std::to_string(unit.id)) : unit.alias;

            // Use centralized color logic (single source of truth)
            std::string color = get_unit_color(unit);
            
            // Get status using unlocked version (we already hold affiliation_state_mutex_)
            std::string status = get_unit_status_unlocked(unit.wacn, unit.sysid, unit.id);
            
            json node_data = {
                {"id", unit.id},
                {"label", label},
                {"color", color},
                {"size", 15},
                {"encryption", unit.encr_seen},
                {"status", status}};
                // {"deregistered", !unit.registered}};

            all_nodes[node_id] = node_data;
            node_count++;

            // Add unit->tg pairings to edge_map
            // Voice links use voice counts only; data-only links use data counts
            for (const auto &[tg_id, count] : unit.tg_activity)
            {
                if (tg_id == 0 || tg_id == -1)
                    continue;
                std::string edge_key = "TG-" + std::to_string(tg_id) + "-" + std::to_string(unit.id);
                
                double unit_weight = 0.0;
                
                if (count.voice > 0)
                {
                    // Voice exists: calculate weight from voice transmissions only
                    unit_weight = (unit.tx_count.voice > 0) ? static_cast<double>(count.voice) / unit.tx_count.voice : 0.0;
                }
                else if (count.data > 0)
                {
                    // Data only: calculate weight from data transmissions only
                    unit_weight = (unit.tx_count.data > 0) ? static_cast<double>(count.data) / unit.tx_count.data : 0.0;
                }
                
                // Round to nearest 0.01, min 0.01 if > 0
                if (unit_weight > 0.0)
                {
                    unit_weight = std::max(0.01, std::round(unit_weight * 100.0) / 100.0);
                }
                
                edge_map[edge_key] = {
                    {"unit", unit.id},
                    {"tg", tg_id},
                    {"unit_weight", unit_weight},
                    {"has_voice", count.voice > 0}};
            }
        }

        // Gather all talkgroup nodes
        for (const auto &[key, tg] : talkgroup_states_)
        {
            if (tg.id == 0 || tg.id == -1)
                continue;
            std::string node_id = "TG-" + std::to_string(tg.id);
            std::string label = tg.alias.empty() ? ("TG " + std::to_string(tg.id)) : tg.alias;
            std::string color = get_talkgroup_color(tg);
            // Get status using unlocked version (we already hold affiliation_state_mutex_)
            std::string status = get_talkgroup_status_unlocked(tg.wacn, tg.sysid, tg.id);
            json node_data = {
                {"id", node_id},
                {"label", label},
                {"color", color},
                {"size", 30},
                {"encryption", tg.encr_seen},
                {"status", status}};

            all_nodes[node_id] = node_data;
            node_count++;

            // Add tg->unit pairings to edge_map (reverse), and set encrypted if tg.encr_seen
            // Voice links use voice counts only; data-only links ignore tg weight (set to 0)
            for (const auto &[unit_id, count] : tg.unit_activity)
            {
                if (unit_id == 0 || unit_id == -1)
                    continue;
                std::string edge_key = "TG-" + std::to_string(tg.id) + "-" + std::to_string(unit_id);
                
                double tg_weight = 0.0;
                
                if (count.voice > 0)
                {
                    // Voice exists: calculate weight from voice transmissions only
                    tg_weight = (tg.tx_count.voice > 0) ? static_cast<double>(count.voice) / tg.tx_count.voice : 0.0;
                    
                    // Round to nearest 0.01, min 0.01 if > 0
                    if (tg_weight > 0.0)
                    {
                        tg_weight = std::max(0.01, std::round(tg_weight * 100.0) / 100.0);
                    }
                }
                // else: data only, tg_weight stays 0.0 (ignore tg side for data-only edges)
                
                if (edge_map.contains(edge_key))
                {
                    edge_map[edge_key]["tg_weight"] = tg_weight;
                    if (tg.encr_seen)
                    {
                        edge_map[edge_key]["encrypted"] = true;
                    }
                }
                else
                {
                    edge_map[edge_key] = {
                        {"unit", unit_id},
                        {"tg", tg.id},
                        {"tg_weight", tg_weight},
                        {"encrypted", tg.encr_seen},
                        {"has_voice", count.voice > 0}};
                }
            }
        }
        // Send all nodes to Gephi
        if (!all_nodes.empty())
        {
            json an_msg = {{"an", all_nodes}};
            out += an_msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
        }

        // Gather all edges with status-based coloring
        for (auto it = edge_map.begin(); it != edge_map.end(); ++it)
        {
            std::string edge_id = it.key();
            long unit_id = it.value()["unit"];
            long tg_id = it.value()["tg"];
            std::string unit_node = std::to_string(unit_id);
            std::string tg_node = "TG-" + std::to_string(tg_id);
            
            // Determine status and color based on voice/data counts
            std::string status = "affil";  // Default to affiliation
            std::string color = GEPHI_COLOR_BLUE;  // Default to blue
            bool edge_encrypted = it.value().value("encrypted", false);
            bool has_voice = it.value().value("has_voice", false);
            
            // Check if this edge has any voice transmissions
            if (has_voice)
            {
                // Grant-based edge: check encryption
                if (edge_encrypted)
                {
                    status = "e_grant";
                    color = GEPHI_COLOR_RED;
                }
                else
                {
                    status = "grant";
                    color = GEPHI_COLOR_BLACK;
                }
            }
            // Otherwise it's affiliation-only (data), status="affil", color=blue (already set)
            
            json edge_data = {
                {"source", unit_node},
                {"target", tg_node},
                {"directed", false},
                {"color", color},
                {"status", status},
                {"encryption", edge_encrypted},
                {"weight", (it.value().value("unit_weight", 0.0) + it.value().value("tg_weight", 0.0)) / 2.0}};

            all_edges[edge_id] = edge_data;
            edge_count++;
        }
        // Send all edges to Gephi
        if (!all_edges.empty())
        {
            json ae_msg = {{"ae", all_edges}};
            out += ae_msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
        }

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Sending " << node_count << " nodes and " << edge_count << " edges to new Gephi connection";
        return out;
    }

    // Gephi streaming helper functions
    std::string create_gephi_add_unit_node(System *sys, long unit_id, const std::string &unit_alpha, bool encrypted)
    {
        std::string node_id = std::to_string(unit_id);
        std::string color = get_unit_effective_color(sys, unit_id);
        std::string status = get_unit_status(sys, unit_id);

        json node_data = {
            {"id", unit_id},
            {"label", unit_alpha.empty() ? node_id : unit_alpha},
            {"color", color},
            {"status", status},
            {"encryption", encrypted},
            {"size", 15}};

        json add_node = {{"an", {{node_id, node_data}}}};
        return add_node.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
    }

    std::string create_gephi_change_unit_node(System *sys, long unit_id, const std::string &unit_alpha, bool encrypted)
    {
        std::string node_id = std::to_string(unit_id);
        std::string color = get_unit_effective_color(sys, unit_id);
        std::string status = get_unit_status(sys, unit_id);

        json node_data = {
            {"id", unit_id},
            {"label", unit_alpha.empty() ? node_id : unit_alpha},
            {"color", color},
            {"status", status},
            {"encryption", encrypted},
            {"size", 15}};

        json change_node = {{"cn", {{node_id, node_data}}}};
        return change_node.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
    }

    std::string create_gephi_add_talkgroup_node(System *sys, long tg_id, const std::string &tg_alpha, bool encrypted)
    {
        std::string node_id = "TG-" + std::to_string(tg_id);
        std::string label = tg_alpha.empty() ? std::to_string(tg_id) : tg_alpha;
        std::string status = get_talkgroup_status(sys->get_wacn(), sys->get_sys_id(), tg_id);
        std::string color = get_talkgroup_effective_color(sys, tg_id);

        json node_data = {
            {"id", node_id},
            {"label", label},
            {"color", color},
            {"status", status},
            {"encryption", encrypted},
            {"size", 30}};

        json add_node = {{"an", {{node_id, node_data}}}};
        return add_node.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
    }

    std::string create_gephi_change_talkgroup_node(System *sys, long tg_id, const std::string &tg_alpha, bool encrypted)
    {
        std::string node_id = "TG-" + std::to_string(tg_id);
        std::string label = tg_alpha.empty() ? std::to_string(tg_id) : tg_alpha;
        std::string status = get_talkgroup_status(sys->get_wacn(), sys->get_sys_id(), tg_id);
        std::string color = get_talkgroup_effective_color(sys, tg_id);

        json node_data = {
            {"id", node_id},
            {"label", label},
            {"color", color},
            {"status", status},
            {"encryption", encrypted},
            {"size", 30}};

        json change_node = {{"cn", {{node_id, node_data}}}};
        return change_node.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
    }

    std::string create_gephi_add_edge(long unit_id, long tg_id, const std::string &status, const std::string &color)
    {
        std::string unit_node = std::to_string(unit_id);
        std::string tg_node = "TG-" + std::to_string(tg_id);
        std::string edge_id = tg_node + "-" + unit_node;

        json edge_data = {
            {"source", unit_node},
            {"target", tg_node},
            {"directed", false},
            {"color", color},
            {"status", status}};

        json add_edge = {{"ae", {{edge_id, edge_data}}}};
        return add_edge.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
    }

    std::string create_gephi_change_edge(long unit_id, long tg_id, const std::string &status, const std::string &color)
    {
        std::string unit_node = std::to_string(unit_id);
        std::string tg_node = "TG-" + std::to_string(tg_id);
        std::string edge_id = tg_node + "-" + unit_node;

        json edge_data = {
            {"source", unit_node},
            {"target", tg_node},
            {"directed", false},
            {"color", color},
            {"status", status}};

        json change_edge = {{"ce", {{edge_id, edge_data}}}};
        return change_edge.dump(-1, ' ', false, json::error_handler_t::replace) + "\r\n";
    }

    void send_gephi_unit_tg_event(System *sys, long unit_id, long tg_id, bool encrypted = false)
    {
        // 0 and -1 are "no unit/talkgroup" placeholders
        if (unit_id == -1 || unit_id == 0 || tg_id == 0 || tg_id == -1)
        {
            return;
        }

        // No /graph-stream client: nothing to do (new clients get the full state)
        if (server_.raw_stream_client_count() == 0)
        {
            return;
        }

        std::string unit_alpha = sys->find_unit_tag(unit_id);

        std::string tg_alpha = "";
        Talkgroup *tg = sys->find_talkgroup(tg_id);
        if (tg)
        {
            tg_alpha = tg->alpha_tag;
        }

        // Determine edge status and color based on voice/data transmission counts
        auto [edge_status, edge_color] = get_edge_status_and_color(sys, unit_id, tg_id);

        // Send "add" and "change" together: add creates the node, change updates an existing one
        std::stringstream events;

        // Send add events (establish nodes/edges with correct initial colors)
        events << create_gephi_add_unit_node(sys, unit_id, unit_alpha, encrypted);
        events << create_gephi_add_talkgroup_node(sys, tg_id, tg_alpha, encrypted);
        events << create_gephi_add_edge(unit_id, tg_id, edge_status, edge_color);

        // Send change events (update labels and colors based on current state)
        events << create_gephi_change_unit_node(sys, unit_id, unit_alpha, encrypted);
        events << create_gephi_change_talkgroup_node(sys, tg_id, tg_alpha, encrypted);
        events << create_gephi_change_edge(unit_id, tg_id, edge_status, edge_color);

        // Queue all events together
        enqueue_graph_event(events.str());
    }

    // Send Gephi events for unit-only updates (no edges)
    void send_gephi_unit_event(System *sys, long unit_id, bool encrypted = false)
    {
        // Filter out anomalous IDs that are not valid for graph theory
        if (unit_id == -1 || unit_id == 0)
        {
            return;
        }

        // No /graph-stream client: nothing to do (new clients get the full state)
        if (server_.raw_stream_client_count() == 0)
        {
            return;
        }

        std::string unit_alpha = sys->find_unit_tag(unit_id);

        std::stringstream events;
        events << create_gephi_add_unit_node(sys, unit_id, unit_alpha, encrypted);
        events << create_gephi_change_unit_node(sys, unit_id, unit_alpha, encrypted);

        enqueue_graph_event(events.str());
    }

    // Helper function to determine edge status and color based on unit-tg relationship
    // Returns: {status, color} where status is "grant", "e_grant", or "affil"
    std::pair<std::string, std::string> get_edge_status_and_color(System *sys, long unit_id, long tg_id) const
    {
        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        
        int wacn = sys->get_wacn();
        int sysid = sys->get_sys_id();
        std::string tg_key = make_tg_key(wacn, sysid, tg_id);
        
        // Look up this unit-tg relationship in talkgroup state
        auto tg_it = talkgroup_states_.find(tg_key);
        if (tg_it != talkgroup_states_.end())
        {
            const auto &tg = tg_it->second;
            auto unit_it = tg.unit_activity.find(unit_id);
            if (unit_it != tg.unit_activity.end())
            {
                const TxCount &tx = unit_it->second;
                
                // If any voice transmissions (grants) exist
                if (tx.voice > 0)
                {
                    // Grant-based edge: check encryption
                    bool encrypted = tg.encr_seen;
                    if (encrypted)
                    {
                        return {"e_grant", GEPHI_COLOR_RED};
                    }
                    else
                    {
                        return {"grant", GEPHI_COLOR_BLACK};
                    }
                }
                // Only data transmissions (affiliations/locations)
                else if (tx.data > 0)
                {
                    return {"affil", GEPHI_COLOR_BLUE};
                }
            }
        }
        
        // Default: assume affiliation (data-only) with blue color
        return {"affil", GEPHI_COLOR_BLUE};
    }

    // Gephi streaming constants
    static constexpr const char *GEPHI_COLOR_BLUE = "#0099CC";
    static constexpr const char *GEPHI_COLOR_LT_BLUE = "#b8edff";
    static constexpr const char *GEPHI_COLOR_XLT_BLUE = "#e0f7ff";  // Extra light blue for 12hr+ idle
    static constexpr const char *GEPHI_COLOR_RED = "#cc0035";
    static constexpr const char *GEPHI_COLOR_LT_RED = "#ffb8cb";
    static constexpr const char *GEPHI_COLOR_XLT_RED = "#ffe0e8";  // Extra light red for 12hr+ idle
    static constexpr const char *GEPHI_COLOR_GREEN = "#32a852";
    static constexpr const char *GEPHI_COLOR_LT_GREEN = "#81c784";
    static constexpr const char *GEPHI_COLOR_GREY = "#808080";
    static constexpr const char *GEPHI_COLOR_BLACK = "#000000";

    // State maps (same as mqtt_status)
    std::map<short, std::string> tr_state_ = {
        {0, "MONITORING"},
        {1, "RECORDING"},
        {2, "INACTIVE"},
        {3, "ACTIVE"},
        {4, "IDLE"},
        {6, "STOPPED"},
        {7, "AVAILABLE"},
        {8, "IGNORE"}};

    // Lookup without inserting (operator[] would add an empty entry for every unknown state)
    std::string state_name(int state) const
    {
        auto it = tr_state_.find(state);
        return it != tr_state_.end() ? it->second : "";
    }

    // Message type mappings for trunk messages
    std::map<short, std::string> message_type_ = {
        {0, "GRANT"},
        {1, "STATUS"},
        {2, "UPDATE"},
        {3, "CONTROL_CHANNEL"},
        {4, "REGISTRATION"},
        {5, "DEREGISTRATION"},
        {6, "AFFILIATION"},
        {7, "SYSID"},
        {8, "ACKNOWLEDGE"},
        {9, "LOCATION"},
        {10, "PATCH_ADD"},
        {11, "PATCH_DELETE"},
        {12, "DATA_GRANT"},
        {13, "UU_ANS_REQ"},
        {14, "UU_V_GRANT"},
        {15, "UU_V_UPDATE"},
        {99, "UNKNOWN"}};

    // Custom logging backend to capture console output
    class WebLogBackend : public logging::sinks::text_ostream_backend
    {
    public:
        explicit WebLogBackend(Tr_Web &parent) : parent_(parent) {}

        static std::string severity_to_string(boost::log::trivial::severity_level sev)
        {
            switch (sev)
            {
            case boost::log::trivial::trace:
                return "trace";
            case boost::log::trivial::debug:
                return "debug";
            case boost::log::trivial::info:
                return "info";
            case boost::log::trivial::warning:
                return "warning";
            case boost::log::trivial::error:
                return "error";
            case boost::log::trivial::fatal:
                return "fatal";
            default:
                return "info";
            }
        }

        void consume(logging::record_view const &rec, std::string const &formatted_message)
        {
            // Prefer the raw Message attribute (keeps any embedded ANSI/tabs).
            // Fall back to formatted_message if Message is unavailable.
            std::string message;
            if (auto msg = rec["Message"].extract<std::string>())
            {
                message = msg.get();
            }
            else
            {
                message = formatted_message;
            }

            auto sev_attr = rec[boost::log::trivial::severity];
            auto sev = sev_attr ? sev_attr.get() : boost::log::trivial::info;
            parent_.add_console_line("[" + severity_to_string(sev) + "] " + message);
        }

    private:
        Tr_Web &parent_;
    };

public:
    Tr_Web() : running_(false) {}

    ~Tr_Web()
    {
        stop();
    }

    // ============================================================================
    // UNIT AND STATE TRACKING
    // ============================================================================

    // Update unit/talkgroup state tracking for VOICE calls (grants)
    void update_affiliation_state(System *sys, long unit_id, long tg_id, bool encrypted)
    {
        // Filter out bogon IDs
        if (unit_id == 0 || unit_id == -1 || tg_id == 0 || tg_id == -1)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        time_t now = time(NULL);

        int wacn = sys->get_wacn();
        int sysid = sys->get_sys_id();
        std::string unit_key = make_unit_key(wacn, sysid, unit_id);
        std::string tg_key = make_tg_key(wacn, sysid, tg_id);

        // Update unit state
        auto &unit = unit_states_[unit_key];
        unit.id = unit_id;
        unit.wacn = wacn;
        unit.sysid = sysid;
        if (unit.alias.empty())
        { // Only set alias if not already stored
            unit.alias = sys->find_unit_tag(unit_id);
        }
        unit.last_active = now;
        unit.registered = true; // Active transmission means registered
        unit.tx_count.voice++;  // Voice transmission
        unit.tg_activity[tg_id].voice++; // Track per-TG voice frequency
        if (encrypted)
        {
            unit.encr_seen = true;
        }

        // Update talkgroup state
        auto &tg = talkgroup_states_[tg_key];
        tg.id = tg_id;
        tg.wacn = wacn;
        tg.sysid = sysid;
        if (tg.alias.empty())
        { // Only set alias if not already stored
            Talkgroup *talkgroup = sys->find_talkgroup(tg_id);
            tg.alias = talkgroup ? talkgroup->alpha_tag : "";
        }
        tg.last_active = now;
        tg.tx_count.voice++;  // Voice transmission
        tg.unit_activity[unit_id].voice++; // Track per-unit voice frequency
        if (encrypted)
        {
            tg.encr_seen = true;
        }

        mark_affiliation_dirty_locked(unit_key, tg_key, tg_id);
    }

    // Update unit/talkgroup state tracking for DATA events (affiliations, locations)
    void update_affiliation_state_data(System *sys, long unit_id, long tg_id)
    {
        // Filter out bogon IDs
        if (unit_id == 0 || unit_id == -1 || tg_id == 0 || tg_id == -1)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        time_t now = time(NULL);

        int wacn = sys->get_wacn();
        int sysid = sys->get_sys_id();
        std::string unit_key = make_unit_key(wacn, sysid, unit_id);
        std::string tg_key = make_tg_key(wacn, sysid, tg_id);

        // Update unit state
        auto &unit = unit_states_[unit_key];
        unit.id = unit_id;
        unit.wacn = wacn;
        unit.sysid = sysid;
        if (unit.alias.empty())
        {
            unit.alias = sys->find_unit_tag(unit_id);
        }
        unit.last_active = now;
        unit.registered = true; // Active means registered
        unit.tx_count.data++;  // Data transmission
        unit.tg_activity[tg_id].data++; // Track per-TG data frequency

        // Update talkgroup state
        auto &tg = talkgroup_states_[tg_key];
        tg.id = tg_id;
        tg.wacn = wacn;
        tg.sysid = sysid;
        if (tg.alias.empty())
        {
            Talkgroup *talkgroup = sys->find_talkgroup(tg_id);
            tg.alias = talkgroup ? talkgroup->alpha_tag : "";
        }
        tg.last_active = now;
        tg.tx_count.data++;  // Data transmission
        tg.unit_activity[unit_id].data++; // Track per-unit data frequency

        mark_affiliation_dirty_locked(unit_key, tg_key, tg_id);
    }

    void set_unit_registration(System *sys, long unit_id, bool registered)
    {
        // 0 and -1 are "no unit" placeholders, not radios
        if (unit_id == 0 || unit_id == -1)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        time_t now = time(NULL);

        int wacn = sys->get_wacn();
        int sysid = sys->get_sys_id();
        std::string unit_key = make_unit_key(wacn, sysid, unit_id);

        auto &unit = unit_states_[unit_key];
        unit.id = unit_id;
        unit.wacn = wacn;
        unit.sysid = sysid;
        if (unit.alias.empty())
        {
            unit.alias = sys->find_unit_tag(unit_id);
        }
        unit.last_active = now;
        unit.registered = registered;

        mark_affiliation_dirty_locked(unit_key);
    }

    // Update unit state for non-voice events (acknowledgements, data, location, etc.)
    // This refreshes the last_active timestamp without talkgroup affiliation
    void update_unit_state(System *sys, long unit_id, bool encrypted = false)
    {
        // 0 and -1 are "no unit" placeholders, not radios
        if (unit_id == 0 || unit_id == -1)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        time_t now = time(NULL);

        int wacn = sys->get_wacn();
        int sysid = sys->get_sys_id();
        std::string unit_key = make_unit_key(wacn, sysid, unit_id);

        auto &unit = unit_states_[unit_key];
        unit.id = unit_id;
        unit.wacn = wacn;
        unit.sysid = sysid;
        if (unit.alias.empty())
        {
            unit.alias = sys->find_unit_tag(unit_id);
        }
        unit.last_active = now;
        // Note: Don't change registration status - only explicit reg/dereg messages do that
        if (encrypted)
        {
            unit.encr_seen = true;
        }

        mark_affiliation_dirty_locked(unit_key);
    }

    // Helper: Constant-time string comparison to prevent timing attacks
    bool constant_time_compare(const std::string &a, const std::string &b) const
    {
        if (a.length() != b.length())
        {
            return false;
        }
        volatile unsigned char result = 0;
        for (size_t i = 0; i < a.length(); ++i)
        {
            result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        }
        return result == 0;
    }

    // Helper: Resolve the client address used for rate limiting and login history.
    // Uses the TCP peer address; proxy headers are honoured only when the peer is a trusted proxy.
    std::string get_client_ip(const httplib::Request &req) const
    {
        const std::string &peer = req.remote_addr;
        if (std::find(trusted_proxies_.begin(), trusted_proxies_.end(), peer) == trusted_proxies_.end())
        {
            return peer;
        }

        // X-Forwarded-For is "client, proxy1, proxy2"; the right-most entry was appended by
        // the trusted proxy itself, so it is the only one the client cannot forge.
        std::string xff = req.get_header("X-Forwarded-For");
        if (!xff.empty())
        {
            size_t comma = xff.rfind(',');
            std::string last = (comma == std::string::npos) ? xff : xff.substr(comma + 1);
            size_t first = last.find_first_not_of(" \t");
            size_t end = last.find_last_not_of(" \t");
            if (first != std::string::npos)
            {
                return last.substr(first, end - first + 1);
            }
        }

        std::string real_ip = req.get_header("X-Real-IP");
        return real_ip.empty() ? peer : real_ip;
    }

    // Helper: Check if IP is rate limited
    bool is_rate_limited(const std::string &client_ip) const
    {
        std::lock_guard<std::mutex> lock(auth_rate_limit_mutex_);
        auto it = auth_attempts_.find(client_ip);
        if (it == auth_attempts_.end())
        {
            return false;
        }

        time_t now = time(NULL);
        // Count recent attempts within the time window
        size_t recent_attempts = 0;
        for (time_t attempt_time : it->second)
        {
            if (now - attempt_time < AUTH_WINDOW_SECONDS)
            {
                ++recent_attempts;
            }
        }
        return recent_attempts >= MAX_AUTH_ATTEMPTS;
    }

    // Login history: kept in memory by the server (last 50, for the admin page) and queued
    // for the database, which keeps every attempt
    void record_login_attempt(const std::string &username, const std::string &client_ip, bool success, const std::string &access_level)
    {
        server_.track_login_attempt(username, client_ip, success, access_level);
        httplib::Server::LoginAttempt attempt;
        attempt.timestamp = time(NULL);
        attempt.username = username;
        attempt.client_ip = client_ip;
        attempt.success = success;
        attempt.access_level = access_level;
        std::lock_guard<std::mutex> lock(pending_logins_mutex_);
        if (pending_logins_.size() < MAX_PENDING_LOGINS)
            pending_logins_.push_back(std::move(attempt));
    }

    // Helper: Record authentication attempt
    void record_auth_attempt(const std::string &client_ip) const
    {
        std::lock_guard<std::mutex> lock(auth_rate_limit_mutex_);
        time_t now = time(NULL);
        auto &attempts = auth_attempts_[client_ip];

        // Remove old attempts outside the window
        attempts.erase(
            std::remove_if(attempts.begin(), attempts.end(),
                           [now](time_t t)
                           { return now - t >= AUTH_WINDOW_SECONDS; }),
            attempts.end());

        attempts.push_back(now);
    }

    // Helper: Drop rate-limit entries with no attempts inside the window (bounds memory)
    void prune_auth_attempts()
    {
        std::lock_guard<std::mutex> lock(auth_rate_limit_mutex_);
        time_t now = time(NULL);
        for (auto it = auth_attempts_.begin(); it != auth_attempts_.end();)
        {
            if (it->second.empty() || now - it->second.back() >= AUTH_WINDOW_SECONDS)
            {
                it = auth_attempts_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Helper: Check if request has valid authentication
    bool check_auth(const httplib::Request &req, bool require_admin = false) const
    {
        // Read-only endpoints are open when no user credentials are set; admin endpoints only when
        // no credentials are set at all (user credentials stand in for missing admin ones)
        if (require_admin)
        {
            // No credentials at all → fully open
            if (expected_admin_creds_.empty() && expected_user_creds_.empty())
            {
                return true;
            }
        }
        else
        {
            // No user creds set → read-only is open to anonymous
            if (expected_user_creds_.empty())
            {
                return true;
            }
        }

        // Only presented credentials count toward the rate limit, or a page polling before
        // login would lock the user out
        auto auth_it = req.headers.find("Authorization");
        if (auth_it == req.headers.end() || auth_it->second.compare(0, 6, "Basic ") != 0)
        {
            return false;
        }
        const std::string &auth_header = auth_it->second;

        // Extract client IP for rate limiting and logging
        std::string client_ip = get_client_ip(req);

        // Check rate limiting
        if (is_rate_limited(client_ip))
        {
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Rate limit exceeded for " << client_ip
                                       << " on " << req.path;
            return false;
        }

        std::string provided_creds = auth_header.substr(6);

        // Validate base64 format (basic check - must contain only valid base64 characters)
        if (provided_creds.empty() ||
            provided_creds.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != std::string::npos)
        {
            record_auth_attempt(client_ip);
            BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Invalid base64 credentials from "
                                     << client_ip << " for " << req.path;
            return false;
        }

        bool auth_success = false;
        if (require_admin)
        {
            if (!expected_admin_creds_.empty())
            {
                // Admin creds configured: require them
                auth_success = constant_time_compare(provided_creds, expected_admin_creds_);
            }
            else
            {
                // No admin creds: fall back to user credentials
                auth_success = !expected_user_creds_.empty() &&
                               constant_time_compare(provided_creds, expected_user_creds_);
            }
        }
        else
        {
            // Regular endpoints accept either user or admin credentials
            auth_success = (!expected_user_creds_.empty() &&
                            constant_time_compare(provided_creds, expected_user_creds_)) ||
                           (!expected_admin_creds_.empty() &&
                            constant_time_compare(provided_creds, expected_admin_creds_));
        }

        if (!auth_success)
        {
            // Valid user credentials on an admin endpoint are a permission problem, not a guess
            bool known = (!expected_user_creds_.empty() && constant_time_compare(provided_creds, expected_user_creds_)) ||
                         (!expected_admin_creds_.empty() && constant_time_compare(provided_creds, expected_admin_creds_));
            if (!known)
                record_auth_attempt(client_ip);
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Authentication failed for "
                                       << client_ip << " on " << req.path
                                       << (require_admin ? " (admin required)" : "");
        }

        return auth_success;
    }

    // ============================================================================
    // SESSION MANAGEMENT
    // ============================================================================

    /// Generate a random session token (256 bits from the OpenSSL CSPRNG, hex encoded)
    std::string generate_session_token() const
    {
        unsigned char bytes[32];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        {
            throw std::runtime_error("RAND_bytes failed");
        }
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned char b : bytes)
        {
            ss << std::setw(2) << static_cast<int>(b);
        }
        return ss.str();
    }

    static std::string sha256_hex(const std::string &data)
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        if (EVP_Digest(data.data(), data.size(), digest, &len, EVP_sha256(), nullptr) != 1)
        {
            throw std::runtime_error("SHA-256 failed");
        }
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < len; ++i)
        {
            ss << std::setw(2) << static_cast<int>(digest[i]);
        }
        return ss.str();
    }

    // Sessions are keyed by the token's hash, so the database holds nothing usable as a cookie
    static std::string session_key(const std::string &token)
    {
        return sha256_hex(token);
    }

    /// Create a new session for a user
    std::string create_session(const std::string &username, bool is_admin)
    {
        std::string token = generate_session_token();
        std::string key = session_key(token);
        time_t now = time(NULL);

        std::lock_guard<std::mutex> lock(sessions_mutex_);

        Session session;
        session.token = key;
        session.username = username;
        session.is_admin = is_admin;
        session.created = now;
        session.last_access = now;
        session.persisted_access = now;
        
        sessions_[key] = session;
        dirty_sessions_.insert(key);
        
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Created session for " << username 
                                 << (is_admin ? " (admin)" : " (user)");
        return token;
    }

    /// Check if a session token is valid and not expired
    bool validate_session(const std::string &token, bool require_admin = false)
    {
        std::string key = session_key(token);
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        
        auto it = sessions_.find(key);
        if (it == sessions_.end())
        {
            return false;
        }
        
        time_t now = time(NULL);
        Session &session = it->second;
        
        // Check if session expired
        if (now - session.last_access > SESSION_TIMEOUT_SECONDS)
        {
            sessions_.erase(it);
            dirty_sessions_.insert(key);
            return false;
        }
        
        // Check admin requirement
        if (require_admin && !session.is_admin)
        {
            return false;
        }
        
        // Update last access time (persisted every few minutes, not on every request)
        session.last_access = now;
        if (now - session.persisted_access >= SESSION_PERSIST_ACCESS_SECONDS)
        {
            session.persisted_access = now;
            dirty_sessions_.insert(key);
        }
        return true;
    }

    /// Get session info (for whoami endpoint)
    bool get_session_info(const std::string &token, std::string &username, bool &is_admin)
    {
        std::string key = session_key(token);
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        
        auto it = sessions_.find(key);
        if (it == sessions_.end())
        {
            return false;
        }
        
        username = it->second.username;
        is_admin = it->second.is_admin;
        return true;
    }

    /// Delete a session (logout)
    void delete_session(const std::string &token)
    {
        std::string key = session_key(token);
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(key);
        dirty_sessions_.insert(key);
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Deleted session";
    }

    /// Clean up expired sessions (called periodically)
    void cleanup_expired_sessions()
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        time_t now = time(NULL);
        
        for (auto it = sessions_.begin(); it != sessions_.end();)
        {
            if (now - it->second.last_access > SESSION_TIMEOUT_SECONDS)
            {
                dirty_sessions_.insert(it->first);
                it = sessions_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Session token from "Authorization: Bearer", else the "session" cookie; empty if none
    static std::string request_session_token(const httplib::Request &req)
    {
        std::string auth_header = req.get_header("Authorization");
        if (auth_header.compare(0, 7, "Bearer ") == 0)
            return auth_header.substr(7);

        const std::string cookies = req.get_header("Cookie");
        size_t pos = 0;
        while (pos < cookies.size())
        {
            size_t end = cookies.find(';', pos);
            if (end == std::string::npos)
                end = cookies.size();
            std::string item = cookies.substr(pos, end - pos);
            size_t first = item.find_first_not_of(" \t");
            if (first != std::string::npos && item.compare(first, 8, "session=") == 0)
                return item.substr(first + 8);
            pos = end + 1;
        }
        return "";
    }

    /// Get username from request (for logging SSE connections)
    /// Checks session cookies, then Basic Auth
    std::string get_username_from_request(const httplib::Request &req) const
    {
        std::string token = request_session_token(req);
        if (!token.empty())
        {
            std::string key = session_key(token);
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(key);
            if (it != sessions_.end() && time(NULL) - it->second.last_access <= SESSION_TIMEOUT_SECONDS)
            {
                return it->second.username;
            }
        }

        // Fall back to Basic Auth
        std::string auth_header = req.get_header("Authorization");
        if (!auth_header.empty() && auth_header.find("Basic ") == 0)
        {
            std::string encoded = auth_header.substr(6);
            std::string decoded = httplib::base64_decode(encoded);
            size_t colon_pos = decoded.find(':');
            if (colon_pos != std::string::npos)
            {
                return decoded.substr(0, colon_pos);
            }
        }
        
        return "anonymous";
    }

    /// Check authentication: session token OR Basic Auth
    /// This allows both web interface (session) and external tools (Basic Auth) to work
    bool check_auth_hybrid(const httplib::Request &req, bool require_admin = false) const
    {
        // Session token (web interface): Bearer header or cookie
        std::string token = request_session_token(req);
        if (!token.empty() && const_cast<Tr_Web *>(this)->validate_session(token, require_admin))
        {
            return true;
        }

        // Fall back to HTTP Basic Auth (for SSE/graphstream and external tools)
        BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Falling back to Basic Auth for " << req.path;
        return check_auth(req, require_admin);
    }

    // require_auth()
    //   401 unless the request has a session or user credentials. No WWW-Authenticate header,
    //   so browsers show the page's login form instead of their own dialog.
    bool require_auth(const httplib::Request &req, httplib::Response &res)
    {
        if (!check_auth_hybrid(req, false))
        {
            res.status = 401;
            // Do NOT set WWW-Authenticate header - we use session-based auth with frontend login modal
            res.set_content("{\"error\": \"Authentication required\"}", "application/json");
            return false;
        }
        return true;
    }

    // require_admin_auth()
    //   As require_auth(), for admin endpoints.
    bool require_admin_auth(const httplib::Request &req, httplib::Response &res)
    {
        if (!check_auth_hybrid(req, true))
        {
            res.status = 401;
            // Do NOT set WWW-Authenticate header - we use session-based auth with frontend login modal
            res.set_content("{\"error\": \"Admin authentication required\"}", "application/json");
            return false;
        }
        return true;
    }

    // Helper: Get color for a unit based on its state (single source of truth)
    std::string get_unit_color(const UnitState &unit) const
    {
        time_t now = time(NULL);
        time_t idle_threshold_6hr = now - (6 * 3600);   // 6 hours
        time_t idle_threshold_12hr = now - (12 * 3600); // 12 hours

        // Grey if deregistered
        if (!unit.registered)
        {
            return GEPHI_COLOR_GREY;
        }
        // Extra light color if idle > 12 hours
        else if (unit.last_active < idle_threshold_12hr)
        {
            return unit.encr_seen ? GEPHI_COLOR_XLT_RED : GEPHI_COLOR_XLT_BLUE;
        }
        // Light color if idle 6-12 hours
        else if (unit.last_active < idle_threshold_6hr)
        {
            return unit.encr_seen ? GEPHI_COLOR_LT_RED : GEPHI_COLOR_LT_BLUE;
        }
        // Regular color if active < 6 hours
        else
        {
            return unit.encr_seen ? GEPHI_COLOR_RED : GEPHI_COLOR_BLUE;
        }
    }
    
    std::string get_talkgroup_color(const TalkgroupState &tg) const
    {
        time_t now = time(NULL);
        time_t idle_threshold = now - (affiliation_timeout_ * 3600);

        // Light color if idle
        if (tg.last_active < idle_threshold)
        {
            return tg.encr_seen ? GEPHI_COLOR_LT_RED : GEPHI_COLOR_LT_BLUE;
        }

        return tg.encr_seen ? GEPHI_COLOR_RED : GEPHI_COLOR_BLUE;
    }

    // get_unit_status()
    //   "u_active" (<6 h), "u_idle6" (6-12 h), "u_idle12" (>12 h) or "u_off" (deregistered);
    //   "u_enc_*" for units seen encrypted.
    std::string get_unit_status(System *sys, long unit_id) const
    {
        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        return get_unit_status_unlocked(sys->get_wacn(), sys->get_sys_id(), unit_id);
    }

    /// Internal helper: Get unit status without acquiring lock (caller must hold affiliation_state_mutex_)
    std::string get_unit_status_unlocked(int wacn, int sysid, long unit_id) const
    {
        std::string unit_key = make_unit_key(wacn, sysid, unit_id);

        auto it = unit_states_.find(unit_key);
        if (it == unit_states_.end())
        {
            return "u_unknown";
        }

        const UnitState &unit = it->second;
        time_t now = time(NULL);
        time_t idle_threshold_6hr = now - (6 * 3600);   // 6 hours
        time_t idle_threshold_12hr = now - (12 * 3600); // 12 hours
        bool encrypted = unit.encr_seen;

        // Off: deregistered units
        if (!unit.registered)
        {
            return encrypted ? "u_enc_off" : "u_off";
        }
        // Idle 12+: registered but no activity for 12+ hours
        else if (unit.last_active < idle_threshold_12hr)
        {
            return encrypted ? "u_enc_idle12" : "u_idle12";
        }
        // Idle 6-12: registered but no activity for 6-12 hours
        else if (unit.last_active < idle_threshold_6hr)
        {
            return encrypted ? "u_enc_idle6" : "u_idle6";
        }
        // Active: recent activity within 6 hours
        else
        {
            return encrypted ? "u_enc_active" : "u_active";
        }
    }

    /// Get status string for a talkgroup based on recent activity
    /// Returns: "active" (recently active), "idle" (no recent activity), or "off" (never seen)
    std::string get_talkgroup_status(int wacn, int sysid, long tg_id) const
    {
        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
        return get_talkgroup_status_unlocked(wacn, sysid, tg_id);
    }

    /// Internal helper: Get talkgroup status without acquiring lock (caller must hold affiliation_state_mutex_)
    std::string get_talkgroup_status_unlocked(int wacn, int sysid, long tg_id) const
    {
        std::string tg_key = make_tg_key(wacn, sysid, tg_id);
        auto it = talkgroup_states_.find(tg_key);
        
        if (it == talkgroup_states_.end())
        {
            return "tg_unknown";
        }

        const TalkgroupState &tg = it->second;
        time_t now = time(NULL);
        time_t idle_threshold = now - (affiliation_timeout_ * 3600);
        bool encrypted = tg.encr_seen;

        // Active: recent activity within timeout window
        if (tg.last_active > idle_threshold)
        {
            return encrypted ? "tg_enc_active" : "tg_active";
        }
        // Idle: seen before but no recent activity
        else if (tg.last_active > 0)
        {
            return encrypted ? "tg_enc_idle" : "tg_idle";
        }
        // Off: never seen activity
        else
        {
            return "tg_unknown";
        }
    }

    // Get effective color for a unit based on state (for grey-to-color transitions)
    std::string get_unit_effective_color(System *sys, long unit_id) const
    {
        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);

        int wacn = sys->get_wacn();
        int sysid = sys->get_sys_id();
        std::string unit_key = make_unit_key(wacn, sysid, unit_id);

        auto it = unit_states_.find(unit_key);
        if (it == unit_states_.end())
        {
            return GEPHI_COLOR_BLUE; // Default
        }

        return get_unit_color(it->second);
    }
    
    std::string get_talkgroup_effective_color(System *sys, long tg_id) const
    {
        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);

        int wacn = sys->get_wacn();
        int sysid = sys->get_sys_id();
        std::string tg_key = make_tg_key(wacn, sysid, tg_id);

        auto it = talkgroup_states_.find(tg_key);
        if (it == talkgroup_states_.end())
        {
            return GEPHI_COLOR_GREEN; // Default
        }

        return get_talkgroup_color(it->second);
    }

    // get_affiliation_data()
    //   Units and talkgroups for /api/affiliations. since > 0 returns only entries active at or
    //   after it; clients pass back the previous response's server_time.
    json get_affiliation_data(int limit = 0, bool units_only = false, bool talkgroups_only = false, time_t since = 0) const
    {
        // Copy under the lock (shared with trunk-recorder's thread), build JSON after
        std::map<std::string, UnitState> units_copy;
        std::map<std::string, TalkgroupState> talkgroups_copy;
        size_t total_units, total_talkgroups;
        int timeout_hours;
        time_t now;
        
        {
            std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
            now = time(NULL);
            if (!talkgroups_only) {
                if (since > 0) {
                    for (const auto &entry : unit_states_)
                        if (entry.second.last_active >= since)
                            units_copy.insert(entry);
                } else {
                    units_copy = unit_states_;
                }
            }
            if (!units_only) {
                if (since > 0) {
                    for (const auto &entry : talkgroup_states_)
                        if (entry.second.last_active >= since)
                            talkgroups_copy.insert(entry);
                } else {
                    talkgroups_copy = talkgroup_states_;
                }
            }
            total_units = unit_states_.size();
            total_talkgroups = talkgroup_states_.size();
            timeout_hours = affiliation_timeout_;
        }
        
        time_t idle_threshold = now - (timeout_hours * 3600);

        // Compact array-based format to reduce payload size
        // Schema: [id, wacn, sysid, alias, encr_seen, last_active, registered, is_idle, tx_count, activity_map]
        json result = {
            {"schema", json::object({{"units", json::array({"id", "wacn", "sysid", "alias", "encr_seen", "last_active", "registered", "is_idle", "tx_count", "tg_activity"})},
                                     {"talkgroups", json::array({"id", "wacn", "sysid", "alias", "encr_seen", "last_active", "is_idle", "tx_count", "unit_activity"})}})},
            {"units", json::array()},
            {"talkgroups", json::array()},
            {"config", {{"timeout_hours", timeout_hours}}},
            {"total_units", total_units},
            {"total_talkgroups", total_talkgroups},
            {"server_time", now},
            {"delta", since > 0}};

        if (!talkgroups_only)
        {
            int count = 0;
            for (const auto &pair : units_copy)
            {
                if (limit > 0 && count >= limit)
                    break;

                const auto &unit = pair.second;
                // Skip bogons
                if (unit.id == 0 || unit.id == -1)
                    continue;

                bool is_idle = unit.last_active < idle_threshold;

                json tg_counts = json::object();
                for (const auto &tg_pair : unit.tg_activity)
                {
                    // Skip bogon talkgroups
                    if (tg_pair.first == 0 || tg_pair.first == -1)
                        continue;
                    // Serialize TxCount as [voice, data] array
                    tg_counts[std::to_string(tg_pair.first)] = json::array({tg_pair.second.voice, tg_pair.second.data});
                }

                // Array format: [id, wacn, sysid, alias, encr_seen, last_active, registered, is_idle, tx_count, tg_activity]
                result["units"].push_back(json::array({unit.id,
                                                       unit.wacn,
                                                       unit.sysid,
                                                       unit.alias,
                                                       unit.encr_seen,
                                                       unit.last_active,
                                                       unit.registered,
                                                       is_idle,
                                                       json::array({unit.tx_count.voice, unit.tx_count.data}), // [voice, data]
                                                       tg_counts}));
                count++;
            }
        }

        if (!units_only)
        {
            int count = 0;
            for (const auto &pair : talkgroups_copy)
            {
                if (limit > 0 && count >= limit)
                    break;

                const auto &tg = pair.second;
                // Skip bogons
                if (tg.id == 0 || tg.id == -1)
                    continue;

                bool is_idle = tg.last_active < idle_threshold;

                json unit_counts = json::object();
                for (const auto &unit_pair : tg.unit_activity)
                {
                    // Skip bogon units
                    if (unit_pair.first == 0 || unit_pair.first == -1)
                        continue;
                    // Serialize TxCount as [voice, data] array
                    unit_counts[std::to_string(unit_pair.first)] = json::array({unit_pair.second.voice, unit_pair.second.data});
                }

                // Array format: [id, wacn, sysid, alias, encr_seen, last_active, is_idle, tx_count, unit_activity]
                result["talkgroups"].push_back(json::array({tg.id,
                                                            tg.wacn,
                                                            tg.sysid,
                                                            tg.alias,
                                                            tg.encr_seen,
                                                            tg.last_active,
                                                            is_idle,
                                                            json::array({tg.tx_count.voice, tg.tx_count.data}), // [voice, data]
                                                            unit_counts}));
                count++;
            }
        }

        return result;
    }

    // ============================================================================
    // AFFILIATION JSON (the pre-database format)
    // ============================================================================
    // Read once to import an earlier version's affiliations.json; written for the optional
    // affiliation_export and as the fallback when the database can't be used.

    enum class LoadResult
    {
        Loaded,
        Missing,
        Failed
    };

    static bool file_exists(const std::string &path)
    {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0;
    }

    // Write via temp file, fsync and rename, so a crash leaves the old or new file. The
    // replaced file is kept as prev_path.
    bool write_file_durably(const std::string &path, const std::string &prev_path, const std::string &text)
    {
        const std::string temp = path + ".tmp";
        FILE *f = std::fopen(temp.c_str(), "w");
        if (!f)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Cannot write " << temp << ": " << strerror(errno);
            return false;
        }
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = (std::fflush(f) == 0) && ok;
        ok = (fsync(fileno(f)) == 0) && ok;
        ok = (std::fclose(f) == 0) && ok;
        if (!ok)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Writing " << temp << " failed: " << strerror(errno) << " (" << path << " left unchanged)";
            std::remove(temp.c_str());
            return false;
        }

        // Keep the previous file
        if (file_exists(path) && std::rename(path.c_str(), prev_path.c_str()) != 0)
        {
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Cannot keep previous save as " << prev_path << ": " << strerror(errno);
        }
        if (std::rename(temp.c_str(), path.c_str()) != 0)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Cannot replace " << path << ": " << strerror(errno) << " (new data left in " << temp << ")";
            return false;
        }

        // Make the renames themselves durable
        std::string dir = ".";
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos)
            dir = slash == 0 ? "/" : path.substr(0, slash);
        int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0)
        {
            fsync(dfd);
            ::close(dfd);
        }
        return true;
    }

    // Affiliation state as JSON: affiliation_export, and the fallback when there is no database
    void export_affiliation_json(const std::string &path)
    {
        if (path.empty())
            return;

        try
        {
            // Copy under the lock; everything slow happens after it is released
            std::map<std::string, UnitState> units;
            std::map<std::string, TalkgroupState> talkgroups;
            {
                std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
                units = unit_states_;
                talkgroups = talkgroup_states_;
            }

            json persist_data = {
                {"version", 1},
                {"saved_at", time(NULL)},
                {"units", json::array()},
                {"talkgroups", json::array()}};

            for (const auto &pair : units)
            {
                const auto &unit = pair.second;
                json tg_counts = json::object();
                for (const auto &tg_pair : unit.tg_activity)
                {
                    tg_counts[std::to_string(tg_pair.first)] = json::array({tg_pair.second.voice, tg_pair.second.data});
                }
                persist_data["units"].push_back({{"id", unit.id},
                                                 {"wacn", unit.wacn},
                                                 {"sysid", unit.sysid},
                                                 {"alias", unit.alias},
                                                 {"encr_seen", unit.encr_seen},
                                                 {"last_active", unit.last_active},
                                                 {"registered", unit.registered},
                                                 {"tx_count", json::array({unit.tx_count.voice, unit.tx_count.data})},
                                                 {"tg_activity", tg_counts}});
            }

            for (const auto &pair : talkgroups)
            {
                const auto &tg = pair.second;
                json unit_counts = json::object();
                for (const auto &unit_pair : tg.unit_activity)
                {
                    unit_counts[std::to_string(unit_pair.first)] = json::array({unit_pair.second.voice, unit_pair.second.data});
                }
                persist_data["talkgroups"].push_back({{"id", tg.id},
                                                      {"wacn", tg.wacn},
                                                      {"sysid", tg.sysid},
                                                      {"alias", tg.alias},
                                                      {"encr_seen", tg.encr_seen},
                                                      {"last_active", tg.last_active},
                                                      {"tx_count", json::array({tg.tx_count.voice, tg.tx_count.data})},
                                                      {"unit_activity", unit_counts}});
            }

            std::string text = persist_data.dump(2, ' ', false, json::error_handler_t::replace); // Pretty print with 2-space indent
            if (write_file_durably(path, path + ".prev", text))
            {
                BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Exported affiliation state to " << path
                                        << " (" << units.size() << " units, " << talkgroups.size() << " talkgroups)";
            }
        }
        catch (const std::exception &e)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to export affiliation state: " << e.what();
        }
    }

    // Parse one saved file into the given maps; nothing is touched unless the whole file parses
    LoadResult load_affiliation_file(const std::string &path, std::map<std::string, UnitState> &units_out,
                                     std::map<std::string, TalkgroupState> &talkgroups_out, std::string &error) const
    {
        if (!file_exists(path))
            return LoadResult::Missing;

        try
        {
            std::ifstream in(path);
            if (!in.good())
            {
                error = std::string("cannot open: ") + strerror(errno);
                return LoadResult::Failed;
            }

            json persist_data;
            in >> persist_data;

            // Check version
            int version = persist_data.value("version", 0);
            if (version != 1)
            {
                error = "unsupported version " + std::to_string(version);
                return LoadResult::Failed;
            }

            std::map<std::string, UnitState> units;
            std::map<std::string, TalkgroupState> talkgroups;

            // Load units
            if (persist_data.contains("units"))
            {
                for (const auto &unit_json : persist_data["units"])
                {
                    UnitState unit;
                    unit.id = unit_json.value("id", 0L);
                    unit.wacn = unit_json.value("wacn", 0);
                    unit.sysid = unit_json.value("sysid", 0);
                    unit.alias = unit_json.value("alias", "");
                    unit.encr_seen = unit_json.value("encr_seen", false);
                    unit.last_active = unit_json.value("last_active", 0L);
                    unit.registered = unit_json.value("registered", false);
                    
                    // Backward compatibility: handle both old format (int) and new format ([int, int])
                    if (unit_json.contains("tx_count"))
                    {
                        if (unit_json["tx_count"].is_array())
                        {
                            auto arr = unit_json["tx_count"];
                            unit.tx_count.voice = arr.size() > 0 ? arr[0].get<int>() : 0;
                            unit.tx_count.data = arr.size() > 1 ? arr[1].get<int>() : 0;
                        }
                        else
                        {
                            // Old format: single int represents voice count only
                            unit.tx_count.voice = unit_json.value("tx_count", 0);
                            unit.tx_count.data = 0;
                        }
                    }

                    if (unit_json.contains("tg_activity"))
                    {
                        for (auto &item : unit_json["tg_activity"].items())
                        {
                            long tg_id = std::stol(item.key());
                            TxCount count;
                            
                            // Backward compatibility: handle both old and new formats
                            if (item.value().is_array())
                            {
                                auto arr = item.value();
                                count.voice = arr.size() > 0 ? arr[0].get<int>() : 0;
                                count.data = arr.size() > 1 ? arr[1].get<int>() : 0;
                            }
                            else
                            {
                                // Old format: single int represents voice count only
                                count.voice = item.value();
                                count.data = 0;
                            }
                            
                            unit.tg_activity[tg_id] = count;
                        }
                    }

                    // Recalculate tx_count from activity map (ignore stored values)
                    unit.tx_count.voice = 0;
                    unit.tx_count.data = 0;
                    for (const auto &tg_pair : unit.tg_activity)
                    {
                        unit.tx_count.voice += tg_pair.second.voice;
                        unit.tx_count.data += tg_pair.second.data;
                    }

                    std::string key = make_unit_key(unit.wacn, unit.sysid, unit.id);
                    units[key] = unit;
                }
            }

            // Load talkgroups
            if (persist_data.contains("talkgroups"))
            {
                for (const auto &tg_json : persist_data["talkgroups"])
                {
                    TalkgroupState tg;
                    tg.id = tg_json.value("id", 0L);
                    tg.wacn = tg_json.value("wacn", 0);
                    tg.sysid = tg_json.value("sysid", 0);
                    tg.alias = tg_json.value("alias", "");
                    tg.encr_seen = tg_json.value("encr_seen", false);
                    tg.last_active = tg_json.value("last_active", 0L);
                    
                    // Backward compatibility: handle both old format (int) and new format ([int, int])
                    if (tg_json.contains("tx_count"))
                    {
                        if (tg_json["tx_count"].is_array())
                        {
                            auto arr = tg_json["tx_count"];
                            tg.tx_count.voice = arr.size() > 0 ? arr[0].get<int>() : 0;
                            tg.tx_count.data = arr.size() > 1 ? arr[1].get<int>() : 0;
                        }
                        else
                        {
                            // Old format: single int represents voice count only
                            tg.tx_count.voice = tg_json.value("tx_count", 0);
                            tg.tx_count.data = 0;
                        }
                    }

                    if (tg_json.contains("unit_activity"))
                    {
                        for (auto &item : tg_json["unit_activity"].items())
                        {
                            long unit_id = std::stol(item.key());
                            TxCount count;
                            
                            // Backward compatibility: handle both old and new formats
                            if (item.value().is_array())
                            {
                                auto arr = item.value();
                                count.voice = arr.size() > 0 ? arr[0].get<int>() : 0;
                                count.data = arr.size() > 1 ? arr[1].get<int>() : 0;
                            }
                            else
                            {
                                // Old format: single int represents voice count only
                                count.voice = item.value();
                                count.data = 0;
                            }
                            
                            tg.unit_activity[unit_id] = count;
                        }
                    }

                    // Recalculate tx_count from activity map (ignore stored values)
                    tg.tx_count.voice = 0;
                    tg.tx_count.data = 0;
                    for (const auto &unit_pair : tg.unit_activity)
                    {
                        tg.tx_count.voice += unit_pair.second.voice;
                        tg.tx_count.data += unit_pair.second.data;
                    }

                    std::string key = make_tg_key(tg.wacn, tg.sysid, tg.id);
                    talkgroups[key] = tg;
                }
            }

            units_out.swap(units);
            talkgroups_out.swap(talkgroups);
            return LoadResult::Loaded;
        }
        catch (const std::exception &e)
        {
            error = e.what();
            return LoadResult::Failed;
        }
    }

    // ============================================================================
    // DATABASE (tr-web.db, vendored SQLite)
    // ============================================================================
    // The in-memory maps stay the working copy (the UI and Gephi read them). trunk-recorder's
    // threads only mark entries dirty; the database thread writes the changed ones every few
    // seconds in one transaction, with full sync, so a power cut loses at most that window.
    // A database that can't be opened or fails its integrity check is never modified: history is
    // then kept in memory and exported to <database>.fallback.json instead.

    static constexpr int DB_SCHEMA_VERSION = 4;
    static constexpr int DB_SAMPLE_RETENTION_SECONDS = 2 * 3600; // raw chart samples (charts show up to 60 min)
    static constexpr int DB_FLUSH_SECONDS = 5;
    static constexpr int DB_FALLBACK_EXPORT_SECONDS = 300;
    static constexpr int DB_BACKUP_SECONDS = 24 * 3600;

    struct LinkRow
    {
        int wacn;
        int sysid;
        long unit_id;
        long tg_id;
        TxCount count;
    };

    std::string database_fallback_path() const
    {
        return (database_path_.empty() ? std::string("tr-web") : database_path_) + ".fallback.json";
    }

    static void fsync_parent_dir(const std::string &path)
    {
        std::string dir = ".";
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos)
            dir = slash == 0 ? "/" : path.substr(0, slash);
        int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (dfd >= 0)
        {
            fsync(dfd);
            ::close(dfd);
        }
    }

    bool open_database()
    {
        if (database_path_.empty())
        {
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "No database configured: affiliation history is kept in memory and exported to "
                                       << database_fallback_path();
            return false;
        }
        const bool created = !file_exists(database_path_);
        auto db = std::make_unique<sqlite_db::Database>();
        auto refuse = [&](const std::string &why)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Cannot use database " << database_path_ << ": " << why
                                     << ". The file will not be modified; affiliation history is kept in memory and exported to "
                                     << database_fallback_path() << " until this is fixed.";
            return false;
        };
        if (!db->open(database_path_))
            return refuse(db->last_error());
        if (created)
        {
            // Holds usernames, login IPs and session hashes: owner access only
            ::chmod(database_path_.c_str(), 0600);
        }
        std::string check = db->query_text("PRAGMA quick_check");
        if (check != "ok")
            return refuse("integrity check failed (" + (check.empty() ? db->last_error() : check) + ")");
        int64_t version = db->query_int("PRAGMA user_version");
        if (version < 0)
            return refuse(db->last_error());
        if (version > DB_SCHEMA_VERSION)
            return refuse("it was created by a newer tr-web (schema " + std::to_string(version) + ")");
        if (version < DB_SCHEMA_VERSION)
        {
            // New database: create the tables
            sqlite_db::Transaction tx(*db);
            bool ok = tx.began() && db->exec(R"SQL(
                CREATE TABLE IF NOT EXISTS units (
                    wacn        INTEGER NOT NULL,
                    sysid       INTEGER NOT NULL,
                    id          INTEGER NOT NULL,
                    alias       TEXT    NOT NULL DEFAULT '',
                    encr_seen   INTEGER NOT NULL DEFAULT 0,
                    registered  INTEGER NOT NULL DEFAULT 0,
                    first_seen  INTEGER,
                    last_active INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY (wacn, sysid, id)
                ) WITHOUT ROWID;
                CREATE TABLE IF NOT EXISTS talkgroups (
                    wacn        INTEGER NOT NULL,
                    sysid       INTEGER NOT NULL,
                    id          INTEGER NOT NULL,
                    alias       TEXT    NOT NULL DEFAULT '',
                    encr_seen   INTEGER NOT NULL DEFAULT 0,
                    first_seen  INTEGER,
                    last_active INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY (wacn, sysid, id)
                ) WITHOUT ROWID;
                CREATE TABLE IF NOT EXISTS unit_talkgroups (
                    wacn         INTEGER NOT NULL,
                    sysid        INTEGER NOT NULL,
                    unit_id      INTEGER NOT NULL,
                    talkgroup_id INTEGER NOT NULL,
                    voice        INTEGER NOT NULL DEFAULT 0,
                    data         INTEGER NOT NULL DEFAULT 0,
                    first_seen   INTEGER,
                    PRIMARY KEY (wacn, sysid, unit_id, talkgroup_id)
                ) WITHOUT ROWID;
                CREATE TABLE IF NOT EXISTS meta (
                    key   TEXT PRIMARY KEY,
                    value TEXT
                ) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS sessions (
                    token_hash  TEXT    PRIMARY KEY,
                    username    TEXT    NOT NULL,
                    is_admin    INTEGER NOT NULL,
                    created     INTEGER NOT NULL,
                    last_access INTEGER NOT NULL
                ) WITHOUT ROWID;
                CREATE TABLE IF NOT EXISTS login_history (
                    id           INTEGER PRIMARY KEY,
                    timestamp    INTEGER NOT NULL,
                    username     TEXT    NOT NULL,
                    client_ip    TEXT    NOT NULL,
                    success      INTEGER NOT NULL,
                    access_level TEXT    NOT NULL
                );
                CREATE INDEX IF NOT EXISTS login_history_time ON login_history (timestamp);
CREATE TABLE IF NOT EXISTS system_samples (
                    system TEXT    NOT NULL,
                    kind   INTEGER NOT NULL,   -- 1 decode rate (msg/s), 2 active calls
                    ts     INTEGER NOT NULL,
                    value  REAL    NOT NULL
                );
                CREATE INDEX IF NOT EXISTS system_samples_ts ON system_samples (ts);
                CREATE TABLE IF NOT EXISTS system_minutes (
                    system TEXT    NOT NULL,
                    kind   INTEGER NOT NULL,
                    minute INTEGER NOT NULL,
                    sum    REAL    NOT NULL,
                    count  INTEGER NOT NULL,
                    min    REAL    NOT NULL,
                    max    REAL    NOT NULL,
                    PRIMARY KEY (system, kind, minute)
                ) WITHOUT ROWID;
                CREATE TABLE IF NOT EXISTS frequency_hours (
                    system                 TEXT    NOT NULL,
                    hour                   INTEGER NOT NULL,
                    freq                   INTEGER NOT NULL,
                    calls                  INTEGER NOT NULL DEFAULT 0,
                    transmissions          INTEGER NOT NULL DEFAULT 0,
                    seconds                REAL    NOT NULL DEFAULT 0,
                    errors                 INTEGER NOT NULL DEFAULT 0,
                    spikes                 INTEGER NOT NULL DEFAULT 0,
                    phase2_calls           INTEGER NOT NULL DEFAULT 0,
                    freq_error_sum         REAL    NOT NULL DEFAULT 0,
                    freq_error_count       INTEGER NOT NULL DEFAULT 0,
                    last_seen              INTEGER NOT NULL DEFAULT 0,
                    voice_bits             REAL    NOT NULL DEFAULT 0,
                    PRIMARY KEY (system, hour, freq)
                ) WITHOUT ROWID;
                CREATE TABLE IF NOT EXISTS talkgroup_hours (
                    system    TEXT    NOT NULL,
                    hour      INTEGER NOT NULL,
                    talkgroup INTEGER NOT NULL,
                    alpha_tag TEXT    NOT NULL DEFAULT '',
                    calls     INTEGER NOT NULL DEFAULT 0,
                    seconds   REAL    NOT NULL DEFAULT 0,
                    encrypted INTEGER NOT NULL DEFAULT 0,
                    emergency INTEGER NOT NULL DEFAULT 0,
                    errors    INTEGER NOT NULL DEFAULT 0,
                    last_seen INTEGER NOT NULL DEFAULT 0,
                    spikes     INTEGER NOT NULL DEFAULT 0,
                    voice_bits REAL    NOT NULL DEFAULT 0,
                    PRIMARY KEY (system, hour, talkgroup)
                ) WITHOUT ROWID;
                CREATE TABLE IF NOT EXISTS unit_hours (
                    system        TEXT    NOT NULL,
                    hour          INTEGER NOT NULL,
                    unit          INTEGER NOT NULL,
                    alias         TEXT    NOT NULL DEFAULT '',
                    transmissions INTEGER NOT NULL DEFAULT 0,
                    seconds       REAL    NOT NULL DEFAULT 0,
                    errors        INTEGER NOT NULL DEFAULT 0,
                    spikes        INTEGER NOT NULL DEFAULT 0,
                    voice_bits    REAL    NOT NULL DEFAULT 0,
                    last_seen     INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY (system, hour, unit)
                ) WITHOUT ROWID;
            )SQL") && db->exec("PRAGMA user_version = " + std::to_string(DB_SCHEMA_VERSION)) && tx.commit();
            if (!ok)
                return refuse("creating tables failed: " + db->last_error());
            if (version == 0)
                BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Created database " << database_path_;
        }
        // Refuse a database missing columns this version writes, rather than fail every write
        for (const char *probe : {"SELECT voice_bits FROM frequency_hours LIMIT 0",
                                  "SELECT spikes, voice_bits FROM talkgroup_hours LIMIT 0",
                                  "SELECT alias, transmissions, seconds, errors, spikes, voice_bits FROM unit_hours LIMIT 0"})
        {
            if (!db->prepare(probe).ok())
                return refuse(std::string("its tables don't match this tr-web version (") + db->last_error() + ")");
        }
        db_ = std::move(db);

        // Web requests read through their own connection
        auto reader = std::make_unique<sqlite_db::Database>();
        if (reader->open(database_path_))
            db_read_ = std::move(reader);
        else
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Cannot open a read connection to " << database_path_ << ": "
                                       << reader->last_error() << " (history windows unavailable)";
        return true;
    }

    // Restore the last hour of chart samples (decode rate, active calls) into memory
    void load_samples_from_db()
    {
        std::map<std::string, std::string> unique_names; // database key -> "N. short name"
        for (auto *sys : tr_systems_)
            unique_names[db_system_key(sys->get_sys_num())] = get_unique_sys_name(sys);

        time_t cutoff = time(NULL) - CALL_RATE_RETENTION_SECONDS;
        auto q = db_->prepare("SELECT system, kind, ts, value FROM system_samples WHERE ts >= ?1 ORDER BY ts");
        q.bind(1, (int64_t)cutoff);
        size_t restored = 0;
        std::lock_guard<std::mutex> lock(data_mutex_);
        while (q.ok() && q.step() == SQLITE_ROW)
        {
            auto it = unique_names.find(q.col_text(0));
            if (it == unique_names.end())
                continue; // system no longer configured
            RatePoint point;
            point.timestamp = (time_t)q.col_int(2);
            point.rate = q.col_double(3);
            if (q.col_int(1) == SAMPLE_DECODE_RATE)
            {
                auto &history = rate_history_[it->second];
                history.push_back(point);
                while (history.size() > MAX_RATE_HISTORY)
                    history.pop_front();
            }
            else
            {
                call_rate_history_[it->second].push_back(point);
            }
            restored++;
        }
        if (restored > 0)
        {
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Restored " << restored << " chart samples from the last hour";
        }
    }

    // Chart samples and hourly call statistics. Failed writes are requeued.
    void flush_stats_to_db()
    {
        std::vector<PendingSample> samples;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            samples.swap(pending_samples_);
        }
        decltype(pending_freq_hours_) freq_hours;
        decltype(pending_tg_hours_) tg_hours;
        decltype(pending_unit_hours_) unit_hours;
        {
            std::lock_guard<std::mutex> lock(system_stats_mutex_);
            freq_hours.swap(pending_freq_hours_);
            tg_hours.swap(pending_tg_hours_);
            unit_hours.swap(pending_unit_hours_);
        }

        time_t now = time(NULL);
        bool prune = now - last_sample_prune_ >= 60;
        if (samples.empty() && freq_hours.empty() && tg_hours.empty() && unit_hours.empty() && !prune)
            return;

        bool ok = false;
        {
            sqlite_db::Transaction tx(*db_);
            auto raw = db_->prepare("INSERT INTO system_samples (system, kind, ts, value) VALUES (?1, ?2, ?3, ?4)");
            auto minute = db_->prepare(R"SQL(
                INSERT INTO system_minutes (system, kind, minute, sum, count, min, max) VALUES (?1, ?2, ?3, ?4, 1, ?4, ?4)
                ON CONFLICT (system, kind, minute) DO UPDATE SET
                    sum = sum + excluded.sum, count = count + 1,
                    min = MIN(min, excluded.min), max = MAX(max, excluded.max))SQL");
            auto fh = db_->prepare(R"SQL(
                INSERT INTO frequency_hours (system, hour, freq, calls, transmissions, seconds, errors, spikes,
                    phase2_calls, freq_error_sum, freq_error_count, last_seen, voice_bits)
                VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)
                ON CONFLICT (system, hour, freq) DO UPDATE SET
                    calls = calls + excluded.calls, transmissions = transmissions + excluded.transmissions,
                    seconds = seconds + excluded.seconds, errors = errors + excluded.errors, spikes = spikes + excluded.spikes,
                    phase2_calls = phase2_calls + excluded.phase2_calls,
                    freq_error_sum = freq_error_sum + excluded.freq_error_sum,
                    freq_error_count = freq_error_count + excluded.freq_error_count,
                    last_seen = MAX(last_seen, excluded.last_seen),
                    voice_bits = voice_bits + excluded.voice_bits)SQL");
            auto th = db_->prepare(R"SQL(
                INSERT INTO talkgroup_hours (system, hour, talkgroup, alpha_tag, calls, seconds, encrypted, emergency, errors, last_seen, spikes, voice_bits)
                VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)
                ON CONFLICT (system, hour, talkgroup) DO UPDATE SET
                    alpha_tag = CASE WHEN excluded.alpha_tag <> '' THEN excluded.alpha_tag ELSE alpha_tag END,
                    calls = calls + excluded.calls, seconds = seconds + excluded.seconds,
                    encrypted = encrypted + excluded.encrypted, emergency = emergency + excluded.emergency,
                    errors = errors + excluded.errors, last_seen = MAX(last_seen, excluded.last_seen),
                    spikes = spikes + excluded.spikes, voice_bits = voice_bits + excluded.voice_bits)SQL");
            auto uh = db_->prepare(R"SQL(
                INSERT INTO unit_hours (system, hour, unit, alias, transmissions, seconds, errors, spikes, voice_bits, last_seen)
                VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)
                ON CONFLICT (system, hour, unit) DO UPDATE SET
                    alias = CASE WHEN excluded.alias <> '' THEN excluded.alias ELSE alias END,
                    transmissions = transmissions + excluded.transmissions, seconds = seconds + excluded.seconds,
                    errors = errors + excluded.errors, spikes = spikes + excluded.spikes,
                    voice_bits = voice_bits + excluded.voice_bits, last_seen = MAX(last_seen, excluded.last_seen))SQL");
            ok = tx.began() && raw.ok() && minute.ok() && fh.ok() && th.ok() && uh.ok();
            for (size_t i = 0; ok && i < samples.size(); ++i)
            {
                const auto &sm = samples[i];
                ok = raw.bind(1, sm.system).bind(2, sm.kind).bind(3, (int64_t)sm.ts).bind(4, sm.value).run() &&
                     minute.bind(1, sm.system).bind(2, sm.kind).bind(3, (int64_t)(sm.ts - sm.ts % 60)).bind(4, sm.value).run();
            }
            for (auto it = freq_hours.begin(); ok && it != freq_hours.end(); ++it)
            {
                const auto &[key, f] = *it;
                fh.bind(1, std::get<0>(key)).bind(2, std::get<1>(key)).bind(3, (int64_t)std::get<2>(key))
                    .bind(4, (int64_t)f.all.calls).bind(5, (int64_t)f.all.transmissions).bind(6, f.all.seconds)
                    .bind(7, (int64_t)f.all.errors).bind(8, (int64_t)f.all.spikes)
                    .bind(9, (int64_t)f.phase2_calls).bind(10, f.freq_error_sum).bind(11, (int64_t)f.freq_error_count)
                    .bind(12, (int64_t)f.last_seen).bind(13, f.all.voice_bits);
                ok = fh.run();
            }
            for (auto it = tg_hours.begin(); ok && it != tg_hours.end(); ++it)
            {
                const auto &[key, t] = *it;
                th.bind(1, std::get<0>(key)).bind(2, std::get<1>(key)).bind(3, (int64_t)std::get<2>(key)).bind(4, t.alpha_tag)
                    .bind(5, (int64_t)t.calls).bind(6, t.seconds).bind(7, (int64_t)t.encrypted).bind(8, (int64_t)t.emergency)
                    .bind(9, (int64_t)t.errors).bind(10, (int64_t)t.last_seen).bind(11, (int64_t)t.spikes).bind(12, t.voice_bits);
                ok = th.run();
            }
            for (auto it = unit_hours.begin(); ok && it != unit_hours.end(); ++it)
            {
                const auto &[key, u] = *it;
                uh.bind(1, std::get<0>(key)).bind(2, std::get<1>(key)).bind(3, (int64_t)std::get<2>(key)).bind(4, u.alias)
                    .bind(5, (int64_t)u.transmissions).bind(6, u.seconds).bind(7, (int64_t)u.errors).bind(8, (int64_t)u.spikes)
                    .bind(9, u.voice_bits).bind(10, (int64_t)u.last_seen);
                ok = uh.run();
            }
            if (ok && prune)
            {
                auto del = db_->prepare("DELETE FROM system_samples WHERE ts < ?1");
                ok = del.ok() && del.bind(1, (int64_t)(now - DB_SAMPLE_RETENTION_SECONDS)).run();
            }
            ok = ok && tx.commit();
        }
        if (ok)
        {
            if (prune)
                last_sample_prune_ = now;
            return;
        }

        // Requeue (merging with anything added meanwhile)
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            samples.insert(samples.end(), pending_samples_.begin(), pending_samples_.end());
            pending_samples_.swap(samples);
            if (pending_samples_.size() > MAX_PENDING_SAMPLES)
                pending_samples_.erase(pending_samples_.begin(), pending_samples_.end() - MAX_PENDING_SAMPLES);
        }
        {
            std::lock_guard<std::mutex> lock(system_stats_mutex_);
            for (const auto &[key, f] : freq_hours)
                merge_stats(pending_freq_hours_[key], f);
            for (const auto &[key, t] : tg_hours)
                merge_stats(pending_tg_hours_[key], t);
            for (const auto &[key, u] : unit_hours)
                merge_stats(pending_unit_hours_[key], u);
        }
        BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Writing statistics to " << database_path_ << " failed (" << db_->last_error() << "); will retry.";
    }

    // Salted PBKDF2 of the configured credentials. Saved sessions are dropped when it changes.
    std::string auth_fingerprint(const std::string &salt) const
    {
        const std::string creds = expected_user_creds_ + "\x1f" + expected_admin_creds_;
        unsigned char out[32];
        if (PKCS5_PBKDF2_HMAC(creds.data(), (int)creds.size(), reinterpret_cast<const unsigned char *>(salt.data()), (int)salt.size(),
                              200000, EVP_sha256(), sizeof(out), out) != 1)
        {
            throw std::runtime_error("PBKDF2 failed");
        }
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned char b : out)
            ss << std::setw(2) << static_cast<int>(b);
        return ss.str();
    }

    void load_sessions_from_db()
    {
        time_t now = time(NULL);
        std::string salt = db_->query_text("SELECT value FROM meta WHERE key = 'auth_salt'");
        if (salt.empty())
        {
            salt = generate_session_token(); // 256 random bits, hex
            auto put = db_->prepare("INSERT OR REPLACE INTO meta (key, value) VALUES ('auth_salt', ?1)");
            put.bind(1, salt).run();
        }
        std::string fingerprint = auth_fingerprint(salt);
        std::string saved = db_->query_text("SELECT value FROM meta WHERE key = 'auth_fingerprint'");
        if (saved != fingerprint)
        {
            int64_t discarded = db_->query_int("SELECT count(*) FROM sessions", 0);
            db_->exec("DELETE FROM sessions");
            auto meta = db_->prepare("INSERT OR REPLACE INTO meta (key, value) VALUES ('auth_fingerprint', ?1)");
            meta.bind(1, fingerprint).run();
            if (discarded > 0)
            {
                BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Credentials changed since the last run: discarded " << discarded << " saved sessions";
            }
        }
        auto expire = db_->prepare("DELETE FROM sessions WHERE last_access < ?1");
        expire.bind(1, (int64_t)(now - SESSION_TIMEOUT_SECONDS)).run();

        std::map<std::string, Session> sessions;
        auto q = db_->prepare("SELECT token_hash, username, is_admin, created, last_access FROM sessions");
        while (q.ok() && q.step() == SQLITE_ROW)
        {
            Session session;
            session.token = q.col_text(0);
            session.username = q.col_text(1);
            session.is_admin = q.col_int(2) != 0;
            session.created = (time_t)q.col_int(3);
            session.last_access = (time_t)q.col_int(4);
            session.persisted_access = session.last_access;
            sessions[session.token] = std::move(session);
        }
        size_t count = sessions.size();
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.swap(sessions);
        }
        if (count > 0)
        {
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Restored " << count << " web sessions";
        }
    }

    // The admin page shows the last 50 attempts; the database keeps all of them
    void load_login_history_from_db()
    {
        auto q = db_->prepare("SELECT timestamp, username, client_ip, success, access_level FROM "
                              "(SELECT * FROM login_history ORDER BY id DESC LIMIT 50) ORDER BY id");
        while (q.ok() && q.step() == SQLITE_ROW)
        {
            server_.track_login_attempt(q.col_text(1), q.col_text(2), q.col_int(3) != 0, q.col_text(4), (time_t)q.col_int(0));
        }
    }

    // Write sessions created/changed/removed since the last flush, and queued login attempts.
    // Failed writes are requeued.
    void flush_sessions_and_logins_to_db()
    {
        std::unordered_set<std::string> keys;
        std::vector<Session> upserts;
        std::vector<std::string> deletes;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            keys.swap(dirty_sessions_);
            for (const auto &key : keys)
            {
                auto it = sessions_.find(key);
                if (it != sessions_.end())
                    upserts.push_back(it->second);
                else
                    deletes.push_back(key);
            }
        }
        std::vector<httplib::Server::LoginAttempt> logins;
        {
            std::lock_guard<std::mutex> lock(pending_logins_mutex_);
            logins.swap(pending_logins_);
        }
        if (upserts.empty() && deletes.empty() && logins.empty())
            return;

        bool ok = false;
        {
            sqlite_db::Transaction tx(*db_);
            auto up = db_->prepare(R"SQL(
                INSERT INTO sessions (token_hash, username, is_admin, created, last_access)
                VALUES (?1, ?2, ?3, ?4, ?5)
                ON CONFLICT (token_hash) DO UPDATE SET last_access = MAX(last_access, excluded.last_access))SQL");
            auto del = db_->prepare("DELETE FROM sessions WHERE token_hash = ?1");
            auto log = db_->prepare("INSERT INTO login_history (timestamp, username, client_ip, success, access_level) VALUES (?1, ?2, ?3, ?4, ?5)");
            ok = tx.began() && up.ok() && del.ok() && log.ok();
            for (size_t i = 0; ok && i < upserts.size(); ++i)
            {
                const Session &se = upserts[i];
                ok = up.bind(1, se.token).bind(2, se.username).bind(3, se.is_admin)
                         .bind(4, (int64_t)se.created).bind(5, (int64_t)se.last_access).run();
            }
            for (size_t i = 0; ok && i < deletes.size(); ++i)
            {
                ok = del.bind(1, deletes[i]).run();
            }
            for (size_t i = 0; ok && i < logins.size(); ++i)
            {
                const auto &a = logins[i];
                ok = log.bind(1, (int64_t)a.timestamp).bind(2, a.username).bind(3, a.client_ip)
                         .bind(4, a.success).bind(5, a.access_level).run();
            }
            ok = ok && tx.commit();
        }
        if (ok)
            return;

        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            dirty_sessions_.insert(keys.begin(), keys.end());
        }
        {
            std::lock_guard<std::mutex> lock(pending_logins_mutex_);
            logins.insert(logins.end(), pending_logins_.begin(), pending_logins_.end());
            pending_logins_.swap(logins);
            if (pending_logins_.size() > MAX_PENDING_LOGINS)
                pending_logins_.erase(pending_logins_.begin(), pending_logins_.end() - MAX_PENDING_LOGINS);
        }
        BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Writing sessions/login history to " << database_path_
                                 << " failed (" << db_->last_error() << "); will retry.";
    }

    // Upsert units, talkgroups and links in one transaction. Counts only grow, so MAX() keeps
    // the newer value. first_seen is NULL for rows imported from affiliations.json.
    bool write_affiliation_rows(const std::vector<UnitState> &units, const std::vector<TalkgroupState> &talkgroups,
                                const std::vector<LinkRow> &links, bool set_first_seen)
    {
        time_t now = time(NULL);
        sqlite_db::Transaction tx(*db_);
        if (!tx.began())
            return false;
        auto unit_stmt = db_->prepare(R"SQL(
            INSERT INTO units (wacn, sysid, id, alias, encr_seen, registered, first_seen, last_active)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
            ON CONFLICT (wacn, sysid, id) DO UPDATE SET
                alias = excluded.alias,
                encr_seen = MAX(encr_seen, excluded.encr_seen),
                registered = excluded.registered,
                last_active = MAX(last_active, excluded.last_active))SQL");
        auto tg_stmt = db_->prepare(R"SQL(
            INSERT INTO talkgroups (wacn, sysid, id, alias, encr_seen, first_seen, last_active)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
            ON CONFLICT (wacn, sysid, id) DO UPDATE SET
                alias = excluded.alias,
                encr_seen = MAX(encr_seen, excluded.encr_seen),
                last_active = MAX(last_active, excluded.last_active))SQL");
        auto link_stmt = db_->prepare(R"SQL(
            INSERT INTO unit_talkgroups (wacn, sysid, unit_id, talkgroup_id, voice, data, first_seen)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
            ON CONFLICT (wacn, sysid, unit_id, talkgroup_id) DO UPDATE SET
                voice = MAX(voice, excluded.voice),
                data = MAX(data, excluded.data))SQL");
        if (!unit_stmt.ok() || !tg_stmt.ok() || !link_stmt.ok())
            return false;

        for (const auto &u : units)
        {
            unit_stmt.bind(1, u.wacn).bind(2, u.sysid).bind(3, (int64_t)u.id).bind(4, u.alias)
                .bind(5, u.encr_seen).bind(6, u.registered).bind(8, (int64_t)u.last_active);
            if (set_first_seen)
                unit_stmt.bind(7, (int64_t)now);
            else
                unit_stmt.bind_null(7);
            if (!unit_stmt.run())
                return false;
        }
        for (const auto &t : talkgroups)
        {
            tg_stmt.bind(1, t.wacn).bind(2, t.sysid).bind(3, (int64_t)t.id).bind(4, t.alias)
                .bind(5, t.encr_seen).bind(7, (int64_t)t.last_active);
            if (set_first_seen)
                tg_stmt.bind(6, (int64_t)now);
            else
                tg_stmt.bind_null(6);
            if (!tg_stmt.run())
                return false;
        }
        for (const auto &l : links)
        {
            link_stmt.bind(1, l.wacn).bind(2, l.sysid).bind(3, (int64_t)l.unit_id).bind(4, (int64_t)l.tg_id)
                .bind(5, l.count.voice).bind(6, l.count.data);
            if (set_first_seen)
                link_stmt.bind(7, (int64_t)now);
            else
                link_stmt.bind_null(7);
            if (!link_stmt.run())
                return false;
        }
        return tx.commit();
    }

    // One-time import of the pre-database affiliations.json (affiliation_cache), and only into
    // an empty database. The JSON file is renamed to *.imported afterwards, never deleted.
    void import_legacy_affiliations()
    {
        if (!db_ || affiliation_cache_.empty())
            return;
        if (db_->query_int("SELECT EXISTS (SELECT 1 FROM units) OR EXISTS (SELECT 1 FROM talkgroups)", 1) != 0)
            return;

        std::map<std::string, UnitState> units;
        std::map<std::string, TalkgroupState> talkgroups;
        std::string error;
        std::string source = affiliation_cache_;
        LoadResult result = load_affiliation_file(source, units, talkgroups, error);
        if (result == LoadResult::Failed)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Cannot import " << source << " (" << error << "); it is left untouched.";
        }
        if (result != LoadResult::Loaded)
            return;

        // Both sides of every unit-talkgroup pair were saved; MAX() in the upsert merges them
        std::vector<UnitState> unit_rows;
        std::vector<TalkgroupState> tg_rows;
        std::vector<LinkRow> links;
        for (const auto &[key, u] : units)
        {
            unit_rows.push_back(u);
            for (const auto &[tg_id, count] : u.tg_activity)
                links.push_back({u.wacn, u.sysid, u.id, tg_id, count});
        }
        for (const auto &[key, t] : talkgroups)
        {
            tg_rows.push_back(t);
            for (const auto &[unit_id, count] : t.unit_activity)
                links.push_back({t.wacn, t.sysid, unit_id, t.id, count});
        }
        if (!write_affiliation_rows(unit_rows, tg_rows, links, false))
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Importing " << source << " into " << database_path_ << " failed ("
                                     << db_->last_error() << "); the JSON file is left untouched and the import will be retried at the next start.";
            return;
        }
        auto meta = db_->prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?1, ?2)");
        meta.bind(1, std::string("imported_from")).bind(2, source).run();
        meta.bind(1, std::string("imported_at")).bind(2, std::to_string(time(NULL))).run();

        std::string done = source + ".imported";
        if (std::rename(source.c_str(), done.c_str()) == 0)
        {
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Imported " << units.size() << " units and " << talkgroups.size()
                                    << " talkgroups from " << source << " into " << database_path_ << "; the file is kept as " << done;
        }
        else
        {
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Imported " << source << " into " << database_path_
                                       << " but could not rename it (" << strerror(errno) << "); it will not be imported again.";
        }
    }

    bool load_affiliations_from_db()
    {
        std::map<std::string, UnitState> units;
        std::map<std::string, TalkgroupState> talkgroups;

        auto u = db_->prepare("SELECT wacn, sysid, id, alias, encr_seen, registered, last_active FROM units");
        if (!u.ok())
            return false;
        while (u.step() == SQLITE_ROW)
        {
            UnitState unit;
            unit.wacn = (int)u.col_int(0);
            unit.sysid = (int)u.col_int(1);
            unit.id = (long)u.col_int(2);
            unit.alias = u.col_text(3);
            unit.encr_seen = u.col_int(4) != 0;
            unit.registered = u.col_int(5) != 0;
            unit.last_active = (time_t)u.col_int(6);
            units[make_unit_key(unit.wacn, unit.sysid, unit.id)] = std::move(unit);
        }

        auto t = db_->prepare("SELECT wacn, sysid, id, alias, encr_seen, last_active FROM talkgroups");
        if (!t.ok())
            return false;
        while (t.step() == SQLITE_ROW)
        {
            TalkgroupState tg;
            tg.wacn = (int)t.col_int(0);
            tg.sysid = (int)t.col_int(1);
            tg.id = (long)t.col_int(2);
            tg.alias = t.col_text(3);
            tg.encr_seen = t.col_int(4) != 0;
            tg.last_active = (time_t)t.col_int(5);
            talkgroups[make_tg_key(tg.wacn, tg.sysid, tg.id)] = std::move(tg);
        }

        auto l = db_->prepare("SELECT wacn, sysid, unit_id, talkgroup_id, voice, data FROM unit_talkgroups");
        if (!l.ok())
            return false;
        while (l.step() == SQLITE_ROW)
        {
            int wacn = (int)l.col_int(0), sysid = (int)l.col_int(1);
            long unit_id = (long)l.col_int(2), tg_id = (long)l.col_int(3);
            TxCount count;
            count.voice = (int)l.col_int(4);
            count.data = (int)l.col_int(5);
            auto uit = units.find(make_unit_key(wacn, sysid, unit_id));
            if (uit != units.end())
                uit->second.tg_activity[tg_id] = count;
            auto tit = talkgroups.find(make_tg_key(wacn, sysid, tg_id));
            if (tit != talkgroups.end())
                tit->second.unit_activity[unit_id] = count;
        }

        // Totals are derived from the pair counts, as the JSON loader did
        for (auto &[key, unit] : units)
            for (const auto &[tg_id, count] : unit.tg_activity)
            {
                unit.tx_count.voice += count.voice;
                unit.tx_count.data += count.data;
            }
        for (auto &[key, tg] : talkgroups)
            for (const auto &[unit_id, count] : tg.unit_activity)
            {
                tg.tx_count.voice += count.voice;
                tg.tx_count.data += count.data;
            }

        size_t unit_count = units.size(), tg_count = talkgroups.size();
        {
            std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
            unit_states_.swap(units);
            talkgroup_states_.swap(talkgroups);
        }
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Loaded " << unit_count << " units and " << tg_count
                                << " talkgroups from " << database_path_;
        return true;
    }

    // Write everything changed since the last flush; on failure, mark it dirty again
    void flush_affiliations_to_db()
    {
        std::unordered_set<std::string> units_dirty, tgs_dirty;
        std::set<std::pair<std::string, long>> links_dirty;
        std::vector<UnitState> units;
        std::vector<TalkgroupState> talkgroups;
        std::vector<LinkRow> links;
        {
            std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
            units_dirty.swap(dirty_units_);
            tgs_dirty.swap(dirty_talkgroups_);
            links_dirty.swap(dirty_links_);
            for (const auto &key : units_dirty)
            {
                auto it = unit_states_.find(key);
                if (it == unit_states_.end())
                    continue;
                UnitState copy; // without the activity map, which is written as links
                copy.id = it->second.id;
                copy.wacn = it->second.wacn;
                copy.sysid = it->second.sysid;
                copy.alias = it->second.alias;
                copy.encr_seen = it->second.encr_seen;
                copy.registered = it->second.registered;
                copy.last_active = it->second.last_active;
                units.push_back(std::move(copy));
            }
            for (const auto &key : tgs_dirty)
            {
                auto it = talkgroup_states_.find(key);
                if (it == talkgroup_states_.end())
                    continue;
                TalkgroupState copy;
                copy.id = it->second.id;
                copy.wacn = it->second.wacn;
                copy.sysid = it->second.sysid;
                copy.alias = it->second.alias;
                copy.encr_seen = it->second.encr_seen;
                copy.last_active = it->second.last_active;
                talkgroups.push_back(std::move(copy));
            }
            for (const auto &[unit_key, tg_id] : links_dirty)
            {
                auto it = unit_states_.find(unit_key);
                if (it == unit_states_.end())
                    continue;
                auto cit = it->second.tg_activity.find(tg_id);
                if (cit != it->second.tg_activity.end())
                    links.push_back({it->second.wacn, it->second.sysid, it->second.id, tg_id, cit->second});
            }
        }
        if (units.empty() && talkgroups.empty() && links.empty())
            return;

        if (write_affiliation_rows(units, talkgroups, links, true))
        {
            db_failures_ = 0;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
            dirty_units_.insert(units_dirty.begin(), units_dirty.end());
            dirty_talkgroups_.insert(tgs_dirty.begin(), tgs_dirty.end());
            dirty_links_.insert(links_dirty.begin(), links_dirty.end());
        }
        // Log the first failure and then every 5 minutes, not every 5 seconds
        if (db_failures_++ % (300 / DB_FLUSH_SECONDS) == 0)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Writing to " << database_path_ << " failed (" << db_->last_error()
                                     << "); changes are kept in memory and retried.";
        }
    }

    // Daily consistent copy to <database>.bak, made while running
    void backup_database_if_due()
    {
        const std::string bak = database_path_ + ".bak";
        time_t now = time(NULL);
        if (last_db_backup_ == 0)
        {
            struct stat st;
            last_db_backup_ = (::stat(bak.c_str(), &st) == 0) ? st.st_mtime : 1;
        }
        if (now - last_db_backup_ < DB_BACKUP_SECONDS)
            return;
        last_db_backup_ = now;

        const std::string tmp = bak + ".tmp";
        std::remove(tmp.c_str());
        if (!db_->backup_to(tmp))
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Database backup to " << tmp << " failed: " << db_->last_error();
            std::remove(tmp.c_str());
            return;
        }
        // Same contents as the database (session hashes, login history): owner access only
        ::chmod(tmp.c_str(), 0600);
        if (std::rename(tmp.c_str(), bak.c_str()) != 0)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Cannot replace " << bak << ": " << strerror(errno);
            return;
        }
        fsync_parent_dir(bak);
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Backed up " << database_path_ << " to " << bak;
    }

    void database_loop()
    {
        time_t last_export = time(NULL);
        time_t last_fallback = 0;
        std::unique_lock<std::mutex> lock(db_wake_mutex_);
        while (!db_stop_)
        {
            db_wake_.wait_for(lock, std::chrono::seconds(DB_FLUSH_SECONDS), [this]
                              { return db_stop_; });
            if (db_stop_)
                break;
            lock.unlock();
            time_t now = time(NULL);
            if (db_)
            {
                flush_affiliations_to_db();
                flush_sessions_and_logins_to_db();
                flush_stats_to_db();
                backup_database_if_due();
            }
            else if (now - last_fallback >= DB_FALLBACK_EXPORT_SECONDS)
            {
                export_affiliation_json(database_fallback_path());
                last_fallback = now;
            }
            if (!affiliation_export_.empty() && now - last_export >= affiliation_export_interval_)
            {
                export_affiliation_json(affiliation_export_);
                last_export = now;
            }
            lock.lock();
        }
    }

    // Final flush and exports at shutdown
    void close_database()
    {
        if (db_)
        {
            flush_affiliations_to_db();
            flush_sessions_and_logins_to_db();
            flush_stats_to_db();
            if (db_read_)
            {
                db_read_->close();
                db_read_.reset();
            }
            db_->close();
            db_.reset();
        }
        else
        {
            export_affiliation_json(database_fallback_path());
        }
        if (!affiliation_export_.empty())
        {
            export_affiliation_json(affiliation_export_);
        }
    }

    // Generate display name for system with number prefix
    std::string get_unique_sys_name(System *sys)
    {
        int sys_num = sys->get_sys_num();
        std::string short_name = sys->get_short_name();
        return std::to_string(sys_num + 1) + ". " + short_name;
    }

    void add_rate_point(const std::string &sys_name, const std::string &short_name, double rate)
    {
        std::lock_guard<std::mutex> lock(data_mutex_);

        RatePoint point;
        point.timestamp = time(NULL);
        point.rate = rate;
        queue_sample_locked(short_name, SAMPLE_DECODE_RATE, point);

        auto &history = rate_history_[sys_name];
        history.push_back(point);

        // Trim to max size (60 minutes of data)
        while (history.size() > MAX_RATE_HISTORY)
        {
            history.pop_front();
        }
    }

    void add_call_rate_point(const std::string &sys_name, const std::string &short_name, int count)
    {
        std::lock_guard<std::mutex> lock(data_mutex_);

        RatePoint point;
        point.timestamp = time(NULL);
        point.rate = static_cast<double>(count);
        queue_sample_locked(short_name, SAMPLE_ACTIVE_CALLS, point);

        auto &history = call_rate_history_[sys_name];
        history.push_back(point);

        // Trim by time (60 minutes) rather than count, since call rate is sampled irregularly
        time_t cutoff = point.timestamp - CALL_RATE_RETENTION_SECONDS;
        while (!history.empty() && history.front().timestamp < cutoff)
        {
            history.pop_front();
        }
    }

    json get_rate_history() const
    {
        // Copy data while holding lock, build JSON after releasing
        std::map<std::string, std::deque<RatePoint>> history_copy;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            history_copy = rate_history_;
        }
        
        json history;
        for (const auto &[sys_name, points] : history_copy)
        {
            json sys_history = json::array();
            for (const auto &point : points)
            {
                sys_history.push_back({{"time", point.timestamp * 1000}, // JavaScript timestamp (ms)
                                       {"rate", point.rate}});
            }
            history[sys_name] = sys_history;
        }

        return history;
    }

    json get_call_rate_history() const
    {
        // Copy data while holding lock, build JSON after releasing
        std::map<std::string, std::deque<RatePoint>> history_copy;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            history_copy = call_rate_history_;
        }
        
        json history;
        for (const auto &[sys_name, points] : history_copy)
        {
            json sys_history = json::array();
            for (const auto &point : points)
            {
                sys_history.push_back({{"time", point.timestamp * 1000}, // JavaScript timestamp (ms)
                                       {"count", static_cast<int>(point.rate)}});
            }
            history[sys_name] = sys_history;
        }

        return history;
    }

    // ============================================================================
    // BASE PLUGIN / API CODE
    // ============================================================================

    int parse_config(json config_data) override
    {
        log_prefix_ = "\t[tr-web]\t";

        port_ = config_data.value("port", 8080);
        bind_address_ = config_data.value("bind", "0.0.0.0");
        username_ = config_data.value("username", "");
        password_ = config_data.value("password", "");
        admin_username_ = config_data.value("admin_username", "");
        admin_password_ = config_data.value("admin_password", "");
        ssl_cert_ = config_data.value("ssl_cert", "");
        ssl_key_ = config_data.value("ssl_key", "");
        console_max_lines_ = config_data.value("console_lines", 5000);
        theme_ = config_data.value("theme", "nostromo");
        max_connections_ = config_data.value("max_connections", 64);
        if (config_data.contains("trusted_proxies") && config_data["trusted_proxies"].is_array())
        {
            trusted_proxies_.clear();
            for (const auto &proxy : config_data["trusted_proxies"])
            {
                if (proxy.is_string())
                {
                    trusted_proxies_.push_back(proxy.get<std::string>());
                }
            }
        }

        // Pre-compute credentials for constant-time comparison
        if (!username_.empty() && !password_.empty())
        {
            expected_user_creds_ = httplib::base64_encode(username_ + ":" + password_);
        }
        if (!admin_username_.empty() && !admin_password_.empty())
        {
            expected_admin_creds_ = httplib::base64_encode(admin_username_ + ":" + admin_password_);
        }

        // Affiliation tracking configuration
        affiliation_timeout_ = config_data.value("affiliation_timeout", 12);
        database_path_ = config_data.value("database", "tr-web.db");
        // The pre-database affiliations.json; imported once into an empty database
        affiliation_cache_ = config_data.value("affiliation_cache", "affiliations.json");
        affiliation_export_ = config_data.value("affiliation_export", "");
        affiliation_export_interval_ = std::max(60, config_data.value("affiliation_export_interval", 3600));

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Port:           " << port_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Bind:           " << bind_address_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Auth:           " << (username_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Admin Auth:     " << (admin_username_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "HTTPS:          " << (ssl_cert_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Console Lines:  " << console_max_lines_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Connections:    " << max_connections_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Theme:          " << theme_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Database:       " << (database_path_.empty() ? "[disabled]" : database_path_);
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Affil Export:   " << (affiliation_export_.empty() ? "[disabled]" : affiliation_export_ + " every " + std::to_string(affiliation_export_interval_) + "s");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Affil Timeout:  " << affiliation_timeout_ << "h";
        if (config_data.contains("affiliation_autosave"))
        {
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "affiliation_autosave is no longer used: changes are saved to the database every "
                                    << DB_FLUSH_SECONDS << "s";
        }

        if (expected_admin_creds_.empty())
        {
            if (expected_user_creds_.empty())
            {
                BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "No credentials configured: admin functions (config editor) are open to anyone who can reach " << bind_address_ << ":" << port_;
            }
            else
            {
                BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "No admin credentials configured: the info-level login also grants admin access (config editor)";
            }
        }

        return 0;
    }

    int init(Config *config, std::vector<Source *> sources, std::vector<System *> systems) override
    {
        tr_config_ = config;
        tr_sources_ = sources;
        tr_systems_ = systems;
        build_db_system_keys();
        return 0;
    }

    int start() override
    {
        log_prefix_ = "[tr-web]\t";

        // Best-effort read of trunk-recorder config (static device metadata)
        try
        {
            std::ifstream in(tr_config_->config_file);
            if (in.good())
            {
                in >> tr_config_json_;
            }
        }
        catch (...)
        {
            // Ignore parse errors; we'll fall back to Source getters.
            tr_config_json_ = json();
        }

        // Setup HTTPS if configured
        if (!ssl_cert_.empty() && !ssl_key_.empty())
        {
            if (!server_.set_https(ssl_cert_, ssl_key_))
            {
                BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to load SSL certificates!";
                BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Falling back to HTTP";
            }
        }

        // Setup routes first
        setup_routes();

        server_.set_max_connections(max_connections_);

        // Enable SSE authentication callback for /events and /graph-stream
        server_.set_sse_auth_callback([this](const httplib::Request &req) -> bool {
            return this->check_auth_hybrid(req, false);
        });
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Configured SSE authentication callback";

        // Enable SSE username extraction callback for logging
        server_.set_sse_username_callback([this](const httplib::Request &req) -> std::string {
            return this->get_username_from_request(req);
        });
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Configured SSE username callback";

        // Setup console log capture
        setup_log_capture();

        // Affiliation history: database (importing the old JSON file once), else memory only
        if (open_database())
        {
            import_legacy_affiliations();
            if (!load_affiliations_from_db())
            {
                BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Reading " << database_path_ << " failed: " << db_->last_error()
                                         << ". The file will not be modified.";
                db_->close();
                db_.reset();
            }
            else
            {
                load_sessions_from_db();
                load_login_history_from_db();
                load_samples_from_db();
            }
        }
        db_thread_ = std::thread(&Tr_Web::database_loop, this);

        // Prime initial caches for first page load
        resend_recorders();
        resend_devices();
        setup_systems(tr_systems_);

        // Start server in background
        running_ = true;
        started_ = true;

        // Broadcast thread (flushes SSE without blocking trunk-recorder)
        broadcast_thread_ = std::thread([this]()
                                        {
      // Writes to TLS clients happen on this thread too
      httplib::block_sigpipe_in_this_thread();
      auto last_console_flush = std::chrono::steady_clock::now();
      size_t graph_dropped_unreported = 0;

      while (running_) {
        // Avoid work if nobody is connected.
        const bool has_clients = (server_.sse_client_count() > 0);

        // Tell open pages to wait for the server instead of treating the drop as a logout
        if (shutdown_notice_.exchange(false) && has_clients) {
          json payload = {{"type", "server_shutdown"}};
          server_.broadcast_sse("server_shutdown", payload.dump(-1, ' ', false, json::error_handler_t::replace));
        }

        // Flush console lines at ~5Hz, batched.
        const auto now = std::chrono::steady_clock::now();
        if (!has_clients) {
          // Nobody to deliver to; a new client loads the history from /api/status instead
          std::lock_guard<std::mutex> lock(console_pending_mutex_);
          console_pending_.clear();
          console_pending_dropped_ = 0;
        } else if ((now - last_console_flush) >= std::chrono::milliseconds(200)) {
          last_console_flush = now;

          std::deque<ConsoleLine> lines;
          size_t dropped = 0;
          {
            std::lock_guard<std::mutex> lock(console_pending_mutex_);
            lines.swap(console_pending_);
            dropped = console_pending_dropped_;
            console_pending_dropped_ = 0;
          }

          if (!lines.empty() || dropped) {
            json payload;
            payload["type"] = "console_batch";
            payload["lines"] = json::array();
            payload["seqs"] = json::array();
            for (const auto &l : lines) {
              payload["lines"].push_back(l.text);
              payload["seqs"].push_back(l.seq);
            }
            payload["dropped"] = dropped;
            server_.broadcast_sse("console_batch", payload.dump(-1, ' ', false, json::error_handler_t::replace));
          }
        }

        // Flush dirty state at ~4Hz.
        if (has_clients) {
          const uint32_t flags = dirty_flags_.exchange(0);
          if (flags) {
            json systems, recorders, calls, rates, devices;
            {
              std::lock_guard<std::mutex> lock(data_mutex_);
              if (flags & DIRTY_SYSTEMS)
                systems = cached_systems_;
              if (flags & DIRTY_RECORDERS)
                recorders = cached_recorders_;
              if (flags & DIRTY_CALLS)
                calls = cached_calls_;
              if (flags & DIRTY_RATES)
                rates = cached_rates_;
              if (flags & DIRTY_DEVICES)
                devices = cached_devices_;
            }

            if (flags & DIRTY_SYSTEMS) {
              json payload = {{"type", "systems"}, {"systems", systems}};
              server_.broadcast_sse("systems", payload.dump(-1, ' ', false, json::error_handler_t::replace));
            }
            if (flags & DIRTY_RECORDERS) {
              json payload = {{"type", "recorders"}, {"recorders", recorders}};
              server_.broadcast_sse("recorders", payload.dump(-1, ' ', false, json::error_handler_t::replace));
            }
            if (flags & DIRTY_CALLS) {
              json payload = {{"type", "calls"}, {"calls_active", calls}};
              server_.broadcast_sse("calls", payload.dump(-1, ' ', false, json::error_handler_t::replace));
            }
            if (flags & DIRTY_RATES) {
              json payload = {{"type", "rates"}, {"rates", rates}};
              server_.broadcast_sse("rates", payload.dump(-1, ' ', false, json::error_handler_t::replace));
            }
            if (flags & DIRTY_DEVICES) {
              json payload = {{"type", "devices"}, {"devices", devices}};
              server_.broadcast_sse("devices", payload.dump(-1, ' ', false, json::error_handler_t::replace));
            }
          }

          // Flush queued discrete events (best-effort).
          // This stays off the trunk-recorder threads.
          std::deque<std::pair<std::string, std::string>> events;
          size_t dropped = 0;
          {
            std::lock_guard<std::mutex> lock(event_queue_mutex_);
            // Limit per-iteration flush to keep latency bounded.
            static constexpr size_t MAX_FLUSH = 100;
            while (!event_queue_.empty() && events.size() < MAX_FLUSH) {
              events.push_back(std::move(event_queue_.front()));
              event_queue_.pop_front();
            }
            dropped = event_queue_dropped_;
            event_queue_dropped_ = 0;
          }

          for (auto &ev : events) {
            server_.broadcast_sse(ev.first, ev.second);
          }
          if (dropped) {
            json payload = {{"type", "event_drop"}, {"dropped", dropped}};
            server_.broadcast_sse("event_drop", payload.dump(-1, ' ', false, json::error_handler_t::replace));
          }
        }

        // Flush graph streaming events for Gephi compatibility (separate /graph-stream endpoint)
        std::deque<std::string> graph_events;
        {
          std::lock_guard<std::mutex> lock(graph_event_queue_mutex_);
          static constexpr size_t MAX_GRAPH_FLUSH = 50;
          while (!graph_event_queue_.empty() && graph_events.size() < MAX_GRAPH_FLUSH) {
            graph_events.push_back(std::move(graph_event_queue_.front()));
            graph_event_queue_.pop_front();
          }
          graph_dropped_unreported += graph_event_queue_dropped_;
          graph_event_queue_dropped_ = 0;
        }

        // Full state for new Gephi clients only
        if (gephi_initial_dump_pending_.exchange(false, std::memory_order_acquire)) {
          server_.send_raw_initial_state("/graph-stream", build_gephi_initial_state());
        }

        // Flush graph events to /graph-stream clients
        for (const auto &graph_event : graph_events) {
          server_.broadcast_raw_to_path("/graph-stream", graph_event);
        }

        // Periodic cleanup of expired sessions (every ~1 minute)
        static time_t last_session_cleanup = time(NULL);
        time_t session_check_time = time(NULL);
        if (session_check_time - last_session_cleanup >= 60) {
          cleanup_expired_sessions();
          prune_auth_attempts();
          last_session_cleanup = session_check_time;
          if (graph_dropped_unreported) {
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Graph stream queue full: dropped " << graph_dropped_unreported << " events in the last minute";
            graph_dropped_unreported = 0;
          }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      } });

        server_thread_ = std::thread([this]()
                                     {
      std::string protocol = server_.is_https() ? "https" : "http";
      BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Starting web server on "
                              << protocol << "://" << bind_address_ << ":" << port_;
      if (!server_.listen(bind_address_, port_)) {
        BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to start web server!";
        running_ = false;
      } });

        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (running_)
        {
            std::string protocol = server_.is_https() ? "https" : "http";
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Web server running at "
                                    << protocol << "://" << bind_address_ << ":" << port_;
        }

        return 0;
    }

    // Called by trunk-recorder and again by the destructor. Always joins the threads, even if
    // listen() failed, or their destructors call std::terminate.
    int stop() override
    {
        if (!started_ || stopped_.exchange(true))
        {
            return 0;
        }

        // Give the broadcast thread a moment to send the shutdown notice to open pages
        if (running_ && server_.sse_client_count() > 0)
        {
            shutdown_notice_ = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        running_ = false;

        // Stop the server first (also waits, bounded, for connection threads to finish)
        server_.stop();

        // Join threads
        if (server_thread_.joinable())
        {
            server_thread_.join();
        }
        if (broadcast_thread_.joinable())
        {
            broadcast_thread_.join();
        }

        // Detach the console capture: the sink refers to this object
        if (web_sink_)
        {
            logging::core::get()->remove_sink(web_sink_);
            web_sink_.reset();
        }

        // Final database flush (plus exports), after the threads that change state have stopped
        {
            std::lock_guard<std::mutex> lock(db_wake_mutex_);
            db_stop_ = true;
        }
        db_wake_.notify_all();
        if (db_thread_.joinable())
        {
            db_thread_.join();
        }
        close_database();

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Web server stopped";
        return 0;
    }

    int setup_systems(std::vector<System *> systems) override
    {
        snapshot_system_data(true);

        json systems_json = json::array();
        for (auto *sys : systems)
        {
            systems_json.push_back(get_system_json(sys));
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_systems_ = systems_json;
        }

        dirty_flags_.fetch_or(DIRTY_SYSTEMS);

        return 0;
    }

    // Called continuously from trunk-recorder's main loop: keep it cheap
    int poll_one() override
    {
        time_t now = time(NULL);
        bool stale;
        {
            std::lock_guard<std::mutex> lock(system_data_mutex_);
            stale = (now - last_ota_snapshot_ >= 10);
        }
        // OTA aliases only matter to someone looking at the dashboard
        if (stale && server_.sse_client_count() > 0)
        {
            snapshot_system_data(false);
        }
        return 0;
    }

    int setup_system(System *system) override
    {
        return setup_systems(tr_systems_);
    }

    int setup_recorder(Recorder *recorder) override
    {
        // Avoid per-recorder broadcast; clients will receive periodic recorders snapshot.
        (void)recorder;
        resend_recorders();

        return 0;
    }

    int setup_config(std::vector<Source *> sources, std::vector<System *> systems) override
    {
        // Cache device frequency ranges once at startup
        device_ranges_.clear();
        device_ranges_.reserve(tr_sources_.size());
        for (auto *source : tr_sources_)
        {
            device_ranges_.push_back({source->get_num(),
                                      source->get_min_hz(),
                                      source->get_max_hz()});
        }

        // Refresh recorders
        resend_recorders();
        resend_devices();
        return 0;
    }

    int calls_active(std::vector<Call *> calls) override
    {
        tr_calls_ = calls;

        // Build current calls map for tracking
        std::map<long, json> current_calls_map;

        // Count calls per system for rate tracking
        std::map<std::string, int> calls_by_system;

        json calls_json = json::array();
        for (auto *call : calls)
        {
            if (call->get_current_length() > 0 || !call->is_conventional())
            {
                json call_json = get_call_json(call);
                long call_num = call->get_call_num();

                current_calls_map[call_num] = call_json;

                System *sys = call->get_system();
                if (sys)
                {
                    std::string unique_name = get_unique_sys_name(sys);
                    calls_by_system[unique_name]++;
                }

                calls_json.push_back(call_json);
            }
        }

        // Detect disappeared calls (synthetic call_end for encrypted calls)
        {
            std::lock_guard<std::mutex> lock(previous_calls_mutex_);
            for (const auto &prev_pair : previous_calls_map_)
            {
                long prev_call_num = prev_pair.first;
                const json &prev_call_json = prev_pair.second;

                // If call was in previous snapshot but not current, it disappeared
                if (current_calls_map.find(prev_call_num) == current_calls_map.end())
                {
                    bool was_encrypted = prev_call_json.value("encrypted", false);

                    if (was_encrypted)
                    {
                        // Cache the encrypted call that disappeared
                        cache_call(prev_call_json);

                        // Send synthetic call_end event to frontend
                        json payload = {{"type", "call_end"}, {"call", prev_call_json}};
                        enqueue_sse_event("call_end", payload);
                    }
                }
            }

            // Update previous calls map for next iteration
            previous_calls_map_ = current_calls_map;
        }

        // Add call rate data points (including zero for systems with no active calls)
        for (auto *sys : tr_systems_)
        {
            std::string sys_name = get_unique_sys_name(sys);
            int count = calls_by_system.count(sys_name) ? calls_by_system[sys_name] : 0;
            add_call_rate_point(sys_name, db_system_key(sys->get_sys_num()), count);
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_calls_ = calls_json;
        }

        dirty_flags_.fetch_or(DIRTY_CALLS);

        return 0;
    }

    int call_start(Call *call) override
    {
        // Best-effort discrete call_start, queued for broadcast thread (skipped with no clients)
        if (server_.sse_client_count() > 0)
        {
            json payload = {{"type", "call_start"}, {"call", get_call_json(call)}};
            enqueue_sse_event("call_start", payload);
        }

        // Also log as a GRANT event for Omnitrunker
        System *sys = call->get_system();
        long source_id = call->get_current_source_id();
        long talkgroup_num = call->get_talkgroup();

        std::string tg_alpha = "";
        Talkgroup *tg = sys->find_talkgroup(talkgroup_num);
        if (tg)
        {
            tg_alpha = tg->alpha_tag;
        }

        std::string unit_alias = sys->find_unit_tag(source_id);

        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", unit_alias},
            {"msg_type", "GRANT"},
            {"talkgroup", talkgroup_num},
            {"tg_alpha", tg_alpha}};

        cache_trunk_message(event_json);

        json grant_payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", grant_payload);

        // Update affiliation state for proper Gephi coloring
        bool encrypted = call->get_encrypted();
        update_affiliation_state(sys, source_id, talkgroup_num, encrypted);

        // Send graph streaming data for Gephi (unit-tg pairing)
        send_gephi_unit_tg_event(sys, source_id, talkgroup_num, encrypted);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int unit_group_affiliation(System *sys, long source_id, long talkgroup_num) override
    {
        // Look up talkgroup alpha tag
        std::string tg_alpha = "";
        Talkgroup *tg = sys->find_talkgroup(talkgroup_num);
        if (tg)
        {
            tg_alpha = tg->alpha_tag;
        }

        // Look up unit alias
        std::string unit_alias = sys->find_unit_tag(source_id);

        // Build event for log
        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", unit_alias},
            {"msg_type", "AFFILIATION"},
            {"talkgroup", talkgroup_num},
            {"tg_alpha", tg_alpha}};

        cache_trunk_message(event_json);

        json payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", payload);

        // Update affiliation tracking for data event (not voice grant)
        update_affiliation_state_data(sys, source_id, talkgroup_num);

        // Send graph streaming data for Gephi (unit-tg pairing)
        send_gephi_unit_tg_event(sys, source_id, talkgroup_num, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int unit_registration(System *sys, long source_id) override
    {
        // Look up unit alias
        std::string unit_alias = sys->find_unit_tag(source_id);

        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", unit_alias},
            {"msg_type", "REGISTRATION"},
            {"talkgroup", nullptr},
            {"tg_alpha", ""}};

        cache_trunk_message(event_json);

        json payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", payload);

        set_unit_registration(sys, source_id, true);

        send_gephi_unit_event(sys, source_id, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int unit_deregistration(System *sys, long source_id) override
    {
        // Look up unit alias
        std::string unit_alias = sys->find_unit_tag(source_id);

        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", unit_alias},
            {"msg_type", "DEREGISTRATION"},
            {"talkgroup", nullptr},
            {"tg_alpha", ""}};

        cache_trunk_message(event_json);

        json payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", payload);

        // Update state: unit is now deregistered
        set_unit_registration(sys, source_id, false);

        // Send Gephi update (standardized builder will include updated status and color)
        send_gephi_unit_event(sys, source_id, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int unit_acknowledge_response(System *sys, long source_id) override
    {
        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", sys->find_unit_tag(source_id)},
            {"msg_type", "ACKNOWLEDGE"},
            {"talkgroup", nullptr},
            {"tg_alpha", ""}};

        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}});

        // Update unit state to track activity
        update_unit_state(sys, source_id, false);
        send_gephi_unit_event(sys, source_id, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int unit_data_grant(System *sys, long source_id) override
    {
        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", sys->find_unit_tag(source_id)},
            {"msg_type", "DATA_GRANT"},
            {"talkgroup", nullptr},
            {"tg_alpha", ""}};

        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}});

        // Update unit state to track activity
        update_unit_state(sys, source_id, false);
        send_gephi_unit_event(sys, source_id, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int unit_answer_request(System *sys, long source_id, long talkgroup_num) override
    {
        Talkgroup *tg = sys->find_talkgroup(talkgroup_num);

        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", sys->find_unit_tag(source_id)},
            {"msg_type", "ANSWER_REQUEST"},
            {"talkgroup", talkgroup_num},
            {"tg_alpha", tg ? tg->alpha_tag : ""}};

        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}});

        // Update unit state to track activity
        update_unit_state(sys, source_id, false);
        send_gephi_unit_tg_event(sys, source_id, talkgroup_num, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int unit_location(System *sys, long source_id, long talkgroup_num) override
    {
        Talkgroup *tg = sys->find_talkgroup(talkgroup_num);

        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", sys->find_unit_tag(source_id)},
            {"msg_type", "LOCATION"},
            {"talkgroup", talkgroup_num},
            {"tg_alpha", tg ? tg->alpha_tag : ""}};

        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}});

        // Update affiliation tracking for data event
        update_affiliation_state_data(sys, source_id, talkgroup_num);
        send_gephi_unit_tg_event(sys, source_id, talkgroup_num, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int call_end(Call_Data_t call_info) override
    {
        record_call_stats(call_info);

        // Prefer the full call JSON produced by trunk-recorder (includes srcList/freqList/tags).
        // Fall back to a minimal summary if it is not populated for some reason.
        json call_json;
        if (!call_info.call_json.is_null() && !call_info.call_json.empty())
        {
            call_json = call_info.call_json;
        }
        else
        {
            call_json = {
                {"freq", int(call_info.freq)},
                {"source_num", int(call_info.source_num)},
                {"recorder_num", int(call_info.recorder_num)},
                {"tdma_slot", int(call_info.tdma_slot)},
                {"phase2_tdma", int(call_info.phase2_tdma)},
                {"start_time", call_info.start_time},
                {"stop_time", call_info.stop_time},
                {"emergency", int(call_info.emergency)},
                {"encrypted", int(call_info.encrypted)},
                {"call_length", int(std::round(call_info.length))},
                {"talkgroup", call_info.talkgroup},
                {"talkgroup_tag", call_info.talkgroup_alpha_tag},
                {"talkgroup_description", call_info.talkgroup_description},
                {"short_name", call_info.short_name}};
        }

        // Add fields not included in trunk-recorder's call JSON
        call_json["call_num"] = call_info.call_num;
        call_json["sys_num"] = call_info.sys_num;

        // Handle conventional unit tracking (no grants for conventional systems)
        // Trunked units are tracked via grant messages, so only process conventional
        System *sys = nullptr;
        for (auto *s : tr_systems_)
        {
            if (s->get_sys_num() == call_info.sys_num)
            {
                sys = s;
                break;
            }
        }

        if (sys)
        {
            std::string sys_type = sys->get_system_type();
            bool is_conventional = (sys_type.find("conventional") != std::string::npos);

            // Only process srcList for conventional systems to avoid duplication
            if (is_conventional && call_json.contains("srcList") && call_json["srcList"].is_array())
            {
                long talkgroup = call_info.talkgroup;
                bool encrypted = call_info.encrypted;

                // Iterate through all units in the srcList
                for (const auto &src_entry : call_json["srcList"])
                {
                    if (src_entry.contains("src") && src_entry["src"].is_number())
                    {
                        long unit_id = src_entry["src"];

                        // Update affiliation state for this unit-talkgroup pair
                        update_affiliation_state(sys, unit_id, talkgroup, encrypted);

                        // Send Gephi event for the unit-talkgroup relationship
                        send_gephi_unit_tg_event(sys, unit_id, talkgroup, encrypted);
                    }
                }
            }
        }

        // Cache for initial page load
        cache_call(call_json);

        // Queue the rich end-event for the broadcast thread.
        json payload = {{"type", "call_end"}, {"call", call_json}};
        enqueue_sse_event("call_end", payload);
        return 0;
    }

    int system_rates(std::vector<System *> systems, float timeDiff) override
    {
        json rates_json = json::array();

        for (auto *sys : systems)
        {
            std::string sys_type = sys->get_system_type();
            if (sys_type.find("conventional") == std::string::npos)
            {
                boost::property_tree::ptree stat_node = sys->get_stats_current(timeDiff);
                double decode_rate = stat_node.get<double>("decoderate");
                decode_rate = std::round(decode_rate * 100) / 100; // Round to 2 decimal places

                double control_channel = 0.0;
                if (sys->control_channel_count() > 0)
                {
                    control_channel = sys->get_current_control_channel();
                }

                rates_json.push_back({{"sys_num", stat_node.get<int>("id")},
                                      {"sys_name", get_unique_sys_name(sys)},
                                      {"decoderate", decode_rate},
                                      {"control_channel", control_channel}});

                // Store in rate history
                add_rate_point(get_unique_sys_name(sys), db_system_key(sys->get_sys_num()), decode_rate);
            }
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_rates_ = rates_json;
        }

        dirty_flags_.fetch_or(DIRTY_RATES);

        return 0;
    }

    // ============================================================================
    // WEB-RELATED CODE
    // ============================================================================

private:

    // Coded voice bits per second (bit error rate denominator): IMBE 144 bits / 20 ms,
    // AMBE (Phase 2, DMR) 72 bits / 20 ms, analog none
    static double voice_bits_per_second(const Call_Data_t &call_info, const std::string &system_type)
    {
        if (call_info.audio_type == "analog")
            return 0;
        if (call_info.phase2_tdma || system_type.find("DMR") != std::string::npos || system_type.find("dmr") != std::string::npos)
            return 3600;
        return 7200;
    }

    void record_call_stats(const Call_Data_t &call_info)
    {
        time_t now = time(NULL);
        long long freq = std::llround(call_info.freq);
        std::string system_type;
        for (auto *s : tr_systems_)
            if (s->get_sys_num() == call_info.sys_num)
                system_type = s->get_system_type();
        const double bit_rate = voice_bits_per_second(call_info, system_type);

        QualityCounts counts;
        counts.calls = 1;
        // Source and error lists are filled together, one entry per transmission
        const bool per_unit = call_info.transmission_source_list.size() == call_info.transmission_error_list.size();
        std::map<long, UnitStats> units;
        for (size_t i = 0; i < call_info.transmission_error_list.size(); ++i)
        {
            const auto &tx = call_info.transmission_error_list[i];
            QualityCounts one;
            one.transmissions = 1;
            one.seconds = tx.total_len;
            one.errors = static_cast<uint64_t>(std::max(0.0, tx.error_count));
            one.spikes = static_cast<uint64_t>(std::max(0.0, tx.spike_count));
            one.voice_bits = tx.total_len * bit_rate;
            merge_counts(counts, one);

            long unit_id = per_unit ? call_info.transmission_source_list[i].source : 0;
            if (unit_id == 0 || unit_id == -1)
                continue;
            UnitStats &u = units[unit_id];
            u.transmissions++;
            u.seconds += one.seconds;
            u.errors += one.errors;
            u.spikes += one.spikes;
            u.voice_bits += one.voice_bits;
            const auto &src = call_info.transmission_source_list[i];
            if (u.alias.empty())
                u.alias = !src.tag.empty() ? src.tag : src.tag_ota;
            u.last_seen = now;
        }
        FreqStats f;
        f.all = counts;
        f.phase2_calls = call_info.phase2_tdma ? 1 : 0;
        f.freq_error_sum = call_info.freq_error;
        f.freq_error_count = 1;
        f.last_seen = now;

        TalkgroupStats tg;
        tg.calls = 1;
        tg.seconds = counts.seconds;
        tg.errors = counts.errors;
        tg.spikes = counts.spikes;
        tg.voice_bits = counts.voice_bits;
        tg.encrypted = call_info.encrypted ? 1 : 0;
        tg.emergency = call_info.emergency ? 1 : 0;
        tg.alpha_tag = call_info.talkgroup_alpha_tag;
        tg.last_seen = now;

        std::lock_guard<std::mutex> lock(system_stats_mutex_);
        // Since-restart totals (the "since restart" window)...
        SystemStats &sys = system_stats_[call_info.sys_num];
        merge_stats(sys.frequencies[freq], f);
        merge_stats(sys.talkgroups[call_info.talkgroup], tg);
        for (const auto &[unit_id, u] : units)
            merge_stats(sys.units[unit_id], u);
        // ...and this hour's totals, added into the database by the database thread
        int64_t hour = now - now % 3600;
        const std::string key = db_system_key(call_info.sys_num);
        merge_stats(pending_freq_hours_[std::make_tuple(key, hour, freq)], f);
        merge_stats(pending_tg_hours_[std::make_tuple(key, hour, (long)call_info.talkgroup)], tg);
        for (const auto &[unit_id, u] : units)
            merge_stats(pending_unit_hours_[std::make_tuple(key, hour, unit_id)], u);
    }

    static void merge_counts(QualityCounts &to, const QualityCounts &from)
    {
        to.calls += from.calls;
        to.transmissions += from.transmissions;
        to.seconds += from.seconds;
        to.errors += from.errors;
        to.spikes += from.spikes;
        to.voice_bits += from.voice_bits;
    }

    static void merge_stats(FreqStats &to, const FreqStats &from)
    {
        merge_counts(to.all, from.all);
        to.phase2_calls += from.phase2_calls;
        to.freq_error_sum += from.freq_error_sum;
        to.freq_error_count += from.freq_error_count;
        to.last_seen = std::max(to.last_seen, from.last_seen);
    }

    static void merge_stats(TalkgroupStats &to, const TalkgroupStats &from)
    {
        to.calls += from.calls;
        to.seconds += from.seconds;
        to.encrypted += from.encrypted;
        to.emergency += from.emergency;
        to.errors += from.errors;
        to.spikes += from.spikes;
        to.voice_bits += from.voice_bits;
        if (!from.alpha_tag.empty())
            to.alpha_tag = from.alpha_tag;
        to.last_seen = std::max(to.last_seen, from.last_seen);
    }

    static void merge_stats(UnitStats &to, const UnitStats &from)
    {
        to.transmissions += from.transmissions;
        to.seconds += from.seconds;
        to.errors += from.errors;
        to.spikes += from.spikes;
        to.voice_bits += from.voice_bits;
        if (!from.alias.empty())
            to.alias = from.alias;
        to.last_seen = std::max(to.last_seen, from.last_seen);
    }

    // "Since restart" window, from memory
    json get_system_stats(int sys_num, size_t top_talkgroups) const
    {
        SystemStats copy;
        time_t since;
        {
            std::lock_guard<std::mutex> lock(system_stats_mutex_);
            auto it = system_stats_.find(sys_num);
            if (it != system_stats_.end())
                copy = it->second;
            since = stats_since_;
        }
        return system_stats_json(sys_num, copy, since, "restart", top_talkgroups);
    }

    // Longer windows, summed from the hourly tables. hours == 0 means all time.
    json get_system_stats_window(System *sys, int hours, const std::string &window, size_t top_talkgroups)
    {
        const std::string name = db_system_key(sys->get_sys_num());
        time_t now = time(NULL);
        int64_t current_hour = now - now % 3600;
        int64_t first_hour = hours > 0 ? current_hour - (int64_t)(hours - 1) * 3600 : 0;
        SystemStats stats;

        auto fq = db_read_->prepare(R"SQL(
            SELECT freq, SUM(calls), SUM(transmissions), SUM(seconds), SUM(errors), SUM(spikes),
                   SUM(phase2_calls), SUM(freq_error_sum), SUM(freq_error_count), MAX(last_seen), SUM(voice_bits)
            FROM frequency_hours WHERE system = ?1 AND hour >= ?2 GROUP BY freq)SQL");
        fq.bind(1, name).bind(2, first_hour);
        while (fq.ok() && fq.step() == SQLITE_ROW)
        {
            FreqStats &f = stats.frequencies[fq.col_int(0)];
            f.all = {(uint64_t)fq.col_int(1), (uint64_t)fq.col_int(2), fq.col_double(3), (uint64_t)fq.col_int(4), (uint64_t)fq.col_int(5)};
            f.phase2_calls = (uint64_t)fq.col_int(6);
            f.freq_error_sum = fq.col_double(7);
            f.freq_error_count = (uint64_t)fq.col_int(8);
            f.last_seen = (time_t)fq.col_int(9);
            f.all.voice_bits = fq.col_double(10);
        }

        // The busiest talkgroups, and the ones with the most errors (the JSON builder ranks both)
        auto alias = db_read_->prepare(R"SQL(
            SELECT alpha_tag FROM talkgroup_hours
            WHERE system = ?1 AND talkgroup = ?2 AND alpha_tag <> '' ORDER BY hour DESC LIMIT 1)SQL");
        for (const char *order : {"SUM(seconds)", "SUM(errors)"})
        {
            auto tq = db_read_->prepare(std::string(R"SQL(
                SELECT talkgroup, SUM(calls), SUM(seconds), SUM(encrypted), SUM(emergency), SUM(errors), MAX(last_seen),
                       SUM(spikes), SUM(voice_bits)
                FROM talkgroup_hours WHERE system = ?1 AND hour >= ?2
                GROUP BY talkgroup ORDER BY )SQL") + order + " DESC LIMIT ?3");
            tq.bind(1, name).bind(2, first_hour).bind(3, (int64_t)top_talkgroups);
            while (tq.ok() && tq.step() == SQLITE_ROW)
            {
                long id = (long)tq.col_int(0);
                if (stats.talkgroups.count(id))
                    continue;
                TalkgroupStats &t = stats.talkgroups[id];
                t.calls = (uint64_t)tq.col_int(1);
                t.seconds = tq.col_double(2);
                t.encrypted = (uint64_t)tq.col_int(3);
                t.emergency = (uint64_t)tq.col_int(4);
                t.errors = (uint64_t)tq.col_int(5);
                t.last_seen = (time_t)tq.col_int(6);
                t.spikes = (uint64_t)tq.col_int(7);
                t.voice_bits = tq.col_double(8);
                alias.bind(1, name).bind(2, (int64_t)id);
                if (alias.step() == SQLITE_ROW)
                    t.alpha_tag = alias.col_text(0);
                alias.reset();
            }
        }

        // Radios with the most errors
        auto uq = db_read_->prepare(R"SQL(
            SELECT unit, SUM(transmissions), SUM(seconds), SUM(errors), SUM(spikes), SUM(voice_bits), MAX(last_seen),
                   (SELECT alias FROM unit_hours a WHERE a.system = u.system AND a.unit = u.unit AND a.alias <> ''
                    ORDER BY a.hour DESC LIMIT 1)
            FROM unit_hours u WHERE system = ?1 AND hour >= ?2
            GROUP BY unit ORDER BY SUM(errors) DESC LIMIT ?3)SQL");
        uq.bind(1, name).bind(2, first_hour).bind(3, (int64_t)top_talkgroups);
        while (uq.ok() && uq.step() == SQLITE_ROW)
        {
            UnitStats &u = stats.units[(long)uq.col_int(0)];
            u.transmissions = (uint64_t)uq.col_int(1);
            u.seconds = uq.col_double(2);
            u.errors = (uint64_t)uq.col_int(3);
            u.spikes = (uint64_t)uq.col_int(4);
            u.voice_bits = uq.col_double(5);
            u.last_seen = (time_t)uq.col_int(6);
            u.alias = uq.col_null(7) ? "" : uq.col_text(7);
        }

        StatsTotals totals;
        for (const auto &[freq, f] : stats.frequencies)
            merge_counts(totals.calls, f.all);
        auto tt = db_read_->prepare("SELECT SUM(encrypted), SUM(emergency) FROM talkgroup_hours WHERE system = ?1 AND hour >= ?2");
        tt.bind(1, name).bind(2, first_hour);
        if (tt.ok() && tt.step() == SQLITE_ROW)
        {
            totals.encrypted = (uint64_t)tt.col_int(0);
            totals.emergency = (uint64_t)tt.col_int(1);
        }

        time_t since = (time_t)first_hour;
        if (hours == 0)
        {
            auto first = db_read_->prepare("SELECT MIN(hour) FROM frequency_hours WHERE system = ?1");
            first.bind(1, name);
            since = (first.ok() && first.step() == SQLITE_ROW && !first.col_null(0)) ? (time_t)first.col_int(0) : now;
        }
        return system_stats_json(sys->get_sys_num(), stats, since, window, top_talkgroups, &totals);
    }

    // Hourly history of one frequency, talkgroup or radio; first_hour 0 means all time
    json get_system_history(System *sys, const std::string &kind, int64_t id, int64_t first_hour)
    {
        const std::string name = db_system_key(sys->get_sys_num());
        json hours = json::array();
        if (kind == "talkgroup")
        {
            auto q = db_read_->prepare(R"SQL(
                SELECT hour, calls, seconds, errors, encrypted, emergency, voice_bits FROM talkgroup_hours
                WHERE system = ?1 AND talkgroup = ?2 AND hour >= ?3 ORDER BY hour)SQL");
            q.bind(1, name).bind(2, id).bind(3, first_hour);
            while (q.ok() && q.step() == SQLITE_ROW)
            {
                hours.push_back({{"hour", q.col_int(0)}, {"calls", q.col_int(1)}, {"seconds", q.col_double(2)},
                                 {"errors", q.col_int(3)}, {"encrypted", q.col_int(4)}, {"emergency", q.col_int(5)},
                                 {"voice_bits", q.col_double(6)}});
            }
        }
        else if (kind == "unit")
        {
            auto q = db_read_->prepare(R"SQL(
                SELECT hour, transmissions, seconds, errors, voice_bits FROM unit_hours
                WHERE system = ?1 AND unit = ?2 AND hour >= ?3 ORDER BY hour)SQL");
            q.bind(1, name).bind(2, id).bind(3, first_hour);
            while (q.ok() && q.step() == SQLITE_ROW)
            {
                hours.push_back({{"hour", q.col_int(0)}, {"transmissions", q.col_int(1)}, {"seconds", q.col_double(2)},
                                 {"errors", q.col_int(3)}, {"voice_bits", q.col_double(4)}});
            }
        }
        else
        {
            auto q = db_read_->prepare(R"SQL(
                SELECT hour, calls, seconds, errors, voice_bits
                FROM frequency_hours WHERE system = ?1 AND freq = ?2 AND hour >= ?3 ORDER BY hour)SQL");
            q.bind(1, name).bind(2, id).bind(3, first_hour);
            while (q.ok() && q.step() == SQLITE_ROW)
            {
                hours.push_back({{"hour", q.col_int(0)}, {"calls", q.col_int(1)}, {"seconds", q.col_double(2)},
                                 {"errors", q.col_int(3)}, {"voice_bits", q.col_double(4)}});
            }
        }
        return {{"sys_num", sys->get_sys_num()}, {"kind", kind}, {"id", id}, {"hours", hours}};
    }

    // Whole-system totals for the stat tiles
    struct StatsTotals
    {
        QualityCounts calls;
        uint64_t encrypted = 0;
        uint64_t emergency = 0;
    };

    json system_stats_json(int sys_num, const SystemStats &copy, time_t since, const std::string &window, size_t top_talkgroups,
                           const StatsTotals *db_totals = nullptr) const
    {
        StatsTotals totals;
        if (db_totals)
        {
            totals = *db_totals;
        }
        else
        {
            for (const auto &[freq, f] : copy.frequencies)
                merge_counts(totals.calls, f.all);
            for (const auto &[id, t] : copy.talkgroups)
            {
                totals.encrypted += t.encrypted;
                totals.emergency += t.emergency;
            }
        }

        auto counts_json = [](const QualityCounts &c)
        {
            return json{{"calls", c.calls}, {"transmissions", c.transmissions}, {"seconds", std::round(c.seconds * 10) / 10}, {"errors", c.errors}, {"spikes", c.spikes}, {"voice_bits", std::round(c.voice_bits)}};
        };

        json freqs = json::array();
        for (const auto &[freq, f] : copy.frequencies)
        {
            freqs.push_back({{"freq", freq},
                             {"all", counts_json(f.all)},
                             {"phase2_calls", f.phase2_calls},
                             {"avg_freq_error", f.freq_error_count ? std::round(f.freq_error_sum / f.freq_error_count) : 0},
                             {"last_seen", f.last_seen}});
        }

        auto talkgroup_json = [](long id, const TalkgroupStats &tg)
        {
            return json{{"talkgroup", id},
                        {"alpha_tag", tg.alpha_tag},
                        {"calls", tg.calls},
                        {"seconds", std::round(tg.seconds * 10) / 10},
                        {"encrypted", tg.encrypted},
                        {"emergency", tg.emergency},
                        {"errors", tg.errors},
                        {"voice_bits", std::round(tg.voice_bits)},
                        {"last_seen", tg.last_seen}};
        };
        // The first `top_talkgroups` entries of `from`, ranked by `better`
        auto top = [top_talkgroups](const auto &from, auto better)
        {
            std::vector<std::pair<long, typename std::decay_t<decltype(from)>::mapped_type>> v(from.begin(), from.end());
            std::sort(v.begin(), v.end(), better);
            if (v.size() > top_talkgroups)
                v.resize(top_talkgroups);
            return v;
        };

        json busiest = json::array();
        for (const auto &[id, tg] : top(copy.talkgroups, [](const auto &a, const auto &b)
                                        { return a.second.seconds > b.second.seconds; }))
            busiest.push_back(talkgroup_json(id, tg));

        // Only entries that had errors: the lists are "where do errors come from"
        json error_talkgroups = json::array();
        for (const auto &[id, tg] : top(copy.talkgroups, [](const auto &a, const auto &b)
                                        { return a.second.errors > b.second.errors; }))
            if (tg.errors > 0)
                error_talkgroups.push_back(talkgroup_json(id, tg));

        json error_units = json::array();
        for (const auto &[id, u] : top(copy.units, [](const auto &a, const auto &b)
                                       { return a.second.errors > b.second.errors; }))
        {
            if (u.errors == 0)
                continue;
            error_units.push_back({{"unit", id},
                                   {"alias", u.alias},
                                   {"transmissions", u.transmissions},
                                   {"seconds", std::round(u.seconds * 10) / 10},
                                   {"errors", u.errors},
                                   {"voice_bits", std::round(u.voice_bits)},
                                   {"last_seen", u.last_seen}});
        }

        return {{"sys_num", sys_num},
                {"window", window},
                {"since", since},
                {"now", time(NULL)},
                {"totals", {{"calls", totals.calls.calls},
                            {"seconds", std::round(totals.calls.seconds * 10) / 10},
                            {"errors", totals.calls.errors},
                            {"spikes", totals.calls.spikes},
                            {"voice_bits", std::round(totals.calls.voice_bits)},
                            {"encrypted", totals.encrypted},
                            {"emergency", totals.emergency}}},
                {"frequencies", freqs},
                {"top_talkgroups", busiest},
                {"error_talkgroups", error_talkgroups},
                {"error_units", error_units}};
    }

    /// Resolve the ?sys_num= query parameter to a System, writing a 400/404 response if it can't
    System *find_system_param(const httplib::Request &req, httplib::Response &res)
    {
        auto it = req.params.find("sys_num");
        int sys_num = -1;
        try
        {
            if (it == req.params.end())
                throw std::invalid_argument("missing");
            size_t consumed = 0;
            sys_num = std::stoi(it->second, &consumed);
            if (consumed != it->second.size())
                throw std::invalid_argument("trailing characters");
        }
        catch (const std::exception &)
        {
            res.status = 400;
            res.set_content("{\"error\": \"missing or invalid sys_num parameter\"}", "application/json");
            return nullptr;
        }

        for (auto *s : tr_systems_)
        {
            if (s->get_sys_num() == sys_num)
            {
                return s;
            }
        }
        res.status = 404;
        res.set_content("{\"error\": \"system not found\"}", "application/json");
        return nullptr;
    }

    // snapshot_system_data()
    //   Serialize the talkgroup, unit tag and OTA lists for /api/system/*. Runs on
    //   trunk-recorder's thread, which owns those lists. OTA aliases change while running.
    void snapshot_system_data(bool include_static)
    {
        std::map<int, std::string> talkgroups, unit_tags, ota;
        for (auto *sys : tr_systems_)
        {
            int sys_num = sys->get_sys_num();
            if (include_static)
            {
                json tgs = json::array();
                for (auto *tg : sys->get_talkgroups())
                {
                    tgs.push_back({{"number", tg->number},
                                   {"alpha_tag", tg->alpha_tag},
                                   {"description", tg->description},
                                   {"tag", tg->tag},
                                   {"group", tg->group},
                                   {"priority", tg->priority}});
                }
                talkgroups[sys_num] = tgs.dump(-1, ' ', false, json::error_handler_t::replace);

                json tags = json::array();
                for (auto *tag : sys->get_unit_tags())
                {
                    tags.push_back({{"pattern", tag->pattern.str()}, {"tag", tag->tag}});
                }
                json unit_tags_response = {
                    {"file", sys->get_unit_tags_file()},
                    {"mode", sys->get_unit_tags_mode()},
                    {"count", tags.size()},
                    {"tags", tags}};
                unit_tags[sys_num] = unit_tags_response.dump(-1, ' ', false, json::error_handler_t::replace);
            }

            json aliases = json::array();
            for (auto *alias : sys->get_unit_tags_ota())
            {
                aliases.push_back({{"unit", alias->unit_id}, {"alias", alias->alias}});
            }
            json ota_response = {
                {"file", sys->get_unit_tags_ota_file()},
                {"count", aliases.size()},
                {"aliases", aliases}};
            ota[sys_num] = ota_response.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        std::lock_guard<std::mutex> lock(system_data_mutex_);
        if (include_static)
        {
            talkgroups_json_.swap(talkgroups);
            unit_tags_json_.swap(unit_tags);
        }
        ota_json_.swap(ota);
        last_ota_snapshot_ = time(NULL);
    }

    void serve_system_snapshot(const httplib::Request &req, httplib::Response &res, const std::map<int, std::string> &snapshot)
    {
        if (!require_auth(req, res))
            return;
        System *sys = find_system_param(req, res);
        if (!sys)
            return;

        std::string body;
        {
            std::lock_guard<std::mutex> lock(system_data_mutex_);
            auto it = snapshot.find(sys->get_sys_num());
            if (it != snapshot.end())
                body = it->second;
        }
        if (body.empty())
        {
            res.status = 503;
            res.set_content("{\"error\": \"system data not loaded yet\"}", "application/json");
            return;
        }
        res.set_content(body, "application/json");
    }

    void setup_log_capture()
    {
        // Setup custom logging sink to capture console output
        typedef logging::sinks::synchronous_sink<WebLogBackend> web_sink_t;

        boost::shared_ptr<web_sink_t> web_sink =
            boost::make_shared<web_sink_t>(boost::make_shared<WebLogBackend>(*this));
        logging::core::get()->add_sink(web_sink);
        web_sink_ = web_sink; // removed again in stop()
    }

    // Write `data` to `path` and fsync it. The file is created with `mode` (and set to it).
    static bool write_whole_file(const std::string &path, const std::string &data, mode_t mode, int flags, std::string &error)
    {
        int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | flags, mode);
        if (fd < 0)
        {
            error = "cannot open " + path + ": " + strerror(errno);
            return false;
        }
        bool ok = ::fchmod(fd, mode) == 0 || errno == EPERM;
        size_t done = 0;
        while (ok && done < data.size())
        {
            ssize_t n = ::write(fd, data.data() + done, data.size() - done);
            if (n < 0 && errno == EINTR)
                continue;
            ok = n > 0;
            if (ok)
                done += static_cast<size_t>(n);
        }
        ok = ok && ::fsync(fd) == 0;
        if (!ok)
            error = "writing " + path + " failed: " + strerror(errno);
        ok = (::close(fd) == 0) && ok;
        return ok;
    }

    // save_config_file()
    //   Replace the config, keeping the old one as backup_path, with the same permissions.
    //   A bind-mounted file can't be renamed over, so it is rewritten in place.
    bool save_config_file(const std::string &path, const std::string &backup_path, const std::string &content, std::string &error)
    {
        mode_t mode = 0600;
        std::string old_content;
        struct stat st;
        if (::stat(path.c_str(), &st) == 0)
        {
            mode = st.st_mode & 07777;
            std::ifstream in(path, std::ios::binary);
            old_content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!in.good() && !in.eof())
            {
                error = "cannot read the current config to back it up";
                return false;
            }
            // Backup: written to a temporary name first, so an old good backup is never half-overwritten
            const std::string backup_tmp = backup_path + ".tmp";
            if (!write_whole_file(backup_tmp, old_content, mode, O_CREAT | O_TRUNC, error) ||
                ::rename(backup_tmp.c_str(), backup_path.c_str()) != 0)
            {
                if (error.empty())
                    error = "cannot create backup " + backup_path + ": " + strerror(errno);
                ::unlink(backup_tmp.c_str());
                return false;
            }
        }

        const std::string temp_path = path + ".tmp.trweb";
        if (!write_whole_file(temp_path, content, mode, O_CREAT | O_TRUNC, error))
        {
            ::unlink(temp_path.c_str());
            return false;
        }
        if (::rename(temp_path.c_str(), path.c_str()) != 0)
        {
            int rename_errno = errno;
            ::unlink(temp_path.c_str());
            if (rename_errno != EBUSY && rename_errno != EXDEV)
            {
                error = std::string("cannot replace ") + path + ": " + strerror(rename_errno);
                return false;
            }
            // Bind-mounted file: rewrite in place (the backup above holds the old contents)
            if (!write_whole_file(path, content, mode, O_TRUNC, error))
                return false;
        }
        fsync_parent_dir(path);
        return true;
    }

    void setup_routes()
    {
        // Main page - serves embedded HTML
        server_.Get("/", [](const httplib::Request &req, httplib::Response &res)
                    { res.set_content(tr_web::HTML_PAGE, "text/html; charset=utf-8"); });

        // Favicon endpoint - serves SVG icon
        server_.Get("/favicon.svg", [](const httplib::Request &req, httplib::Response &res)
                    { 
                        const char* svg = R"(<svg width="40" height="40" viewBox="0 0 1200 1200" xmlns="http://www.w3.org/2000/svg"><path fill="#e94560" d="m787.35 373.6v-291.25c9.5273-8.7461 15.938-20.941 15.938-35.004-0.003906-26.086-21.254-47.34-47.512-47.34-26.258 0-47.496 21.254-47.496 47.34 0 14.062 6.4062 26.258 15.938 35.004v287.03h-92.664v-104.69c0-17.34-14.219-31.402-31.559-31.402-17.496 0-31.559 14.062-31.559 31.402v104.69h-70.621v-57.816c0-26.09-21.254-47.184-47.34-47.184-26.09 0-47.34 21.098-47.34 47.184v66.875c-18.914 10.945-32.355 30.625-32.355 53.918v254.69c0 34.691 9.6836 78.59 21.562 97.656s21.562 62.965 21.562 97.656v254.53c0 34.691 28.441 63.133 63.121 63.133h245.94c34.691 0 63.133-28.441 63.133-63.133v-254.54c0-34.691 9.6953-78.59 21.562-97.656 11.867-19.066 21.562-62.965 21.562-97.656l0.003907-254.68c-0.011719-27.191-17.676-49.848-41.879-58.75zm-1.2617 305.62h-372.18v-31.559h372.19v31.559zm0-66.25h-372.18v-31.559h372.19v31.559zm0-66.41h-372.18v-31.559h372.19v31.559zm0-66.25h-372.18v-31.559h372.19v31.559z"/></svg>)";
                        res.set_content(svg, "image/svg+xml"); });

        // Fallback ICO favicon for Safari
        server_.Get("/favicon.ico", [](const httplib::Request &req, httplib::Response &res)
                    { 
                        // Redirect to SVG version
                        // Relative, so it also works under a reverse-proxy subpath
                        res.status = 302;
                        res.set_header("Location", "favicon.svg"); });

        // SSE endpoint for live updates
        server_.SSE("/events");

        // Dedicated endpoint for Gephi graph streaming (raw JSON, not SSE)
        server_.RawStream("/graph-stream");

        // Notify when Gephi clients connect so we can send current state
        server_.set_raw_stream_connect_notify([this]()
                                              { this->request_gephi_initial_dump(); });

        // /events and /graph-stream authenticate through set_sse_auth_callback() (see start()):
        // session cookie for the web UI, HTTP Basic Auth for Gephi and other external tools.

        // Login endpoint - validates credentials and returns session token
        server_.Post("/api/login", [this](const httplib::Request &req, httplib::Response &res)
                     {
      try {
        json request_data = json::parse(req.body);
        std::string username = request_data.value("username", "");
        std::string password = request_data.value("password", "");
        
        // Get client IP for rate limiting and logging
        std::string client_ip = get_client_ip(req);

        if (is_rate_limited(client_ip)) {
          BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Login rate limit exceeded for " << client_ip;
          res.status = 429;
          res.set_content("{\"error\": \"Too many failed attempts, try again later\"}", "application/json");
          return;
        }

        if (username.empty() || password.empty()) {
          res.status = 400;
          res.set_content("{\"error\": \"Username and password required\"}", "application/json");
          return;
        }

        // Check credentials
        std::string provided_creds = httplib::base64_encode(username + ":" + password);
        bool is_admin = false;
        bool auth_success = false;

        if (!expected_admin_creds_.empty() && constant_time_compare(provided_creds, expected_admin_creds_)) {
          auth_success = true;
          is_admin = true;
        } else if (!expected_user_creds_.empty() && constant_time_compare(provided_creds, expected_user_creds_)) {
          auth_success = true;
          // No admin creds configured: user credentials are de-facto admin
          is_admin = expected_admin_creds_.empty();
        }

        if (!auth_success) {
          // Track failed login attempt (username is attacker-supplied: keep the stored copy short)
          record_auth_attempt(client_ip);
          record_login_attempt(username.substr(0, 64), client_ip, false, "failed");
          res.status = 401;
          res.set_content("{\"error\": \"Invalid credentials\"}", "application/json");
          return;
        }

        // Track successful login attempt
        record_login_attempt(username, client_ip, true, is_admin ? "admin" : "user");
        
        // Create session
        std::string token = create_session(username, is_admin);
        
        // Set cookie so EventSource (SSE) can authenticate automatically
        res.set_header("Set-Cookie", "session=" + token + "; Path=/; HttpOnly; SameSite=Strict" +
                                         (server_.is_https() ? "; Secure" : ""));

        json response = {
            {"token", token},
            {"auth_level", is_admin ? "admin" : "user"}};
        res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");

      } catch (const std::exception &e) {
        res.status = 400;
        json error = {{"error", std::string("Invalid request: ") + e.what()}};
        res.set_content(error.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
      } });

        // Logout endpoint - deletes session token
        server_.Post("/api/logout", [this](const httplib::Request &req, httplib::Response &res)
                     {
      std::string token = request_session_token(req);
      if (!token.empty()) {
        delete_session(token);
      }

      // Clear the session cookie
      res.set_header("Set-Cookie", "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");

      json response = {{"message", "Logged out successfully"}};
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // REST API endpoint for initial state
        server_.Get("/api/status", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;

      json response;
      {
        std::lock_guard<std::mutex> lock(data_mutex_);
        response["recorders"] = cached_recorders_;
        response["calls"] = cached_calls_;
        response["systems"] = cached_systems_;
        response["devices"] = cached_devices_;
        response["rates"] = cached_rates_;
      }
      response["config"] = {
          {"theme", theme_},
          {"console_max_lines", console_max_lines_}};
      response["rateHistory"] = get_rate_history();
      response["callRateHistory"] = get_call_rate_history();
      response["callHistory"] = get_call_history();
      response["trunkMessages"] = get_trunk_messages();
      // consoleSeq lets the client skip streamed console lines already in this history
      uint64_t console_seq = 0;
      response["consoleLogs"] = get_console_logs(&console_seq);
      response["consoleSeq"] = console_seq;
      response["timestamp"] = time(NULL);
      response["sse_clients"] = server_.sse_client_count();

      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Rate history endpoint
        server_.Get("/api/rates/history", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      json response = get_rate_history();
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Call rate history endpoint
        server_.Get("/api/calls/rate-history", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      json response = get_call_rate_history();
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Console logs endpoint
        server_.Get("/api/console", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      json response = {{"lines", get_console_logs()}};
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Affiliations data endpoint (with optional pagination)
        server_.Get("/api/affiliations", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;

      // Parse optional query parameters for pagination
      int limit = 0;
      bool units_only = false;
      bool talkgroups_only = false;
      
      auto limit_it = req.params.find("limit");
      if (limit_it != req.params.end()) {
          try {
              limit = std::stoi(limit_it->second);
          } catch(...) {}
      }
      
      time_t since = 0;
      auto since_it = req.params.find("since");
      if (since_it != req.params.end()) {
          try {
              since = static_cast<time_t>(std::stoll(since_it->second));
          } catch(...) {}
      }

      auto view_it = req.params.find("view");
      if (view_it != req.params.end()) {
          if (view_it->second == "units") units_only = true;
          else if (view_it->second == "talkgroups") talkgroups_only = true;
      }

      json response = get_affiliation_data(limit, units_only, talkgroups_only, since);
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // ?sys_num=N. Served from snapshots (see snapshot_system_data()).
        server_.Get("/api/system/talkgroups", [this](const httplib::Request &req, httplib::Response &res)
                    { serve_system_snapshot(req, res, talkgroups_json_); });
        server_.Get("/api/system/unit_tags", [this](const httplib::Request &req, httplib::Response &res)
                    { serve_system_snapshot(req, res, unit_tags_json_); });
        server_.Get("/api/system/unit_tags_ota", [this](const httplib::Request &req, httplib::Response &res)
                    { serve_system_snapshot(req, res, ota_json_); });
        // Hourly history of one frequency, talkgroup or radio: ?sys_num=N&kind=freq|talkgroup|unit&id=X&window=...
        server_.Get("/api/system/history", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      System *sys = find_system_param(req, res);
      if (!sys) return;
      if (!db_read_) {
        res.status = 503;
        res.set_content("{\"error\": \"history is not available (no database)\"}", "application/json");
        return;
      }
      std::string kind = req.params.count("kind") ? req.params.at("kind") : "";
      int64_t id = 0;
      try {
        size_t used = 0;
        const std::string &raw = req.params.at("id");
        id = std::stoll(raw, &used);
        if (used != raw.size()) throw std::invalid_argument("id");
      } catch (...) {
        kind.clear();
      }
      if (kind != "freq" && kind != "talkgroup" && kind != "unit") {
        res.status = 400;
        res.set_content("{\"error\": \"kind must be freq, talkgroup or unit, with a numeric id\"}", "application/json");
        return;
      }
      static const std::map<std::string, int> window_hours = {{"24h", 24}, {"7d", 7 * 24}, {"30d", 30 * 24}, {"all", 0}};
      std::string window = req.params.count("window") ? req.params.at("window") : "24h";
      time_t now = time(NULL);
      int64_t first_hour;
      if (window == "restart") {
        first_hour = stats_since_ - stats_since_ % 3600;
      } else if (window_hours.count(window)) {
        int hours = window_hours.at(window);
        first_hour = hours ? (now - now % 3600) - (int64_t)(hours - 1) * 3600 : 0;
      } else {
        res.status = 400;
        res.set_content("{\"error\": \"window must be restart, 24h, 7d, 30d or all\"}", "application/json");
        return;
      }
      json response = get_system_history(sys, kind, id, first_hour);
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Per-frequency call quality and busiest talkgroups since startup
        server_.Get("/api/system/stats", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      System *sys = find_system_param(req, res);
      if (!sys) return;
      // window: restart (default, from memory) or 24h / 7d / 30d / all (hourly totals in the database)
      static const std::map<std::string, int> window_hours = {{"24h", 24}, {"7d", 7 * 24}, {"30d", 30 * 24}, {"all", 0}};
      std::string window = req.params.count("window") ? req.params.at("window") : "restart";
      json response;
      if (window == "restart") {
        response = get_system_stats(sys->get_sys_num(), 25);
      } else if (window_hours.count(window)) {
        if (!db_read_) {
          res.status = 503;
          res.set_content("{\"error\": \"history is not available (no database)\"}", "application/json");
          return;
        }
        // Summing long windows reads many hourly rows; every open page asks every 15 s
        const auto key = std::make_pair(sys->get_sys_num(), window);
        time_t now = time(NULL);
        {
          std::lock_guard<std::mutex> lock(stats_window_cache_mutex_);
          auto it = stats_window_cache_.find(key);
          if (it != stats_window_cache_.end() && now - it->second.first < STATS_WINDOW_CACHE_SECONDS) {
            res.set_content(it->second.second, "application/json");
            return;
          }
        }
        std::string body = get_system_stats_window(sys, window_hours.at(window), window, 25).dump(-1, ' ', false, json::error_handler_t::replace);
        {
          std::lock_guard<std::mutex> lock(stats_window_cache_mutex_);
          stats_window_cache_[key] = {now, body};
        }
        res.set_content(body, "application/json");
        return;
      } else {
        res.status = 400;
        res.set_content("{\"error\": \"window must be restart, 24h, 7d, 30d or all\"}", "application/json");
        return;
      }
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Admin: Get login history
        server_.Get("/api/admin/login-history", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_admin_auth(req, res)) return;
      
      auto history = server_.get_login_history();
      json response = json::array();

      for (const auto &attempt : history) {
        json entry = {
            {"timestamp", attempt.timestamp},
            {"username", attempt.username},
            {"client_ip", attempt.client_ip},
            {"success", attempt.success},
            {"access_level", attempt.access_level}};
        response.push_back(entry);
      }

      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Admin: Get trunk-recorder config
        server_.Get("/api/admin/config", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_admin_auth(req, res)) return;
      
      try {
        std::string config_path = tr_config_->config_file;
        std::ifstream config_file(config_path);
        if (!config_file.good()) {
          res.status = 404;
          json error = {{"error", "Config file not found: " + config_path}};
          res.set_content(error.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
          return;
        }

        std::string config_content((std::istreambuf_iterator<char>(config_file)),
                                   std::istreambuf_iterator<char>());
        json response = {
            {"content", config_content},
            {"path", config_path}};
        res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
      } catch (const std::exception &e) {
        res.status = 500;
        json error = {{"error", std::string("Failed to read config: ") + e.what()}};
        res.set_content(error.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
      } });

        // Admin: Save config (atomic with backup)
        server_.Post("/api/admin/save-config", [this](const httplib::Request &req, httplib::Response &res)
                     {
      if (!require_admin_auth(req, res)) return;
      
      try {
        json request_data;
        try {
          request_data = json::parse(req.body);
        } catch (const json::exception &e) {
          BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to parse save-config request: " << e.what();
          BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Request body length: " << req.body.size();
          res.status = 400;
          json error = {{"error", std::string("Invalid request: ") + e.what()}};
          res.set_content(error.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
          return;
        }

        std::string new_content = request_data.value("content", "");
        // Never a client-supplied path: that would allow writing any file
        const std::string config_path = tr_config_->config_file;

        if (new_content.empty()) {
          res.status = 400;
          res.set_content("{\"error\": \"Empty configuration\"}", "application/json");
          return;
        }

        // Validate JSON on server side
        try {
          auto parsed = json::parse(new_content);
          (void)parsed; // Suppress unused warning
        } catch (const std::exception &e) {
          res.status = 400;
          json error = {{"error", std::string("Invalid JSON: ") + e.what()}};
          res.set_content(error.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
          return;
        }

        // The previous config is kept as .bak.trweb
        std::string backup_path = config_path + ".bak.trweb";
        std::string error;
        if (!save_config_file(config_path, backup_path, new_content, error)) {
          BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Saving " << config_path << " failed: " << error;
          res.status = 500;
          json err = {{"error", "Failed to save configuration: " + error}};
          res.set_content(err.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
          return;
        }

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Configuration saved (backup: " << backup_path << ")";

        json response = {
            {"success", true},
            {"backup", backup_path},
            {"message", "Configuration saved successfully"}};
        res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");

      } catch (const json::exception &e) {
        res.status = 400;
        json error = {{"error", std::string("Invalid request: ") + e.what()}};
        res.set_content(error.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
      } catch (const std::exception &e) {
        res.status = 500;
        json error = {{"error", std::string("Failed to save config: ") + e.what()}};
        res.set_content(error.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
      } });

        // Admin: Restart trunk-recorder
        server_.Post("/api/admin/restart", [this](const httplib::Request &req, httplib::Response &res)
                     {
      if (!require_admin_auth(req, res)) return;
      
      BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Restart requested via web admin interface";

      json response = {
          {"status", "ok"},
          {"message", "Restart initiated"},
          {"timestamp", time(NULL)}};
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");

      // SIGINT is trunk-recorder's graceful exit; a supervisor (systemd, Docker) restarts it.
      // Delayed so the response goes out first.
      std::string prefix = log_prefix_;
      std::thread([prefix]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        BOOST_LOG_TRIVIAL(warning) << prefix << "Executing restart: requesting graceful shutdown (SIGINT)";
        kill(getpid(), SIGINT);
      }).detach(); });

        // Whoami - returns current user's auth level and username
        server_.Get("/api/whoami", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!check_auth_hybrid(req, false)) {
        res.status = 401;
        res.set_content("{\"error\": \"Not authenticated\"}", "application/json");
        return;
      }

      std::string auth_level = "user";
      std::string username = "";
      
      // Try to get session info first
      std::string token = request_session_token(req);

      bool is_admin = false;
      bool identity_resolved = false;
      if (!token.empty() && get_session_info(token, username, is_admin)) {
        // Got session info
        auth_level = is_admin ? "admin" : "user";
        identity_resolved = true;
      } else {
        // Fall back to Basic Auth parsing
        std::string auth_header = req.get_header("Authorization");
        if (!auth_header.empty() && auth_header.find("Basic ") == 0) {
          std::string provided_creds = auth_header.substr(6);
          if (!expected_admin_creds_.empty() && constant_time_compare(provided_creds, expected_admin_creds_)) {
            auth_level = "admin";
            username = admin_username_.empty() ? "admin" : admin_username_;
            identity_resolved = true;
          } else if (!expected_user_creds_.empty() && constant_time_compare(provided_creds, expected_user_creds_)) {
            // No admin creds configured: user credentials are de-facto admin
            auth_level = expected_admin_creds_.empty() ? "admin" : "user";
            username = username_.empty() ? "user" : username_;
            identity_resolved = true;
          }
        }
      }

      // No identity resolved → open/anonymous access (no credentials configured)
      if (!identity_resolved) {
        auth_level = "anonymous";
      }

      json response = {
          {"auth_level", auth_level},
          {"username", username},
          {"timestamp", time(NULL)}};
      res.set_content(response.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });

        // Health check
        server_.Get("/health", [this](const httplib::Request &req, httplib::Response &res)
                    {
      json health = {
          {"status", "ok"},
          {"timestamp", time(NULL)},
          {"https", server_.is_https()}};
      res.set_content(health.dump(-1, ' ', false, json::error_handler_t::replace), "application/json"); });
    }

    void resend_recorders()
    {
        json recorders_json = json::array();

        // Add regular recorders
        for (auto *source : tr_sources_)
        {
            std::vector<Recorder *> sourceRecorders = source->get_recorders();
            for (auto *recorder : sourceRecorders)
            {
                recorders_json.push_back(get_recorder_json(recorder));
            }
        }

        // Add control channels as pseudo-recorders
        for (auto *sys : tr_systems_)
        {
            if (sys->control_channel_count() > 0)
            {
                double ctrl_freq = sys->get_current_control_channel();

                // Find which device this control channel belongs to using cached ranges
                int device_num = -1;
                for (const auto &range : device_ranges_)
                {
                    if (ctrl_freq >= range.min_hz && ctrl_freq <= range.max_hz)
                    {
                        device_num = range.num;
                        break;
                    }
                }

                // Capitalize system type to match recorder type format (P25, not p25)
                std::string sys_type = sys->get_system_type();
                if (!sys_type.empty())
                {
                    sys_type[0] = std::toupper(sys_type[0]);
                }

                // Create pseudo-recorder for control channel
                json ctrl_recorder = {
                    {"id", "ctrl_" + std::to_string(sys->get_sys_num())},
                    {"src_num", device_num},
                    {"rec_num", "CC" + std::to_string(sys->get_sys_num())}, // Special marker for control channel
                    {"type", sys_type + " CC"},
                    {"duration", 0.0},
                    {"freq", ctrl_freq},
                    {"count", 0},
                    {"rec_state", 0}, // MONITORING state
                    {"rec_state_type", "MONITORING"},
                    {"squelched", false},
                    {"is_control_channel", true},
                    {"sys_num", sys->get_sys_num()},
                    {"sys_name", sys->get_short_name()}};

                recorders_json.push_back(ctrl_recorder);
            }
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_recorders_ = recorders_json;
        }

        dirty_flags_.fetch_or(DIRTY_RECORDERS);
    }

    void resend_devices()
    {
        json devices_json = json::array();

        for (auto *source : tr_sources_)
        {
            json gain_stages = json::array();
            for (const auto &stage : source->get_gain_stages())
            {
                if (stage.value == 0)
                    continue;
                gain_stages.push_back({{"name", stage.stage_name},
                                       {"value", stage.value}});
            }

            devices_json.push_back({{"src_num", source->get_num()},
                                    {"driver", source->get_driver()},
                                    {"device", source->get_device()},
                                    {"center", source->get_center()},
                                    {"rate", source->get_rate()},
                                    {"error", source->get_error()},
                                    {"gain", source->get_gain()},
                                    {"digital_recorders", source->digital_recorder_count()},
                                    {"analog_recorders", source->analog_recorder_count()},
                                    {"autotune_enabled", source->get_autotune_source()},
                                    {"autotune_offset_hz", source->get_autotune_source() ? source->get_source_error() : 0},
                                    {"gain_stages", gain_stages}});
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_devices_ = devices_json;
        }

        dirty_flags_.fetch_or(DIRTY_DEVICES);
    }

    json get_recorder_json(Recorder *recorder)
    {
        boost::property_tree::ptree stat_node = recorder->get_stats();

        return {
            {"id", stat_node.get<std::string>("id")},
            {"src_num", stat_node.get<int>("srcNum")},
            {"rec_num", stat_node.get<int>("recNum")},
            {"type", stat_node.get<std::string>("type")},
            {"duration", stat_node.get<double>("duration")},
            {"freq", recorder->get_freq()},
            {"count", stat_node.get<int>("count")},
            {"rec_state", stat_node.get<int>("state")},
            {"rec_state_type", state_name(stat_node.get<int>("state"))},
            {"squelched", recorder->is_squelched()}};
    }

    json get_call_json(Call *call)
    {
        boost::property_tree::ptree stat_node = call->get_stats();
        System *sys = call->get_system();
        Talkgroup *tg = sys->find_talkgroup(stat_node.get<int>("talkgroup"));

        json call_json = {
            {"id", stat_node.get<std::string>("id")},
            {"call_num", stat_node.get<long>("callNum")},
            {"sys_num", stat_node.get<int>("sysNum")},
            {"sys_name", stat_node.get<std::string>("shortName")},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"freq", stat_node.get<double>("freq")},
            {"unit", stat_node.get<long>("srcId")},
            {"unit_alpha_tag", sys->find_unit_tag(stat_node.get<long>("srcId"))},
            {"talkgroup", stat_node.get<int>("talkgroup")},
            {"talkgroup_alpha_tag", ""},
            {"talkgroup_description", ""},
            {"elapsed", stat_node.get<long>("elapsed")},
            {"length", stat_node.get<double>("length")},
            {"call_state", stat_node.get<int>("state")},
            {"call_state_type", state_name(stat_node.get<int>("state"))},
            {"phase2_tdma", stat_node.get<bool>("phase2")},
            {"tdma_slot", call->get_tdma_slot()},
            {"analog", stat_node.get<bool>("analog", false)},
            {"conventional", stat_node.get<bool>("conventional")},
            {"encrypted", stat_node.get<bool>("encrypted")},
            {"emergency", stat_node.get<bool>("emergency")},
            {"start_time", stat_node.get<long>("startTime")},
            {"rec_num", stat_node.get<int>("recNum", -1)},
            {"src_num", stat_node.get<int>("srcNum", -1)},
            {"rec_state", stat_node.get<int>("recState", -1)},
            {"rec_state_type", state_name(stat_node.get<int>("recState", -1))}};

        if (tg != nullptr)
        {
            call_json["talkgroup_alpha_tag"] = tg->alpha_tag;
            call_json["talkgroup_description"] = tg->description;
        }

        return call_json;
    }

    std::string int_to_hex(int num, int places)
    {
        if (num == 0 && places == 0)
            return "0";
        std::stringstream stream;
        stream << std::setfill('0') << std::uppercase;
        if (places > 0)
            stream << std::setw(places);
        stream << std::hex << num;
        return stream.str();
    }

    json get_system_json(System *sys)
    {
        boost::property_tree::ptree stat_node = sys->get_stats();

        double control_channel = 0.0;
        if (sys->control_channel_count() > 0)
        {
            control_channel = sys->get_current_control_channel();
        }

        json control_channels = json::array();
        try
        {
            for (double cc : sys->get_control_channels())
            {
                control_channels.push_back(cc);
            }
        }
        catch (...)
        {
        }

        return {
            {"sys_num", stat_node.get<int>("id")},
            {"sys_name", stat_node.get<std::string>("name")},
            {"short_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"type", stat_node.get<std::string>("type")},
            {"sysid", int_to_hex(stat_node.get<int>("sysid"), 0)},
            {"wacn", int_to_hex(stat_node.get<int>("wacn"), 0)},
            {"nac", int_to_hex(stat_node.get<int>("nac"), 0)},
            {"rfss", sys->get_sys_rfss()},
            {"site_id", sys->get_sys_site_id()},
            {"control_channel", control_channel},
            {"control_channels", control_channels},
            {"talkgroups_file", sys->get_talkgroups_file()},
            {"unit_tags_file", sys->get_unit_tags_file()},
            {"unit_tags_mode", sys->get_unit_tags_mode()},
            {"unit_tags_ota_file", sys->get_unit_tags_ota_file()}};
    }

    // Factory method
public:
    static boost::shared_ptr<Tr_Web> create()
    {
        return boost::shared_ptr<Tr_Web>(new Tr_Web());
    }
};

BOOST_DLL_ALIAS(
    Tr_Web::create,
    create_plugin)
