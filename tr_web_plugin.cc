// tr-web: Web Status Plugin for Trunk-Recorder

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <signal.h>
#include <string>
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

// Plugin headers
#include "httplib.h"
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

    // ============================================================================
    // WEB-RELATED CODE
    // ============================================================================

    // Console log buffer
    mutable std::mutex console_mutex_;
    std::deque<std::string> console_logs_;

    // Pending console lines for SSE (bounded, flushed from broadcast thread)
    mutable std::mutex console_pending_mutex_;
    std::deque<std::string> console_pending_;
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
            timestamped_line.resize(MAX_CONSOLE_LINE_BYTES);
            timestamped_line += "…(truncated)";
        }

        {
            std::lock_guard<std::mutex> lock(console_mutex_);
            console_logs_.push_back(timestamped_line);
            while (console_logs_.size() > console_max_lines_)
            {
                console_logs_.pop_front();
            }
        }

        // Queue for SSE broadcast (do not write sockets from trunk-recorder threads)
        {
            std::lock_guard<std::mutex> lock(console_pending_mutex_);
            static constexpr size_t MAX_PENDING = 2000;
            if (console_pending_.size() >= MAX_PENDING)
            {
                ++console_pending_dropped_;
            }
            else
            {
                console_pending_.push_back(timestamped_line);
            }
        }
    }

    json get_console_logs() const
    {
        // Copy data while holding lock, build JSON after releasing
        std::deque<std::string> logs_copy;
        {
            std::lock_guard<std::mutex> lock(console_mutex_);
            logs_copy = console_logs_;
        }
        
        json logs = json::array();
        for (const auto &line : logs_copy)
        {
            logs.push_back(line);
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

    // Pre-computed credentials for constant-time comparison
    std::string expected_user_creds_;
    std::string expected_admin_creds_;

    // Rate limiting for authentication attempts
    mutable std::mutex auth_rate_limit_mutex_;
    mutable std::map<std::string, std::vector<time_t>> auth_attempts_;
    static constexpr size_t MAX_AUTH_ATTEMPTS = 10;
    static constexpr time_t AUTH_WINDOW_SECONDS = 60;

    // Session management
    struct Session {
        std::string token;
        std::string username;
        bool is_admin;
        time_t created;
        time_t last_access;
    };
    mutable std::mutex sessions_mutex_;
    std::map<std::string, Session> sessions_; // token -> Session
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

    // Configuration for affiliation tracking
    int affiliation_timeout_ = 12;
    std::string affiliation_cache_;
    int affiliation_autosave_ = 300;
    time_t last_affiliation_save_ = 0;

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
        DIRTY_DEVICES = 1u << 4
    };

    void enqueue_sse_event(const std::string &event, std::string data)
    {
        // Only enqueue if there are connected SSE clients
        if (server_.sse_client_count() == 0)
        {
            return;
        }

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

    // Send current affiliation state to newly connected Gephi clients
    void send_gephi_initial_state()
    {
        std::lock_guard<std::mutex> lock(affiliation_state_mutex_);

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

            // Add unit->tg pairings to edge_map (voice transmissions only)
            for (const auto &[tg_id, count] : unit.tg_activity)
            {
                if (tg_id == 0 || tg_id == -1)
                    continue;
                std::string edge_key = "TG-" + std::to_string(tg_id) + "-" + std::to_string(unit.id);
                // Use voice count only for now (future: separate edges for voice/data)
                double unit_weight = (unit.tx_count.voice > 0) ? static_cast<double>(count.voice) / unit.tx_count.voice : 0.0;
                // Round to nearest 0.01, min 0.01 if > 0
                if (unit_weight > 0.0)
                {
                    unit_weight = std::max(0.01, std::round(unit_weight * 100.0) / 100.0);
                }
                edge_map[edge_key] = {
                    {"unit", unit.id},
                    {"tg", tg_id},
                    {"unit_weight", unit_weight}};
            }
        }

        // Gather all talkgroup nodes
        for (const auto &[key, tg] : talkgroup_states_)
        {
            if (tg.id == 0 || tg.id == -1)
                continue;
            std::string node_id = "TG-" + std::to_string(tg.id);
            std::string label = tg.alias.empty() ? ("TG " + std::to_string(tg.id)) : tg.alias;
            std::string color = tg.encr_seen ? GEPHI_COLOR_RED : GEPHI_COLOR_GREEN;
            // Get status using unlocked version (we already hold affiliation_state_mutex_)
            std::string status = get_talkgroup_status_unlocked(tg.wacn, tg.sysid, tg.id);
            json node_data = {
                {"id", node_id},
                {"label", label},
                {"color", color},
                {"size", 25},
                {"encryption", tg.encr_seen},
                {"status", status}};

            all_nodes[node_id] = node_data;
            node_count++;

            // Add tg->unit pairings to edge_map (reverse), and set encrypted if tg.encr_seen
            // Voice transmissions only for now (future: separate edges for voice/data)
            for (const auto &[unit_id, count] : tg.unit_activity)
            {
                if (unit_id == 0 || unit_id == -1)
                    continue;
                std::string edge_key = "TG-" + std::to_string(tg.id) + "-" + std::to_string(unit_id);
                // Use voice count only for now
                double tg_weight = (tg.tx_count.voice > 0) ? static_cast<double>(count.voice) / tg.tx_count.voice : 0.0;
                // Round to nearest 0.01, min 0.01 if > 0
                if (tg_weight > 0.0)
                {
                    tg_weight = std::max(0.01, std::round(tg_weight * 100.0) / 100.0);
                }
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
                        {"encrypted", tg.encr_seen}};
                }
            }
        }
        // Send all nodes to Gephi
        if (!all_nodes.empty())
        {
            json an_msg = {{"an", all_nodes}};
            server_.broadcast_raw_to_path("/graph-stream", an_msg.dump(-1) + "\r\n");
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
            // Look up the TxCount for this unit-tg pair
            std::string status = "affil";  // Default to affiliation
            std::string color = GEPHI_COLOR_BLUE;  // Default to blue
            bool edge_encrypted = it.value().value("encrypted", false);
            
            // Check if this edge has any voice transmissions
            double voice_weight = it.value().value("unit_weight", 0.0);
            if (voice_weight > 0.0)
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
            server_.broadcast_raw_to_path("/graph-stream", ae_msg.dump(-1) + "\r\n");
        }

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Sent " << node_count << " nodes and " << edge_count << " edges to new Gephi connection";
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
        return add_node.dump(-1) + "\r\n";
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
        return change_node.dump(-1) + "\r\n";
    }

    std::string create_gephi_add_talkgroup_node(System *sys, long tg_id, const std::string &tg_alpha, bool encrypted)
    {
        std::string node_id = "TG-" + std::to_string(tg_id);
        std::string label = tg_alpha.empty() ? std::to_string(tg_id) : tg_alpha;
        std::string status = get_talkgroup_status(sys->get_wacn(), sys->get_sys_id(), tg_id);

        json node_data = {
            {"id", node_id},
            {"label", label},
            {"color", encrypted ? GEPHI_COLOR_RED : GEPHI_COLOR_GREEN},
            {"status", status},
            {"encryption", encrypted},
            {"size", 25}};

        json add_node = {{"an", {{node_id, node_data}}}};
        return add_node.dump(-1) + "\r\n";
    }

    std::string create_gephi_change_talkgroup_node(System *sys, long tg_id, const std::string &tg_alpha, bool encrypted)
    {
        std::string node_id = "TG-" + std::to_string(tg_id);
        std::string label = tg_alpha.empty() ? std::to_string(tg_id) : tg_alpha;
        std::string status = get_talkgroup_status(sys->get_wacn(), sys->get_sys_id(), tg_id);

        json node_data = {
            {"id", node_id},
            {"label", label},
            {"color", encrypted ? GEPHI_COLOR_RED : GEPHI_COLOR_GREEN},
            {"status", status},
            {"encryption", encrypted},
            {"size", 25}};

        json change_node = {{"cn", {{node_id, node_data}}}};
        return change_node.dump(-1) + "\r\n";
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
        return add_edge.dump(-1) + "\r\n";
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
        return change_edge.dump(-1) + "\r\n";
    }

    void send_gephi_unit_tg_event(System *sys, long unit_id, long tg_id, bool encrypted = false)
    {
        // Filter out anomalous IDs that are not valid for graph theory
        // -1 indicates unknown/invalid radio ID
        // 0 indicates uninitialized or missing unit/talkgroup ID
        if (unit_id == -1 || unit_id == 0 || tg_id == 0)
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

        // Always send both "add" and "change" events (no state tracking)
        // - "add" events set initial color based on current state
        // - "change" events update color dynamically
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
    static constexpr const char *GEPHI_COLOR_RED = "#a83232";
    static constexpr const char *GEPHI_COLOR_GREEN = "#32a852";
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
    }

    // Update unit/talkgroup state tracking for DATA events (affiliations, locations)
    void update_affiliation_state_data(System *sys, long unit_id, long tg_id)
    {
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
    }

    void set_unit_registration(System *sys, long unit_id, bool registered)
    {
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
    }

    // Update unit state for non-voice events (acknowledgements, data, location, etc.)
    // This refreshes the last_active timestamp without talkgroup affiliation
    void update_unit_state(System *sys, long unit_id, bool encrypted = false)
    {
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

    // Helper: Check if IP is rate limited
    bool is_rate_limited(const std::string &client_ip) const
    {
        // Don't rate limit "unknown" IPs (typically localhost without proxy headers)
        // This allows local testing and direct connections to work
        if (client_ip == "unknown")
        {
            return false;
        }
        
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

    // Helper: Check if request has valid authentication
    bool check_auth(const httplib::Request &req, bool require_admin = false) const
    {
        // If no auth configured, allow access
        if (username_.empty() || password_.empty())
        {
            return true;
        }

        // Extract client IP for rate limiting and logging
        std::string client_ip = "unknown";
        auto remote_addr = req.headers.find("X-Forwarded-For");
        if (remote_addr != req.headers.end())
        {
            client_ip = remote_addr->second;
        }
        else
        {
            remote_addr = req.headers.find("X-Real-IP");
            if (remote_addr != req.headers.end())
            {
                client_ip = remote_addr->second;
            }
        }

        // Check rate limiting
        if (is_rate_limited(client_ip))
        {
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Rate limit exceeded for " << client_ip
                                       << " on " << req.path;
            return false;
        }

        auto auth_it = req.headers.find("Authorization");
        if (auth_it == req.headers.end())
        {
            record_auth_attempt(client_ip);
            BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Missing Authorization header from "
                                     << client_ip << " for " << req.path;
            return false;
        }

        const std::string &auth_header = auth_it->second;
        if (auth_header.length() < 7 || auth_header.substr(0, 6) != "Basic ")
        {
            record_auth_attempt(client_ip);
            BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Invalid Authorization format from "
                                     << client_ip << " for " << req.path;
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
            // Admin endpoints require admin credentials
            auth_success = !expected_admin_creds_.empty() &&
                           constant_time_compare(provided_creds, expected_admin_creds_);
        }
        else
        {
            // Regular endpoints accept either user or admin credentials
            auth_success = constant_time_compare(provided_creds, expected_user_creds_) ||
                           (!expected_admin_creds_.empty() &&
                            constant_time_compare(provided_creds, expected_admin_creds_));
        }

        if (!auth_success)
        {
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

    /// Generate a random session token
    std::string generate_session_token() const
    {
        std::stringstream ss;
        ss << std::hex << time(NULL) << "-" << rand() << "-" << rand();
        return httplib::base64_encode(ss.str());
    }

    /// Create a new session for a user
    std::string create_session(const std::string &username, bool is_admin)
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        
        std::string token = generate_session_token();
        time_t now = time(NULL);
        
        Session session;
        session.token = token;
        session.username = username;
        session.is_admin = is_admin;
        session.created = now;
        session.last_access = now;
        
        sessions_[token] = session;
        
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Created session for " << username 
                                 << (is_admin ? " (admin)" : " (user)");
        return token;
    }

    /// Check if a session token is valid and not expired
    bool validate_session(const std::string &token, bool require_admin = false)
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        
        auto it = sessions_.find(token);
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
            return false;
        }
        
        // Check admin requirement
        if (require_admin && !session.is_admin)
        {
            return false;
        }
        
        // Update last access time
        session.last_access = now;
        return true;
    }

    /// Get session info (for whoami endpoint)
    bool get_session_info(const std::string &token, std::string &username, bool &is_admin)
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        
        auto it = sessions_.find(token);
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
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(token);
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
                it = sessions_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    /// Check authentication: session token OR Basic Auth
    /// This allows both web interface (session) and external tools (Basic Auth) to work
    bool check_auth_hybrid(const httplib::Request &req, bool require_admin = false) const
    {
        // First check for session token in Authorization header or cookie
        std::string auth_header = req.get_header("Authorization");
        if (!auth_header.empty() && auth_header.find("Bearer ") == 0)
        {
            std::string token = auth_header.substr(7); // Remove "Bearer "
            BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Checking Bearer token for " << req.path 
                                     << " (require_admin=" << require_admin << ")";
            bool valid = const_cast<Tr_Web*>(this)->validate_session(token, require_admin);
            if (valid) {
                BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Bearer token validated successfully";
                return true;
            }
            BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Bearer token validation failed";
        }
        
        // Check for token in cookie
        std::string cookie_header = req.get_header("Cookie");
        if (!cookie_header.empty())
        {
            // Simple cookie parsing: look for session=TOKEN
            size_t pos = cookie_header.find("session=");
            if (pos != std::string::npos)
            {
                size_t start = pos + 8; // Length of "session="
                size_t end = cookie_header.find(";", start);
                std::string token = cookie_header.substr(start, end == std::string::npos ? std::string::npos : end - start);
                BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Checking cookie token for " << req.path 
                                         << " (require_admin=" << require_admin << ")";
                if (const_cast<Tr_Web*>(this)->validate_session(token, require_admin))
                {
                    BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Cookie token validated successfully";
                    return true;
                }
                BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Cookie token validation failed";
            }
        }
        
        // Fall back to HTTP Basic Auth (for SSE/graphstream and external tools)
        BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Falling back to Basic Auth for " << req.path;
        return check_auth(req, require_admin);
    }

    /// Helper: Check if request has valid user authentication (for API endpoints)
    /// Returns true if authenticated, false otherwise
    /// NOTE: Does NOT set WWW-Authenticate header to avoid triggering browser auth dialogs
    /// Frontend should handle 401 responses by showing login modal
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

    /// Helper: Check if request has valid admin authentication (for admin API endpoints)
    /// Returns true if authenticated as admin, false otherwise
    /// NOTE: Does NOT set WWW-Authenticate header to avoid triggering browser auth dialogs
    /// Frontend should handle 401 responses by showing login modal
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
        time_t idle_threshold = now - (affiliation_timeout_ * 3600);

        // Grey if deregistered OR idle
        if (!unit.registered || unit.last_active < idle_threshold)
        {
            return GEPHI_COLOR_GREY;
        }

        return unit.encr_seen ? GEPHI_COLOR_RED : GEPHI_COLOR_BLUE;
    }

    /// Get status string for a unit based on its state
    /// Returns: "active" (recently active), "idle" (inactive but registered), or "off" (deregistered)
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
            return "u_off";
        }

        const UnitState &unit = it->second;
        time_t now = time(NULL);
        time_t idle_threshold = now - (affiliation_timeout_ * 3600);

        // Off: deregistered units
        if (!unit.registered)
        {
            return "u_off";
        }
        // Idle: registered but no recent activity
        else if (unit.last_active < idle_threshold)
        {
            return "u_idle";
        }
        // Active: recent activity within timeout window
        else
        {
            return "u_active";
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
            return "tg_off";
        }

        const TalkgroupState &tg = it->second;
        time_t now = time(NULL);
        time_t idle_threshold = now - (affiliation_timeout_ * 3600);

        // Active: recent activity within timeout window
        if (tg.last_active > idle_threshold)
        {
            return "tg_active";
        }
        // Idle: seen before but no recent activity
        else if (tg.last_active > 0)
        {
            return "tg_idle";
        }
        // Off: never seen activity
        else
        {
            return "tg_off";
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

    // Get affiliation data for API
    json get_affiliation_data(int limit = 0, bool units_only = false, bool talkgroups_only = false) const
    {
        // Copy data while holding lock, build JSON after releasing
        std::map<std::string, UnitState> units_copy;
        std::map<std::string, TalkgroupState> talkgroups_copy;
        size_t total_units, total_talkgroups;
        int timeout_hours;
        
        {
            std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
            if (!talkgroups_only) {
                units_copy = unit_states_;
            }
            if (!units_only) {
                talkgroups_copy = talkgroup_states_;
            }
            total_units = unit_states_.size();
            total_talkgroups = talkgroup_states_.size();
            timeout_hours = affiliation_timeout_;
        }
        
        time_t now = time(NULL);
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
            {"total_talkgroups", total_talkgroups}};

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

    // Save affiliation state to JSON file
    void save_affiliation_state()
    {
        if (affiliation_cache_.empty())
            return;

        try
        {
            json persist_data = {
                {"version", 1},
                {"saved_at", time(NULL)},
                {"units", json::array()},
                {"talkgroups", json::array()}};

            {
                std::lock_guard<std::mutex> lock(affiliation_state_mutex_);

                for (const auto &pair : unit_states_)
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

                for (const auto &pair : talkgroup_states_)
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
            }

            // Write to file atomically (write to temp, then rename)
            std::string temp_file = affiliation_cache_ + ".tmp";
            std::ofstream out(temp_file);
            if (!out.good())
            {
                BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Failed to open " << temp_file << " for writing";
                return;
            }
            out << persist_data.dump(2); // Pretty print with 2-space indent
            out.close();

            // Atomic rename
            if (std::rename(temp_file.c_str(), affiliation_cache_.c_str()) != 0)
            {
                BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Failed to rename temp file to " << affiliation_cache_;
            }
            else
            {
                BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Saved affiliation state to " << affiliation_cache_;
            }
        }
        catch (const std::exception &e)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to save affiliation state: " << e.what();
        }
    }

    // Load affiliation state from JSON file
    void load_affiliation_state()
    {
        if (affiliation_cache_.empty())
            return;

        try
        {
            std::ifstream in(affiliation_cache_);
            if (!in.good())
            {
                BOOST_LOG_TRIVIAL(info) << log_prefix_ << "No existing affiliation state file found (this is normal on first run)";
                return;
            }

            json persist_data;
            in >> persist_data;

            // Check version
            int version = persist_data.value("version", 0);
            if (version != 1)
            {
                BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Unsupported affiliation state version: " << version;
                return;
            }

            std::lock_guard<std::mutex> lock(affiliation_state_mutex_);
            unit_states_.clear();
            talkgroup_states_.clear();

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

                    std::string key = make_unit_key(unit.wacn, unit.sysid, unit.id);
                    unit_states_[key] = unit;
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

                    std::string key = make_tg_key(tg.wacn, tg.sysid, tg.id);
                    talkgroup_states_[key] = tg;
                }
            }

            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Loaded " << unit_states_.size() << " units and "
                                    << talkgroup_states_.size() << " talkgroups from " << affiliation_cache_;
        }
        catch (const std::exception &e)
        {
            BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to load affiliation state: " << e.what();
        }
    }

    // Generate display name for system with number prefix
    std::string get_unique_sys_name(System *sys)
    {
        int sys_num = sys->get_sys_num();
        std::string short_name = sys->get_short_name();
        return std::to_string(sys_num + 1) + ". " + short_name;
    }

    void add_rate_point(const std::string &sys_name, double rate)
    {
        std::lock_guard<std::mutex> lock(data_mutex_);

        RatePoint point;
        point.timestamp = time(NULL);
        point.rate = rate;

        auto &history = rate_history_[sys_name];
        history.push_back(point);

        // Trim to max size (60 minutes of data)
        while (history.size() > MAX_RATE_HISTORY)
        {
            history.pop_front();
        }
    }

    void add_call_rate_point(const std::string &sys_name, int count)
    {
        std::lock_guard<std::mutex> lock(data_mutex_);

        RatePoint point;
        point.timestamp = time(NULL);
        point.rate = static_cast<double>(count);

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
        affiliation_cache_ = config_data.value("affiliation_cache", "affiliations.json");
        affiliation_autosave_ = config_data.value("affiliation_autosave", 300);

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Port:           " << port_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Bind:           " << bind_address_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Auth:           " << (username_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Admin Auth:     " << (admin_username_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "HTTPS:          " << (ssl_cert_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Console Lines:  " << console_max_lines_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Theme:          " << theme_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Affil Cache:    " << (affiliation_cache_.empty() ? "[disabled]" : affiliation_cache_);
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Affil Timeout:  " << affiliation_timeout_ << "h";
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Affil Autosave: " << affiliation_autosave_ << "s";

        return 0;
    }

    int init(Config *config, std::vector<Source *> sources, std::vector<System *> systems) override
    {
        tr_config_ = config;
        tr_sources_ = sources;
        tr_systems_ = systems;
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

        // Configure httplib authentication (used as backup / for SSE endpoints)
        // Admin credentials protect admin endpoints if plugin auth fails
        if (!admin_username_.empty() && !admin_password_.empty())
        {
            server_.set_admin_auth(admin_username_, admin_password_);
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Configured httplib admin auth";
        }

        // Enable SSE authentication callback for /events and /graph-stream
        server_.set_sse_auth_callback([this](const httplib::Request &req) -> bool {
            return this->check_auth_hybrid(req, false);
        });
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Configured SSE authentication callback";

        // Note: We use session-based auth for web interface and Basic Auth for SSE/external tools
        // Authentication is checked manually in each endpoint via require_auth() / require_admin_auth()
        // This allows proper login/logout functionality while keeping SSE/graphstream working

        // Setup console log capture
        setup_log_capture();

        // Load persisted affiliation state
        load_affiliation_state();

        // Prime initial caches for first page load
        resend_recorders();
        resend_devices();
        setup_systems(tr_systems_);

        // Start server in background
        running_ = true;

        // Broadcast thread (flushes SSE without blocking trunk-recorder)
        broadcast_thread_ = std::thread([this]()
                                        {
      auto last_console_flush = std::chrono::steady_clock::now();

      while (running_) {
        // Avoid work if nobody is connected.
        const bool has_clients = (server_.sse_client_count() > 0);

        // Flush console lines at ~5Hz, batched.
        const auto now = std::chrono::steady_clock::now();
        if (has_clients && (now - last_console_flush) >= std::chrono::milliseconds(200)) {
          last_console_flush = now;

          std::deque<std::string> lines;
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
            for (const auto &l : lines)
              payload["lines"].push_back(l);
            payload["dropped"] = dropped;
            server_.broadcast_sse("console_batch", payload.dump(-1));
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
              server_.broadcast_sse("systems", payload.dump(-1));
            }
            if (flags & DIRTY_RECORDERS) {
              json payload = {{"type", "recorders"}, {"recorders", recorders}};
              server_.broadcast_sse("recorders", payload.dump(-1));
            }
            if (flags & DIRTY_CALLS) {
              json payload = {{"type", "calls"}, {"calls_active", calls}};
              server_.broadcast_sse("calls", payload.dump(-1));
            }
            if (flags & DIRTY_RATES) {
              json payload = {{"type", "rates"}, {"rates", rates}};
              server_.broadcast_sse("rates", payload.dump(-1));
            }
            if (flags & DIRTY_DEVICES) {
              json payload = {{"type", "devices"}, {"devices", devices}};
              server_.broadcast_sse("devices", payload.dump(-1));
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
            server_.broadcast_sse("event_drop", payload.dump(-1));
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
          graph_event_queue_dropped_ = 0;
        }

        // Send initial state to new Gephi connections
        if (gephi_initial_dump_pending_.exchange(false, std::memory_order_acquire)) {
          send_gephi_initial_state();
        }

        // Flush graph events to /graph-stream clients
        if (!graph_events.empty()) {
        }
        for (const auto &graph_event : graph_events) {
          server_.broadcast_raw_to_path("/graph-stream", graph_event);
        }

        // Periodic save of affiliation state
        if (!affiliation_cache_.empty()) {
          time_t current_time = time(NULL);
          if (current_time - last_affiliation_save_ >= affiliation_autosave_) {
            save_affiliation_state();
            last_affiliation_save_ = current_time;
          }
        }

        // Periodic cleanup of expired sessions (every ~1 minute)
        static time_t last_session_cleanup = time(NULL);
        time_t session_check_time = time(NULL);
        if (session_check_time - last_session_cleanup >= 60) {
          cleanup_expired_sessions();
          last_session_cleanup = session_check_time;
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

    int stop() override
    {
        if (!running_)
        {
            return 0; // Already stopped, don't touch server or threads
        }
        running_ = false;

        // Stop the server first
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

        // Save affiliation state on shutdown
        save_affiliation_state();

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Web server stopped";
        return 0;
    }

    int setup_systems(std::vector<System *> systems) override
    {
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
                        enqueue_sse_event("call_end", payload.dump(-1));
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
            add_call_rate_point(sys_name, count);
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
        // Best-effort discrete call_start (cheap), queued for broadcast thread.
        json call_json = get_call_json(call);
        json payload = {{"type", "call_start"}, {"call", call_json}};
        enqueue_sse_event("call_start", payload.dump(-1));

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
        enqueue_sse_event("unit_event", grant_payload.dump(-1));

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
        enqueue_sse_event("unit_event", payload.dump(-1));

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
        enqueue_sse_event("unit_event", payload.dump(-1));

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
        enqueue_sse_event("unit_event", payload.dump(-1));

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
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump(-1));

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
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump(-1));

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
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump(-1));

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
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump(-1));

        // Update affiliation tracking for data event
        update_affiliation_state_data(sys, source_id, talkgroup_num);
        send_gephi_unit_tg_event(sys, source_id, talkgroup_num, false);

        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }

    int call_end(Call_Data_t call_info) override
    {
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
        enqueue_sse_event("call_end", payload.dump(-1));
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
                add_rate_point(get_unique_sys_name(sys), decode_rate);
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
    void setup_log_capture()
    {
        // Setup custom logging sink to capture console output
        typedef logging::sinks::synchronous_sink<WebLogBackend> web_sink_t;

        boost::shared_ptr<web_sink_t> web_sink =
            boost::make_shared<web_sink_t>(boost::make_shared<WebLogBackend>(*this));
        logging::core::get()->add_sink(web_sink);
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
                        res.status = 302;
                        res.set_header("Location", "/favicon.svg"); });

        // SSE endpoint for live updates
        server_.SSE("/events");

        // Dedicated endpoint for Gephi graph streaming (raw JSON, not SSE)
        server_.RawStream("/graph-stream");

        // Notify when Gephi clients connect so we can send current state
        server_.set_raw_stream_connect_notify([this]()
                                              { this->request_gephi_initial_dump(); });

        // NOTE: We do NOT use set_sse_auth_callback() because httplib adds WWW-Authenticate
        // headers that trigger browser auth dialogs. Instead, SSE connects and auth is
        // checked via session cookies. If the session expires, frontend will detect
        // 401 responses on API calls and show the login modal.
        // For Gephi/external tools, they can still use HTTP Basic Auth on /graph-stream

        // Login endpoint - validates credentials and returns session token
        server_.Post("/api/login", [this](const httplib::Request &req, httplib::Response &res)
                     {
      try {
        json request_data = json::parse(req.body);
        std::string username = request_data.value("username", "");
        std::string password = request_data.value("password", "");
        
        // Get client IP for logging
        std::string client_ip = req.get_header("X-Forwarded-For");
        if (client_ip.empty()) {
          client_ip = req.get_header("X-Real-IP");
        }
        if (client_ip.empty()) {
          client_ip = "unknown";
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

        if (constant_time_compare(provided_creds, expected_admin_creds_)) {
          auth_success = true;
          is_admin = true;
        } else if (constant_time_compare(provided_creds, expected_user_creds_)) {
          auth_success = true;
          is_admin = false;
        }

        if (!auth_success) {
          // Track failed login attempt
          server_.track_login_attempt(username, client_ip, false, "failed");
          res.status = 401;
          res.set_content("{\"error\": \"Invalid credentials\"}", "application/json");
          return;
        }

        // Track successful login attempt
        server_.track_login_attempt(username, client_ip, true, is_admin ? "admin" : "user");
        
        // Create session
        std::string token = create_session(username, is_admin);
        
        // Set cookie so EventSource (SSE) can authenticate automatically
        res.set_header("Set-Cookie", "session=" + token + "; Path=/; HttpOnly; SameSite=Strict");
        
        json response = {
            {"token", token},
            {"auth_level", is_admin ? "admin" : "user"}};
        res.set_content(response.dump(-1), "application/json");

      } catch (const std::exception &e) {
        res.status = 400;
        json error = {{"error", std::string("Invalid request: ") + e.what()}};
        res.set_content(error.dump(-1), "application/json");
      } });

        // Logout endpoint - deletes session token
        server_.Post("/api/logout", [this](const httplib::Request &req, httplib::Response &res)
                     {
      // Extract token from Authorization header or cookie
      std::string token;
      std::string auth_header = req.get_header("Authorization");
      if (!auth_header.empty() && auth_header.find("Bearer ") == 0) {
        token = auth_header.substr(7);
      } else {
        std::string cookie_header = req.get_header("Cookie");
        if (!cookie_header.empty()) {
          size_t pos = cookie_header.find("session=");
          if (pos != std::string::npos) {
            size_t start = pos + 8;
            size_t end = cookie_header.find(";", start);
            token = cookie_header.substr(start, end == std::string::npos ? std::string::npos : end - start);
          }
        }
      }

      if (!token.empty()) {
        delete_session(token);
      }

      // Clear the session cookie
      res.set_header("Set-Cookie", "session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");

      json response = {{"message", "Logged out successfully"}};
      res.set_content(response.dump(-1), "application/json"); });

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
      // COMMENTED OUT: unitAffiliations not currently displayed in UI
      // response["unitAffiliations"] = get_unit_affiliations();
      response["consoleLogs"] = get_console_logs();
      response["timestamp"] = time(NULL);
      response["sse_clients"] = server_.sse_client_count();

      // Clear pending console queue after initial inload
      {
        std::lock_guard<std::mutex> lock(console_pending_mutex_);
        console_pending_.clear();
        console_pending_dropped_ = 0;
      }

      res.set_content(response.dump(-1), "application/json"); });

        // Rate history endpoint
        server_.Get("/api/rates/history", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      json response = get_rate_history();
      res.set_content(response.dump(-1), "application/json"); });

        // Call rate history endpoint
        server_.Get("/api/calls/rate-history", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      json response = get_call_rate_history();
      res.set_content(response.dump(-1), "application/json"); });

        // Console logs endpoint
        server_.Get("/api/console", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      json response = {{"lines", get_console_logs()}};
      res.set_content(response.dump(-1), "application/json"); });

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
      
      auto view_it = req.params.find("view");
      if (view_it != req.params.end()) {
          if (view_it->second == "units") units_only = true;
          else if (view_it->second == "talkgroups") talkgroups_only = true;
      }

      json response = get_affiliation_data(limit, units_only, talkgroups_only);
      res.set_content(response.dump(-1), "application/json"); });

        // System data endpoints - parse sys_num from path
        server_.Get("/api/system/talkgroups", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      // Parse sys_num from query parameter
      auto it = req.params.find("sys_num");
      if (it == req.params.end()) {
        res.status = 400;
        res.set_content("{\"error\": \"missing sys_num parameter\"}", "application/json");
        return;
      }
      int sys_num = std::stoi(it->second);

      System *sys = nullptr;
      for (auto *s : tr_systems_) {
        if (s->get_sys_num() == sys_num) {
          sys = s;
          break;
        }
      }
      if (!sys) {
        res.status = 404;
        res.set_content("{\"error\": \"system not found\"}", "application/json");
        return;
      }

      json tgs = json::array();
      for (auto *tg : sys->get_talkgroups()) {
        tgs.push_back({{"number", tg->number},
                       {"alpha_tag", tg->alpha_tag},
                       {"description", tg->description},
                       {"tag", tg->tag},
                       {"group", tg->group},
                       {"priority", tg->priority}});
      }
      res.set_content(tgs.dump(-1), "application/json"); });

        server_.Get("/api/system/unit_tags", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      auto it = req.params.find("sys_num");
      if (it == req.params.end()) {
        res.status = 400;
        res.set_content("{\"error\": \"missing sys_num parameter\"}", "application/json");
        return;
      }
      int sys_num = std::stoi(it->second);

      System *sys = nullptr;
      for (auto *s : tr_systems_) {
        if (s->get_sys_num() == sys_num) {
          sys = s;
          break;
        }
      }
      if (!sys) {
        res.status = 404;
        res.set_content("{\"error\": \"system not found\"}", "application/json");
        return;
      }

      json tags = json::array();
      for (auto *tag : sys->get_unit_tags()) {
        std::string pattern_str = tag->pattern.str();
        json tag_obj;
        tag_obj["pattern"] = pattern_str;
        tag_obj["tag"] = tag->tag;
        tags.push_back(tag_obj);
      }

      json response = {
          {"file", sys->get_unit_tags_file()},
          {"mode", sys->get_unit_tags_mode()},
          {"count", tags.size()},
          {"tags", tags}};
      res.set_content(response.dump(-1), "application/json"); });

        server_.Get("/api/system/unit_tags_ota", [this](const httplib::Request &req, httplib::Response &res)
                    {
      if (!require_auth(req, res)) return;
      
      auto it = req.params.find("sys_num");
      if (it == req.params.end()) {
        res.status = 400;
        res.set_content("{\"error\": \"missing sys_num parameter\"}", "application/json");
        return;
      }
      int sys_num = std::stoi(it->second);

      System *sys = nullptr;
      for (auto *s : tr_systems_) {
        if (s->get_sys_num() == sys_num) {
          sys = s;
          break;
        }
      }
      if (!sys) {
        res.status = 404;
        res.set_content("{\"error\": \"system not found\"}", "application/json");
        return;
      }

      json aliases = json::array();
      for (auto *ota : sys->get_unit_tags_ota()) {
        json ota_obj;
        ota_obj["unit"] = ota->unit_id;
        ota_obj["alias"] = ota->alias;
        aliases.push_back(ota_obj);
      }

      json response = {
          {"file", sys->get_unit_tags_ota_file()},
          {"count", aliases.size()},
          {"aliases", aliases}};
      res.set_content(response.dump(-1), "application/json"); });

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

      res.set_content(response.dump(-1), "application/json"); });

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
          res.set_content(error.dump(-1), "application/json");
          return;
        }

        std::string config_content((std::istreambuf_iterator<char>(config_file)),
                                   std::istreambuf_iterator<char>());
        json response = {
            {"content", config_content},
            {"path", config_path}};
        res.set_content(response.dump(-1), "application/json");
      } catch (const std::exception &e) {
        res.status = 500;
        json error = {{"error", std::string("Failed to read config: ") + e.what()}};
        res.set_content(error.dump(-1), "application/json");
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
          res.set_content(error.dump(-1), "application/json");
          return;
        }

        std::string new_content = request_data.value("content", "");
        std::string config_path = request_data.value("path", tr_config_->config_file);

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
          res.set_content(error.dump(-1), "application/json");
          return;
        }

        // Create backup with .bak.trweb suffix
        std::string backup_path = config_path + ".bak.trweb";

        // Copy current config to backup
        std::ifstream src(config_path, std::ios::binary);
        if (src.good()) {
          std::ofstream dst(backup_path, std::ios::binary);
          dst << src.rdbuf();
          if (!dst.good()) {
            res.status = 500;
            res.set_content("{\"error\": \"Failed to create backup\"}", "application/json");
            return;
          }
        }

        // Atomic save: write to temp file, then rename
        std::string temp_path = config_path + ".tmp.trweb";
        {
          std::ofstream temp_file(temp_path);
          if (!temp_file.good()) {
            res.status = 500;
            res.set_content("{\"error\": \"Failed to create temporary file\"}", "application/json");
            return;
          }
          temp_file << new_content;
          temp_file.flush();
          if (!temp_file.good()) {
            res.status = 500;
            res.set_content("{\"error\": \"Failed to write configuration\"}", "application/json");
            return;
          }
        }

        // Atomic rename
        if (std::rename(temp_path.c_str(), config_path.c_str()) != 0) {
          res.status = 500;
          res.set_content("{\"error\": \"Failed to save configuration\"}", "application/json");
          std::remove(temp_path.c_str()); // Clean up temp file
          return;
        }

        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Configuration saved (backup: " << backup_path << ")";

        json response = {
            {"success", true},
            {"backup", backup_path},
            {"message", "Configuration saved successfully"}};
        res.set_content(response.dump(-1), "application/json");

      } catch (const json::exception &e) {
        res.status = 400;
        json error = {{"error", std::string("Invalid request: ") + e.what()}};
        res.set_content(error.dump(-1), "application/json");
      } catch (const std::exception &e) {
        res.status = 500;
        json error = {{"error", std::string("Failed to save config: ") + e.what()}};
        res.set_content(error.dump(-1), "application/json");
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
      res.set_content(response.dump(-1), "application/json");

      // Schedule restart in a separate thread to allow response to complete
      std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Executing restart...";

        // Send SIGHUP to self to trigger graceful restart
        kill(getpid(), SIGHUP);
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
      std::string token;
      std::string auth_header = req.get_header("Authorization");
      if (!auth_header.empty() && auth_header.find("Bearer ") == 0) {
        token = auth_header.substr(7);
      } else {
        std::string cookie_header = req.get_header("Cookie");
        if (!cookie_header.empty()) {
          size_t pos = cookie_header.find("session=");
          if (pos != std::string::npos) {
            size_t start = pos + 8;
            size_t end = cookie_header.find(";", start);
            token = cookie_header.substr(start, end == std::string::npos ? std::string::npos : end - start);
          }
        }
      }

      bool is_admin = false;
      if (!token.empty() && get_session_info(token, username, is_admin)) {
        // Got session info
        auth_level = is_admin ? "admin" : "user";
      } else {
        // Fall back to Basic Auth parsing
        auth_header = req.get_header("Authorization");
        if (!auth_header.empty() && auth_header.find("Basic ") == 0) {
          std::string provided_creds = auth_header.substr(6);
          if (!expected_admin_creds_.empty() && constant_time_compare(provided_creds, expected_admin_creds_)) {
            auth_level = "admin";
            username = admin_username_.empty() ? "admin" : admin_username_;
          } else if (!expected_user_creds_.empty() && constant_time_compare(provided_creds, expected_user_creds_)) {
            auth_level = "user";
            username = username_.empty() ? "user" : username_;
          }
        }
      }

      json response = {
          {"auth_level", auth_level},
          {"username", username},
          {"timestamp", time(NULL)}};
      res.set_content(response.dump(-1), "application/json"); });

        // Health check
        server_.Get("/health", [this](const httplib::Request &req, httplib::Response &res)
                    {
      json health = {
          {"status", "ok"},
          {"timestamp", time(NULL)},
          {"https", server_.is_https()}};
      res.set_content(health.dump(-1), "application/json"); });
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

        const json *cfg_sources = nullptr;
        if (tr_config_json_.is_object() && tr_config_json_.contains("sources") && tr_config_json_["sources"].is_array())
        {
            cfg_sources = &tr_config_json_["sources"];
        }

        for (auto *source : tr_sources_)
        {
            const int src_num = source->get_num();
            const json *cfg = nullptr;
            if (cfg_sources && src_num >= 0 && static_cast<size_t>(src_num) < cfg_sources->size())
            {
                const json &maybe = (*cfg_sources)[static_cast<size_t>(src_num)];
                if (maybe.is_object())
                    cfg = &maybe;
            }

            auto cfg_str = [&](const char *key, const std::string &fallback) -> std::string
            {
                try
                {
                    if (cfg && cfg->contains(key) && (*cfg)[key].is_string())
                        return (*cfg)[key].get<std::string>();
                }
                catch (...)
                {
                }
                return fallback;
            };

            auto cfg_dbl = [&](const char *key, double fallback) -> double
            {
                try
                {
                    if (cfg && cfg->contains(key) && (*cfg)[key].is_number())
                        return (*cfg)[key].get<double>();
                }
                catch (...)
                {
                }
                return fallback;
            };

            json gain_stages = json::array();
            for (const auto &stage : source->get_gain_stages())
            {
                if (stage.value == 0)
                    continue;
                gain_stages.push_back({{"name", stage.stage_name},
                                       {"value", stage.value}});
            }

            devices_json.push_back({{"src_num", src_num},
                                    {"driver", cfg_str("driver", source->get_driver())},
                                    {"device", cfg_str("device", source->get_device())},
                                    {"center", cfg_dbl("center", source->get_center())},
                                    {"rate", cfg_dbl("rate", source->get_rate())},
                                    {"error", cfg_dbl("error", source->get_error())},
                                    {"gain", cfg_dbl("gain", source->get_gain())},
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
            {"rec_state_type", tr_state_[stat_node.get<int>("state")]},
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
            {"call_state_type", tr_state_[stat_node.get<int>("state")]},
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
            {"rec_state_type", tr_state_[stat_node.get<int>("recState", -1)]}};

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
