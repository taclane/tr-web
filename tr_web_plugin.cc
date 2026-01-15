// tr-web: Web Status Plugin for Trunk-Recorder

#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <deque>
#include <iostream>
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <regex>
#include <iomanip>
#include <fstream>

// Trunk-Recorder headers
#include "../../trunk-recorder/source.h"
#include "../../trunk-recorder/systems/system_impl.h"
#include "../../lib/json.hpp"
#include "../../trunk-recorder/plugin_manager/plugin_api.h"

// System/library headers
#include <boost/dll/alias.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

// Plugin headers
#include "httplib.h"
#include "web_assets.h" 

using namespace std;
using json = nlohmann::json;
namespace logging = boost::log;

struct RatePoint {
    time_t timestamp;
    double rate;
};

class Tr_Web : public Plugin_Api {
    // HTTP Server
    httplib::Server server_;
    std::thread server_thread_;
    std::thread broadcast_thread_;
    std::atomic<bool> running_;
    
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
    
    // Trunk-Recorder references
    Config *tr_config_;
    std::vector<Source *> tr_sources_;
    std::vector<System *> tr_systems_;
    std::vector<Call *> tr_calls_;
    
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
    static const size_t MAX_RATE_HISTORY = 1200;  // 60 min at 3 sec intervals
    
    // Call rate history per system (keeps 60 minutes of data)
    std::map<std::string, std::deque<RatePoint>> call_rate_history_;
    
    // Recent call history cache (last N completed calls)
    mutable std::mutex call_history_mutex_;
    std::deque<json> call_history_;
    static const size_t MAX_CALL_HISTORY = 100;
    
    // Track previous calls to detect disappearances (encrypted calls)
    mutable std::mutex previous_calls_mutex_;
    std::map<long, json> previous_calls_map_;  // call_num -> call_json
    
    // Trunking message buffer (for Omnitrunker tab)
    mutable std::mutex trunk_messages_mutex_;
    std::deque<json> trunk_messages_;
    static const size_t MAX_TRUNK_MESSAGES = 500;
    
    // Unit affiliation tracking (unit_id -> talkgroup)
    mutable std::mutex unit_affiliations_mutex_;
    std::map<long, long> unit_affiliations_;
    
    // Console log buffer
    mutable std::mutex console_mutex_;
    std::deque<std::string> console_logs_;

    // Pending console lines for SSE (bounded, flushed from broadcast thread)
    mutable std::mutex console_pending_mutex_;
    std::deque<std::string> console_pending_;
    size_t console_pending_dropped_ = 0;

    // Discrete SSE events that should be delivered even if the periodic snapshots miss them
    // (e.g., call_end with rich trunk-recorder call_json)
    mutable std::mutex event_queue_mutex_;
    std::deque<std::pair<std::string, std::string>> event_queue_;
    size_t event_queue_dropped_ = 0;

    // Dirty flags for SSE broadcasts (avoid doing work in trunk-recorder threads)
    std::atomic<uint32_t> dirty_flags_{0};

    enum DirtyBits : uint32_t {
        DIRTY_SYSTEMS   = 1u << 0,
        DIRTY_RECORDERS = 1u << 1,
        DIRTY_CALLS     = 1u << 2,
        DIRTY_RATES     = 1u << 3,
        DIRTY_TRUNK_MESSAGES = 1u << 4,
        DIRTY_DEVICES   = 1u << 4
    };

    void enqueue_sse_event(const std::string& event, std::string data) {
        std::lock_guard<std::mutex> lock(event_queue_mutex_);
        static constexpr size_t MAX_EVENTS = 2000;
        if (event_queue_.size() >= MAX_EVENTS) {
            ++event_queue_dropped_;
            return;
        }
        event_queue_.emplace_back(event, std::move(data));
    }
    
    // State maps (same as mqtt_status)
    std::map<short, std::string> tr_state_ = {
        {0, "MONITORING"},
        {1, "RECORDING"},
        {2, "INACTIVE"},
        {3, "ACTIVE"},
        {4, "IDLE"},
        {6, "STOPPED"},
        {7, "AVAILABLE"},
        {8, "IGNORE"}
    };
    
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
        {99, "UNKNOWN"}
    };

    // Custom logging backend to capture console output
    class WebLogBackend : public logging::sinks::text_ostream_backend {
    public:
        explicit WebLogBackend(Tr_Web& parent) : parent_(parent) {}

        static std::string severity_to_string(boost::log::trivial::severity_level sev) {
            switch (sev) {
                case boost::log::trivial::trace: return "trace";
                case boost::log::trivial::debug: return "debug";
                case boost::log::trivial::info: return "info";
                case boost::log::trivial::warning: return "warning";
                case boost::log::trivial::error: return "error";
                case boost::log::trivial::fatal: return "fatal";
                default: return "info";
            }
        }
        
        void consume(logging::record_view const& rec, std::string const& formatted_message) {
            // Prefer the raw Message attribute (keeps any embedded ANSI/tabs).
            // Fall back to formatted_message if Message is unavailable.
            std::string message;
            if (auto msg = rec["Message"].extract<std::string>()) {
                message = msg.get();
            } else {
                message = formatted_message;
            }

            auto sev_attr = rec[boost::log::trivial::severity];
            auto sev = sev_attr ? sev_attr.get() : boost::log::trivial::info;
            parent_.add_console_line("[" + severity_to_string(sev) + "] " + message);
        }
        
    private:
        Tr_Web& parent_;
    };

public:
    Tr_Web() : running_(false) {}
    
    ~Tr_Web() {
        stop();
    }
    
    void add_console_line(const std::string& line) {
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
        if (timestamped_line.size() > MAX_CONSOLE_LINE_BYTES) {
            timestamped_line.resize(MAX_CONSOLE_LINE_BYTES);
            timestamped_line += "…(truncated)";
        }
        
        {
            std::lock_guard<std::mutex> lock(console_mutex_);
            console_logs_.push_back(timestamped_line);
            while (console_logs_.size() > console_max_lines_) {
                console_logs_.pop_front();
            }
        }

        // Queue for SSE broadcast (do not write sockets from trunk-recorder threads)
        {
            std::lock_guard<std::mutex> lock(console_pending_mutex_);
            static constexpr size_t MAX_PENDING = 2000;
            if (console_pending_.size() >= MAX_PENDING) {
                ++console_pending_dropped_;
            } else {
                console_pending_.push_back(timestamped_line);
            }
        }
    }
    
    json get_console_logs() const {
        std::lock_guard<std::mutex> lock(console_mutex_);
        json logs = json::array();
        for (const auto& line : console_logs_) {
            logs.push_back(line);
        }
        return logs;
    }
    
    void cache_call(const json& call_json) {
        std::lock_guard<std::mutex> lock(call_history_mutex_);
        call_history_.push_back(call_json);
        while (call_history_.size() > MAX_CALL_HISTORY) {
            call_history_.pop_front();
        }
    }
    
    json get_call_history() const {
        std::lock_guard<std::mutex> lock(call_history_mutex_);
        json history = json::array();
        for (const auto& call : call_history_) {
            history.push_back(call);
        }
        return history;
    }
    
    void cache_trunk_message(const json& msg_json) {
        std::lock_guard<std::mutex> lock(trunk_messages_mutex_);
        trunk_messages_.push_back(msg_json);
        while (trunk_messages_.size() > MAX_TRUNK_MESSAGES) {
            trunk_messages_.pop_front();
        }
    }
    
    json get_trunk_messages() const {
        std::lock_guard<std::mutex> lock(trunk_messages_mutex_);
        json messages = json::array();
        for (const auto& msg : trunk_messages_) {
            messages.push_back(msg);
        }
        return messages;
    }
    
    json get_unit_affiliations() const {
        std::lock_guard<std::mutex> lock(unit_affiliations_mutex_);
        json affiliations = json::object();
        for (const auto& pair : unit_affiliations_) {
            affiliations[std::to_string(pair.first)] = pair.second;
        }
        return affiliations;
    }
    
    // Generate display name for system with number prefix
    std::string get_unique_sys_name(System *sys) {
        int sys_num = sys->get_sys_num();
        std::string short_name = sys->get_short_name();
        return std::to_string(sys_num + 1) + ". " + short_name;
    }
    
    void add_rate_point(const std::string& sys_name, double rate) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        
        RatePoint point;
        point.timestamp = time(NULL);
        point.rate = rate;
        
        auto& history = rate_history_[sys_name];
        history.push_back(point);
        
        // Trim to max size (60 minutes of data)
        while (history.size() > MAX_RATE_HISTORY) {
            history.pop_front();
        }
    }
    
    void add_call_rate_point(const std::string& sys_name, int count) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        
        RatePoint point;
        point.timestamp = time(NULL);
        point.rate = static_cast<double>(count);
        
        auto& history = call_rate_history_[sys_name];
        history.push_back(point);
        
        // Trim to max size (60 minutes of data)
        while (history.size() > MAX_RATE_HISTORY) {
            history.pop_front();
        }
    }
    
    json get_rate_history() const {
        std::lock_guard<std::mutex> lock(data_mutex_);
        json history;
        
        for (const auto& [sys_name, points] : rate_history_) {
            json sys_history = json::array();
            for (const auto& point : points) {
                sys_history.push_back({
                    {"time", point.timestamp * 1000},  // JavaScript timestamp (ms)
                    {"rate", point.rate}
                });
            }
            history[sys_name] = sys_history;
        }
        
        return history;
    }
    
    json get_call_rate_history() const {
        std::lock_guard<std::mutex> lock(data_mutex_);
        json history;
        
        for (const auto& [sys_name, points] : call_rate_history_) {
            json sys_history = json::array();
            for (const auto& point : points) {
                sys_history.push_back({
                    {"time", point.timestamp * 1000},  // JavaScript timestamp (ms)
                    {"count", static_cast<int>(point.rate)}
                });
            }
            history[sys_name] = sys_history;
        }
        
        return history;
    }
    
    int parse_config(json config_data) override {
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
        
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Port:          " << port_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Bind:          " << bind_address_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Auth:          " << (username_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Admin Auth:    " << (admin_username_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "HTTPS:         " << (ssl_cert_.empty() ? "[disabled]" : "[enabled]");
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Console Lines: " << console_max_lines_;
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Theme:         " << theme_;
        
        return 0;
    }
    
    int init(Config *config, std::vector<Source *> sources, std::vector<System *> systems) override {
        tr_config_ = config;
        tr_sources_ = sources;
        tr_systems_ = systems;
        return 0;
    }
    
    int start() override {
        log_prefix_ = "[tr-web]\t";

        // Best-effort read of trunk-recorder config.json (static device metadata)
        try {
            std::ifstream in("./config.json");
            if (in.good()) {
                in >> tr_config_json_;
            }
        } catch (...) {
            // Ignore parse errors; we'll fall back to Source getters.
            tr_config_json_ = json();
        }
        
        // Setup authentication
        if (!username_.empty() && !password_.empty()) {
            server_.set_auth(username_, password_);
        }
        
        // Setup admin authentication
        if (!admin_username_.empty() && !admin_password_.empty()) {
            server_.set_admin_auth(admin_username_, admin_password_);
        }
        
        // Setup HTTPS if configured
        if (!ssl_cert_.empty() && !ssl_key_.empty()) {
            if (!server_.set_https(ssl_cert_, ssl_key_)) {
                BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to load SSL certificates!";
                BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Falling back to HTTP";
            }
        }
        
        // Setup routes
        setup_routes();
        
        // Setup console log capture
        setup_log_capture();

        // Prime initial caches for first page load
        resend_recorders();
        resend_devices();
        setup_systems(tr_systems_);
        
        // Start server in background
        running_ = true;

        // Broadcast thread (flushes SSE without blocking trunk-recorder)
        broadcast_thread_ = std::thread([this]() {
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
                        for (const auto& l : lines) payload["lines"].push_back(l);
                        payload["dropped"] = dropped;
                        server_.broadcast_sse("console_batch", payload.dump());
                    }
                }

                // Flush dirty state at ~4Hz.
                if (has_clients) {
                    const uint32_t flags = dirty_flags_.exchange(0);
                    if (flags) {
                        json systems, recorders, calls, rates, devices;
                        {
                            std::lock_guard<std::mutex> lock(data_mutex_);
                            if (flags & DIRTY_SYSTEMS) systems = cached_systems_;
                            if (flags & DIRTY_RECORDERS) recorders = cached_recorders_;
                            if (flags & DIRTY_CALLS) calls = cached_calls_;
                            if (flags & DIRTY_RATES) rates = cached_rates_;
                            if (flags & DIRTY_DEVICES) devices = cached_devices_;
                        }

                        if (flags & DIRTY_SYSTEMS) {
                            json payload = {{"type", "systems"}, {"systems", systems}};
                            server_.broadcast_sse("systems", payload.dump());
                        }
                        if (flags & DIRTY_RECORDERS) {
                            json payload = {{"type", "recorders"}, {"recorders", recorders}};
                            server_.broadcast_sse("recorders", payload.dump());
                        }
                        if (flags & DIRTY_CALLS) {
                            json payload = {{"type", "calls"}, {"calls_active", calls}};
                            server_.broadcast_sse("calls", payload.dump());
                        }
                        if (flags & DIRTY_RATES) {
                            json payload = {{"type", "rates"}, {"rates", rates}};
                            server_.broadcast_sse("rates", payload.dump());
                        }
                        if (flags & DIRTY_DEVICES) {
                            json payload = {{"type", "devices"}, {"devices", devices}};
                            server_.broadcast_sse("devices", payload.dump());
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

                    for (auto& ev : events) {
                        server_.broadcast_sse(ev.first, ev.second);
                    }
                    if (dropped) {
                        json payload = {{"type", "event_drop"}, {"dropped", dropped}};
                        server_.broadcast_sse("event_drop", payload.dump());
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

        server_thread_ = std::thread([this]() {
            std::string protocol = server_.is_https() ? "https" : "http";
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Starting web server on " 
                                    << protocol << "://" << bind_address_ << ":" << port_;
            if (!server_.listen(bind_address_, port_)) {
                BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to start web server!";
                running_ = false;
            }
        });
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (running_) {
            std::string protocol = server_.is_https() ? "https" : "http";
            BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Web server running at " 
                                    << protocol << "://" << bind_address_ << ":" << port_;
        }
        
        return 0;
    }
    
    int stop() override {
        if (!running_) {
            return 0;  // Already stopped, don't touch server or threads
        }
        running_ = false;
        
        // Stop the server first
        server_.stop();
        
        // Join threads
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        if (broadcast_thread_.joinable()) {
            broadcast_thread_.join();
        }
        
        BOOST_LOG_TRIVIAL(info) << log_prefix_ << "Web server stopped";
        return 0;
    }
    
    int setup_systems(std::vector<System *> systems) override {
        json systems_json = json::array();
        for (auto* sys : systems) {
            systems_json.push_back(get_system_json(sys));
        }
        
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_systems_ = systems_json;
        }

        dirty_flags_.fetch_or(DIRTY_SYSTEMS);
        
        return 0;
    }
    
    int setup_system(System *system) override {
        return setup_systems(tr_systems_);
    }
    
    int setup_recorder(Recorder *recorder) override {
        // Avoid per-recorder broadcast; clients will receive periodic recorders snapshot.
        (void)recorder;
        resend_recorders();
        
        return 0;
    }
    
    int setup_config(std::vector<Source *> sources, std::vector<System *> systems) override {
        // Refresh recorders
        resend_recorders();
        resend_devices();
        return 0;
    }
    
    int calls_active(std::vector<Call *> calls) override {
        tr_calls_ = calls;
        
        // Build current calls map for tracking
        std::map<long, json> current_calls_map;
        
        // Count calls per system for rate tracking
        std::map<std::string, int> calls_by_system;
        
        json calls_json = json::array();
        for (auto* call : calls) {
            if (call->get_current_length() > 0 || !call->is_conventional()) {
                json call_json = get_call_json(call);
                long call_num = call->get_call_num();
                
                current_calls_map[call_num] = call_json;
                
                System* sys = call->get_system();
                if (sys) {
                    std::string unique_name = get_unique_sys_name(sys);
                    calls_by_system[unique_name]++;
                }
                
                calls_json.push_back(call_json);
            }
        }
        
        // Detect disappeared calls (synthetic call_end for encrypted calls)
        {
            std::lock_guard<std::mutex> lock(previous_calls_mutex_);
            for (const auto& prev_pair : previous_calls_map_) {
                long prev_call_num = prev_pair.first;
                const json& prev_call_json = prev_pair.second;
                
                // If call was in previous snapshot but not current, it disappeared
                if (current_calls_map.find(prev_call_num) == current_calls_map.end()) {
                    bool was_encrypted = prev_call_json.value("encrypted", false);
                    
                    if (was_encrypted) {
                        // Cache the encrypted call that disappeared
                        cache_call(prev_call_json);
                        
                        // Send synthetic call_end event to frontend
                        json payload = {{"type", "call_end"}, {"call", prev_call_json}};
                        enqueue_sse_event("call_end", payload.dump());
                    }
                }
            }
            
            // Update previous calls map for next iteration
            previous_calls_map_ = current_calls_map;
        }
        
        // Add call rate data points (including zero for systems with no active calls)
        for (auto* sys : tr_systems_) {
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
    
    int call_start(Call *call) override {
        // Best-effort discrete call_start (cheap), queued for broadcast thread.
        json call_json = get_call_json(call);
        json payload = {{"type", "call_start"}, {"call", call_json}};
        enqueue_sse_event("call_start", payload.dump());
        
        // Also log as a GRANT event for Omnitrunker
        System *sys = call->get_system();
        long source_id = call->get_current_source_id();
        long talkgroup_num = call->get_talkgroup();
        
        std::string tg_alpha = "";
        Talkgroup *tg = sys->find_talkgroup(talkgroup_num);
        if (tg) {
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
            {"tg_alpha", tg_alpha}
        };
        
        cache_trunk_message(event_json);
        
        json grant_payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", grant_payload.dump());
        
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int unit_group_affiliation(System *sys, long source_id, long talkgroup_num) override {
        {
            std::lock_guard<std::mutex> lock(unit_affiliations_mutex_);
            unit_affiliations_[source_id] = talkgroup_num;
        }
        
        // Look up talkgroup alpha tag
        std::string tg_alpha = "";
        Talkgroup *tg = sys->find_talkgroup(talkgroup_num);
        if (tg) {
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
            {"tg_alpha", tg_alpha}
        };
        
        cache_trunk_message(event_json);
        
        json payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", payload.dump());
        
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int unit_registration(System *sys, long source_id) override {
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
            {"tg_alpha", ""}
        };
        
        cache_trunk_message(event_json);
        
        json payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", payload.dump());
        
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int unit_deregistration(System *sys, long source_id) override {
        {
            std::lock_guard<std::mutex> lock(unit_affiliations_mutex_);
            unit_affiliations_.erase(source_id);
        }
        
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
            {"tg_alpha", ""}
        };
        
        cache_trunk_message(event_json);
        
        json payload = {{"type", "unit_event"}, {"event", event_json}};
        enqueue_sse_event("unit_event", payload.dump());
        
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int unit_acknowledge_response(System *sys, long source_id) override {
        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", sys->find_unit_tag(source_id)},
            {"msg_type", "ACKNOWLEDGE"},
            {"talkgroup", nullptr},
            {"tg_alpha", ""}
        };
        
        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump());
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int unit_data_grant(System *sys, long source_id) override {
        json event_json = {
            {"timestamp", time(NULL)},
            {"sys_name", sys->get_short_name()},
            {"unique_sys_name", get_unique_sys_name(sys)},
            {"site_id", sys->get_sys_site_id()},
            {"unit", source_id},
            {"unit_alias", sys->find_unit_tag(source_id)},
            {"msg_type", "DATA_GRANT"},
            {"talkgroup", nullptr},
            {"tg_alpha", ""}
        };
        
        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump());
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int unit_answer_request(System *sys, long source_id, long talkgroup_num) override {
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
            {"tg_alpha", tg ? tg->alpha_tag : ""}
        };
        
        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump());
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int unit_location(System *sys, long source_id, long talkgroup_num) override {
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
            {"tg_alpha", tg ? tg->alpha_tag : ""}
        };
        
        cache_trunk_message(event_json);
        enqueue_sse_event("unit_event", json{{"type", "unit_event"}, {"event", event_json}}.dump());
        dirty_flags_.fetch_or(DIRTY_TRUNK_MESSAGES);
        return 0;
    }
    
    int call_end(Call_Data_t call_info) override {
        // Prefer the full call JSON produced by trunk-recorder (includes srcList/freqList/tags).
        // Fall back to a minimal summary if it is not populated for some reason.
        json call_json;
        if (!call_info.call_json.is_null() && !call_info.call_json.empty()) {
            call_json = call_info.call_json;
        } else {
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
                {"short_name", call_info.short_name}
            };
        }

        // Add fields not included in trunk-recorder's call JSON
        call_json["call_num"] = call_info.call_num;
        call_json["sys_num"] = call_info.sys_num;
        
        // Cache for initial page load
        cache_call(call_json);
        
        // Queue the rich end-event for the broadcast thread.
        json payload = {{"type", "call_end"}, {"call", call_json}};
        enqueue_sse_event("call_end", payload.dump());
        return 0;
    }
    
    int system_rates(std::vector<System *> systems, float timeDiff) override {
        json rates_json = json::array();
        
        for (auto* sys : systems) {
            std::string sys_type = sys->get_system_type();
            if (sys_type.find("conventional") == std::string::npos) {
                boost::property_tree::ptree stat_node = sys->get_stats_current(timeDiff);
                double decode_rate = stat_node.get<double>("decoderate");

                double control_channel = 0.0;
                if (sys->control_channel_count() > 0) {
                    control_channel = sys->get_current_control_channel();
                }
                
                rates_json.push_back({
                    {"sys_num", stat_node.get<int>("id")},
                    {"sys_name", get_unique_sys_name(sys)},
                    {"decoderate", decode_rate},
                    {"control_channel", control_channel}
                });
                
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

private:
    void setup_log_capture() {
        // Setup custom logging sink to capture console output
        typedef logging::sinks::synchronous_sink<WebLogBackend> web_sink_t;
        
        boost::shared_ptr<web_sink_t> web_sink = 
            boost::make_shared<web_sink_t>(boost::make_shared<WebLogBackend>(*this));
        logging::core::get()->add_sink(web_sink);
    }
    
    void setup_routes() {
        // Main page - serves embedded HTML
        server_.Get("/", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(tr_web::HTML_PAGE, "text/html; charset=utf-8");
        });
        
        // SSE endpoint for live updates
        server_.SSE("/events");
        
        // REST API endpoint for initial state
        server_.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
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
                {"console_max_lines", console_max_lines_}
            };
            response["rateHistory"] = get_rate_history();
            response["callRateHistory"] = get_call_rate_history();
            response["callHistory"] = get_call_history();
            response["trunkMessages"] = get_trunk_messages();
            response["unitAffiliations"] = get_unit_affiliations();
            response["consoleLogs"] = get_console_logs();
            response["timestamp"] = time(NULL);
            response["sse_clients"] = server_.sse_client_count();
            
            // Clear pending console queue after initial inload
            {
                std::lock_guard<std::mutex> lock(console_pending_mutex_);
                console_pending_.clear();
                console_pending_dropped_ = 0;
            }
            
            res.set_content(response.dump(), "application/json");
        });
        
        // Rate history endpoint
        server_.Get("/api/rates/history", [this](const httplib::Request& req, httplib::Response& res) {
            json response = get_rate_history();
            res.set_content(response.dump(), "application/json");
        });
        
        // Call rate history endpoint
        server_.Get("/api/calls/rate-history", [this](const httplib::Request& req, httplib::Response& res) {
            json response = get_call_rate_history();
            res.set_content(response.dump(), "application/json");
        });
        
        // Console logs endpoint
        server_.Get("/api/console", [this](const httplib::Request& req, httplib::Response& res) {
            json response = {{"lines", get_console_logs()}};
            res.set_content(response.dump(), "application/json");
        });
        
        // System data endpoints - parse sys_num from path
        server_.Get("/api/system/talkgroups", [this](const httplib::Request& req, httplib::Response& res) {
            // Parse sys_num from query parameter
            auto it = req.params.find("sys_num");
            if (it == req.params.end()) {
                res.status = 400;
                res.set_content("{\"error\": \"missing sys_num parameter\"}", "application/json");
                return;
            }
            int sys_num = std::stoi(it->second);
            
            System* sys = nullptr;
            for (auto* s : tr_systems_) {
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
            for (auto* tg : sys->get_talkgroups()) {
                tgs.push_back({
                    {"number", tg->number},
                    {"alpha_tag", tg->alpha_tag},
                    {"description", tg->description},
                    {"tag", tg->tag},
                    {"group", tg->group},
                    {"priority", tg->priority}
                });
            }
            res.set_content(tgs.dump(), "application/json");
        });

        server_.Get("/api/system/unit_tags", [this](const httplib::Request& req, httplib::Response& res) {
            auto it = req.params.find("sys_num");
            if (it == req.params.end()) {
                res.status = 400;
                res.set_content("{\"error\": \"missing sys_num parameter\"}", "application/json");
                return;
            }
            int sys_num = std::stoi(it->second);
            
            System* sys = nullptr;
            for (auto* s : tr_systems_) {
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
            for (auto* tag : sys->get_unit_tags()) {
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
                {"tags", tags}
            };
            res.set_content(response.dump(), "application/json");
        });

        server_.Get("/api/system/unit_tags_ota", [this](const httplib::Request& req, httplib::Response& res) {
            auto it = req.params.find("sys_num");
            if (it == req.params.end()) {
                res.status = 400;
                res.set_content("{\"error\": \"missing sys_num parameter\"}", "application/json");
                return;
            }
            int sys_num = std::stoi(it->second);
            
            System* sys = nullptr;
            for (auto* s : tr_systems_) {
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
            for (auto* ota : sys->get_unit_tags_ota()) {
                json ota_obj;
                ota_obj["unit"] = ota->unit_id;
                ota_obj["alias"] = ota->alias;
                aliases.push_back(ota_obj);
            }
            
            json response = {
                {"file", sys->get_unit_tags_ota_file()},
                {"count", aliases.size()},
                {"aliases", aliases}
            };
            res.set_content(response.dump(), "application/json");
        });

        // Admin: Get login history
        server_.Get("/api/admin/login-history", [this](const httplib::Request& req, httplib::Response& res) {
            auto history = server_.get_login_history();
            json response = json::array();
            
            for (const auto& attempt : history) {
                json entry = {
                    {"timestamp", attempt.timestamp},
                    {"username", attempt.username},
                    {"client_ip", attempt.client_ip},
                    {"success", attempt.success},
                    {"access_level", attempt.access_level}
                };
                response.push_back(entry);
            }
            
            res.set_content(response.dump(), "application/json");
        });
        
        // Admin: Get trunk-recorder config
        server_.Get("/api/admin/config", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                std::ifstream config_file("./config.json");
                if (!config_file.good()) {
                    res.status = 404;
                    json error = {{"error", "Config file not found: ./config.json"}};
                    res.set_content(error.dump(), "application/json");
                    return;
                }
                
                std::string config_content((std::istreambuf_iterator<char>(config_file)),
                                          std::istreambuf_iterator<char>());
                json response = {
                    {"content", config_content},
                    {"path", "./config.json"}
                };
                res.set_content(response.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                json error = {{"error", std::string("Failed to read config: ") + e.what()}};
                res.set_content(error.dump(), "application/json");
            }
        });
        
        // Admin: Save config (atomic with backup)
        server_.Post("/api/admin/save-config", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                // Log request size for debugging
                BOOST_LOG_TRIVIAL(debug) << log_prefix_ << "Save config request size: " << req.body.size() << " bytes";
                
                json request_data;
                try {
                    request_data = json::parse(req.body);
                } catch (const json::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Failed to parse save-config request: " << e.what();
                    BOOST_LOG_TRIVIAL(error) << log_prefix_ << "Request body length: " << req.body.size();
                    res.status = 400;
                    json error = {{"error", std::string("Invalid request: ") + e.what()}};
                    res.set_content(error.dump(), "application/json");
                    return;
                }
                
                std::string new_content = request_data.value("content", "");
                std::string config_path = request_data.value("path", "./config.json");
                
                if (new_content.empty()) {
                    res.status = 400;
                    res.set_content("{\"error\": \"Empty configuration\"}", "application/json");
                    return;
                }
                
                // Validate JSON on server side
                try {
                    auto parsed = json::parse(new_content);
                    (void)parsed; // Suppress unused warning
                } catch (const std::exception& e) {
                    res.status = 400;
                    json error = {{"error", std::string("Invalid JSON: ") + e.what()}};
                    res.set_content(error.dump(), "application/json");
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
                    {"message", "Configuration saved successfully"}
                };
                res.set_content(response.dump(), "application/json");
                
            } catch (const json::exception& e) {
                res.status = 400;
                json error = {{"error", std::string("Invalid request: ") + e.what()}};
                res.set_content(error.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                json error = {{"error", std::string("Failed to save config: ") + e.what()}};
                res.set_content(error.dump(), "application/json");
            }
        });
        
        // Admin: Restart trunk-recorder
        server_.Post("/api/admin/restart", [this](const httplib::Request& req, httplib::Response& res) {
            BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Restart requested via web admin interface";
            
            json response = {
                {"status", "ok"},
                {"message", "Restart initiated"},
                {"timestamp", time(NULL)}
            };
            res.set_content(response.dump(), "application/json");
            
            // Schedule restart in a separate thread to allow response to complete
            std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                BOOST_LOG_TRIVIAL(warning) << log_prefix_ << "Executing restart...";
                
                // Send SIGHUP to self to trigger graceful restart
                kill(getpid(), SIGHUP);
            }).detach();
        });

        // Whoami - returns current user's auth level without requiring admin
        server_.Get("/api/whoami", [this](const httplib::Request& req, httplib::Response& res) {
            std::string auth_level = "none";
            
            // Check if user has admin credentials
            auto auth_it = req.headers.find("Authorization");
            if (auth_it == req.headers.end()) {
                auth_it = req.headers.find("authorization");
            }
            
            if (auth_it != req.headers.end() && auth_it->second.substr(0, 6) == "Basic ") {
                std::string decoded_creds = auth_it->second.substr(6);
                
                // Compare against stored credentials (already base64 encoded)
                if (!admin_username_.empty() && decoded_creds == httplib::base64_encode(admin_username_ + ":" + admin_password_)) {
                    auth_level = "admin";
                } else if (!username_.empty() && decoded_creds == httplib::base64_encode(username_ + ":" + password_)) {
                    auth_level = "info";
                }
            }
            
            json response = {
                {"auth_level", auth_level},
                {"timestamp", time(NULL)}
            };
            res.set_content(response.dump(), "application/json");
        });

        // Health check
        server_.Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
            json health = {
                {"status", "ok"}, 
                {"timestamp", time(NULL)},
                {"https", server_.is_https()}
            };
            res.set_content(health.dump(), "application/json");
        });
    }
    
    void resend_recorders() {
        json recorders_json = json::array();
        
        for (auto* source : tr_sources_) {
            std::vector<Recorder *> sourceRecorders = source->get_recorders();
            for (auto* recorder : sourceRecorders) {
                recorders_json.push_back(get_recorder_json(recorder));
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_recorders_ = recorders_json;
        }

        dirty_flags_.fetch_or(DIRTY_RECORDERS);
    }

    void resend_devices() {
        json devices_json = json::array();

        const json* cfg_sources = nullptr;
        if (tr_config_json_.is_object() && tr_config_json_.contains("sources") && tr_config_json_["sources"].is_array()) {
            cfg_sources = &tr_config_json_["sources"];
        }

        for (auto* source : tr_sources_) {
            const int src_num = source->get_num();
            const json* cfg = nullptr;
            if (cfg_sources && src_num >= 0 && static_cast<size_t>(src_num) < cfg_sources->size()) {
                const json& maybe = (*cfg_sources)[static_cast<size_t>(src_num)];
                if (maybe.is_object()) cfg = &maybe;
            }

            auto cfg_str = [&](const char* key, const std::string& fallback) -> std::string {
                try {
                    if (cfg && cfg->contains(key) && (*cfg)[key].is_string()) return (*cfg)[key].get<std::string>();
                } catch (...) {
                }
                return fallback;
            };

            auto cfg_dbl = [&](const char* key, double fallback) -> double {
                try {
                    if (cfg && cfg->contains(key) && (*cfg)[key].is_number()) return (*cfg)[key].get<double>();
                } catch (...) {
                }
                return fallback;
            };

            json gain_stages = json::array();
            for (const auto& stage : source->get_gain_stages()) {
                if (stage.value == 0) continue;
                gain_stages.push_back({
                    {"name", stage.stage_name},
                    {"value", stage.value}
                });
            }

            devices_json.push_back({
                {"src_num", src_num},
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
                {"gain_stages", gain_stages}
            });
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            cached_devices_ = devices_json;
        }

        dirty_flags_.fetch_or(DIRTY_DEVICES);
    }
    
    json get_recorder_json(Recorder *recorder) {
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
            {"squelched", recorder->is_squelched()}
        };
    }
    
    json get_call_json(Call *call) {
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
            {"rec_state_type", tr_state_[stat_node.get<int>("recState", -1)]}
        };
        
        if (tg != nullptr) {
            call_json["talkgroup_alpha_tag"] = tg->alpha_tag;
            call_json["talkgroup_description"] = tg->description;
        }
        
        return call_json;
    }
    
    std::string int_to_hex(int num, int places) {
        if (num == 0 && places == 0) return "0";
        std::stringstream stream;
        stream << std::setfill('0') << std::uppercase;
        if (places > 0) stream << std::setw(places);
        stream << std::hex << num;
        return stream.str();
    }
    
    json get_system_json(System *sys) {
        boost::property_tree::ptree stat_node = sys->get_stats();

        double control_channel = 0.0;
        if (sys->control_channel_count() > 0) {
            control_channel = sys->get_current_control_channel();
        }

        json control_channels = json::array();
        try {
            for (double cc : sys->get_control_channels()) {
                control_channels.push_back(cc);
            }
        } catch (...) {
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
            {"unit_tags_ota_file", sys->get_unit_tags_ota_file()}
        };
    }
    
    // Factory method
public:
    static boost::shared_ptr<Tr_Web> create() {
        return boost::shared_ptr<Tr_Web>(new Tr_Web());
    }
};

BOOST_DLL_ALIAS(
    Tr_Web::create,
    create_plugin
)
