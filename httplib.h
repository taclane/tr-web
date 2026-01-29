// httplib.h - Minimal Header-Only HTTP/HTTPS Server for tr-web
// ============================================================

#ifndef TR_WEB_HTTPLIB_H
#define TR_WEB_HTTPLIB_H

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <unordered_set>
#include <deque>

#include <zlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

// OpenSSL for HTTPS support
#include <openssl/ssl.h>
#include <openssl/err.h>

// Boost for logging
#include <boost/log/trivial.hpp>

namespace httplib {

// ============================================================
// Base64 encoding/decoding for Basic Auth
// ============================================================

static const std::string base64_chars = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

inline std::string base64_encode(const std::string &in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline std::string base64_decode(const std::string &in) {
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

// ============================================================
// Socket wrapper for unified HTTP/HTTPS handling
// ============================================================

class SocketWrapper {
public:
    virtual ~SocketWrapper() = default;
    virtual ssize_t read(void* buf, size_t len) = 0;
    virtual ssize_t write(const void* buf, size_t len) = 0;
    virtual void close() = 0;
    virtual int fd() const = 0;
    virtual bool is_valid() const = 0;
};

class PlainSocket : public SocketWrapper {
public:
    explicit PlainSocket(int fd) : fd_(fd), valid_(fd >= 0) {}
    
    ssize_t read(void* buf, size_t len) override {
        return ::recv(fd_, buf, len, 0);
    }
    
    ssize_t write(const void* buf, size_t len) override {
        return ::send(fd_, buf, len, MSG_NOSIGNAL);
    }
    
    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
            valid_ = false;
        }
    }
    
    int fd() const override { return fd_; }
    bool is_valid() const override { return valid_; }
    
private:
    int fd_;
    bool valid_;
};

class SSLSocket : public SocketWrapper {
public:
    SSLSocket(int fd, SSL* ssl) : fd_(fd), ssl_(ssl), valid_(fd >= 0 && ssl != nullptr) {}
    
    ~SSLSocket() {
        close();
    }
    
    ssize_t read(void* buf, size_t len) override {
        if (!ssl_) return -1;
        int ret = SSL_read(ssl_, buf, len);
        if (ret <= 0) {
            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return 0; // Would block
            }
            return -1;
        }
        return ret;
    }
    
    ssize_t write(const void* buf, size_t len) override {
        if (!ssl_) return -1;
        int ret = SSL_write(ssl_, buf, len);
        if (ret <= 0) {
            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return 0; // Would block
            }
            return -1;
        }
        return ret;
    }
    
    void close() override {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        valid_ = false;
    }
    
    int fd() const override { return fd_; }
    bool is_valid() const override { return valid_; }
    
private:
    int fd_;
    SSL* ssl_;
    bool valid_;
};

// ============================================================
// HTTP Request
// ============================================================

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::map<std::string, std::string> params;
    
    bool has_header(const std::string& key) const {
        return headers.find(key) != headers.end();
    }
    
    std::string get_header(const std::string& key) const {
        auto it = headers.find(key);
        return (it != headers.end()) ? it->second : "";
    }
};

// ============================================================
// HTTP Response
// ============================================================

struct Response {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
    
    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }
    
    void set_content(const std::string& content, const std::string& content_type) {
        body = content;
        headers["Content-Type"] = content_type;
    }
};

// ============================================================
// SSE Client connection
// ============================================================

struct SSEClient {
    std::shared_ptr<SocketWrapper> socket;
    std::atomic<bool> connected;
    std::mutex write_mutex;
    std::string client_ip;
    std::string username;
    std::string path;  // Track which path this client is connected to
    
    explicit SSEClient(std::shared_ptr<SocketWrapper> sock, const std::string& ip = "", const std::string& user = "", const std::string& p = "") 
        : socket(sock), connected(true), client_ip(ip), username(user), path(p) {}
    
    bool send_event(const std::string& event, const std::string& data) {
        if (!connected || !socket->is_valid()) return false;
        std::lock_guard<std::mutex> lock(write_mutex);
        
        std::string message;
        if (!event.empty()) {
            message += "event: " + event + "\n";
        }
        
        // Split data by newlines
        std::istringstream stream(data);
        std::string line;
        while (std::getline(stream, line)) {
            message += "data: " + line + "\n";
        }
        message += "\n";
        
        ssize_t sent = socket->write(message.c_str(), message.length());
        if (sent <= 0) {
            connected = false;
            return false;
        }
        return true;
    }
    
    void close() {
        connected = false;
        socket->close();
    }
};

// ============================================================
// Route handler type
// ============================================================

using Handler = std::function<void(const Request&, Response&)>;

// ============================================================
// HTTP/HTTPS Server
// ============================================================

class Server {
public:
    // Login history tracking structure
    struct LoginAttempt {
        time_t timestamp;
        std::string username;
        std::string client_ip;
        bool success;
        std::string access_level;  // "info", "admin", or "failed"
    };
    
    Server() : running_(false), server_fd_(-1), ssl_ctx_(nullptr), use_https_(false) {}
    
    ~Server() {
        stop();
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
        }
    }
    
    // Enable HTTPS with certificate and key files
    bool set_https(const std::string& cert_file, const std::string& key_file) {
        // Initialize OpenSSL
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        
        // Create SSL context
        const SSL_METHOD* method = TLS_server_method();
        ssl_ctx_ = SSL_CTX_new(method);
        if (!ssl_ctx_) {
            return false;
        }
        
        // Set minimum TLS version to 1.2
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
        
        // Load certificate
        if (SSL_CTX_use_certificate_file(ssl_ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
            return false;
        }
        
        // Load private key
        if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
            return false;
        }
        
        // Verify private key matches certificate
        if (!SSL_CTX_check_private_key(ssl_ctx_)) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
            return false;
        }
        
        use_https_ = true;
        return true;
    }
    
    bool is_https() const { return use_https_; }
    
    // Set authentication credentials (empty = disabled)
    void set_auth(const std::string& username, const std::string& password) {
        if (!username.empty() && !password.empty()) {
            auth_enabled_ = true;
            auth_credentials_ = base64_encode(username + ":" + password);
        } else {
            auth_enabled_ = false;
        }
    }
    
    // Set admin authentication credentials
    void set_admin_auth(const std::string& username, const std::string& password) {
        if (!username.empty() && !password.empty()) {
            admin_credentials_ = base64_encode(username + ":" + password);
        }
    }
    
    // Get login history (thread-safe)
    std::vector<LoginAttempt> get_login_history() {
        std::lock_guard<std::mutex> lock(login_history_mutex_);
        return std::vector<LoginAttempt>(login_history_.begin(), login_history_.end());
    }
    
    // Route handlers
    void Get(const std::string& path, Handler handler) {
        routes_["GET"][path] = handler;
    }
    
    void Post(const std::string& path, Handler handler) {
        routes_["POST"][path] = handler;
    }
    
    // Register an SSE endpoint
    void SSE(const std::string& path) {
        sse_paths_.push_back(path);
    }
    
    // Register a raw stream endpoint (like SSE but without "data:" prefix)
    void RawStream(const std::string& path) {
        sse_paths_.push_back(path);  // Still needs to be in sse_paths_ to be handled
        raw_stream_paths_.push_back(path);  // Track separately for format detection
    }
    
    // Set fast notification for raw stream connections (just sets a flag, no heavy work!)
    void set_raw_stream_connect_notify(std::function<void()> notify) {
        raw_stream_connect_notify_ = notify;
    }
    
    // Send SSE event to all connected clients (except raw stream clients)
    void broadcast_sse(const std::string& event, const std::string& data) {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        for (auto it = sse_clients_.begin(); it != sse_clients_.end();) {
            // Skip raw stream clients - they only receive raw data
            bool is_raw_stream_client = false;
            for (const auto& raw_path : raw_stream_paths_) {
                if ((*it)->path == raw_path) {
                    is_raw_stream_client = true;
                    break;
                }
            }
            
            if (!is_raw_stream_client) {
                if (!(*it)->connected || !(*it)->send_event(event, data)) {
                    (*it)->close();
                    it = sse_clients_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    
    // Send raw data to clients on a specific path (for raw streaming like Gephi)
    void broadcast_raw_to_path(const std::string& path, const std::string& data) {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        int sent_count = 0;
        int total_clients = 0;
        for (auto it = sse_clients_.begin(); it != sse_clients_.end();) {
            // Only send to clients on the specified path
            if ((*it)->path == path) {
                total_clients++;
                if (!(*it)->connected) {
                    (*it)->close();
                    it = sse_clients_.erase(it);
                    continue;
                }
                
                // Send raw data without SSE formatting
                std::lock_guard<std::mutex> write_lock((*it)->write_mutex);
                ssize_t written = (*it)->socket->write(data.c_str(), data.size());
                if (written < 0 || static_cast<size_t>(written) != data.size()) {
                    BOOST_LOG_TRIVIAL(warning) << "[Web Plugin]\tFailed to send raw data to client on " << path << ": written=" << written << " expected=" << data.size();
                    (*it)->close();
                    it = sse_clients_.erase(it);
                    continue;
                } else {
                    sent_count++;
                }
            }
            ++it;
        }
        // if (total_clients > 0) {
        //     BOOST_LOG_TRIVIAL(info) << "[Web Plugin]\tSent " << data.size() << " bytes to " << sent_count << "/" << total_clients << " clients on " << path;
        // }
    }
    
    size_t sse_client_count() {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        return sse_clients_.size();
    }
    
    // Start server (blocking)
    bool listen(const std::string& host, int port) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return false;
        
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        
        if (host == "0.0.0.0" || host.empty()) {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        }
        
        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(server_fd_);
            return false;
        }
        
        if (::listen(server_fd_, 10) < 0) {
            ::close(server_fd_);
            return false;
        }
        
        running_ = true;
        
        while (running_) {
            struct pollfd pfd;
            pfd.fd = server_fd_;
            pfd.events = POLLIN;
            
            int ret = poll(&pfd, 1, 100); // 100ms timeout for checking running_
            if (ret > 0 && (pfd.revents & POLLIN)) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_fd >= 0) {
                    std::thread(&Server::handle_client, this, client_fd).detach();
                }
            }
        }
        
        ::close(server_fd_);
        return true;
    }
    
    // Start server in background thread
    bool listen_async(const std::string& host, int port) {
        server_thread_ = std::thread([this, host, port]() {
            listen(host, port);
        });
        // Give it a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return running_;
    }
    
    void stop() {
        running_ = false;
        
        // Close all SSE clients
        {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            for (auto& client : sse_clients_) {
                client->close();
            }
            sse_clients_.clear();
        }
        
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
        
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
    bool is_running() const { return running_; }

    // Get client IP address from socket file descriptor
    static std::string get_client_ip(int fd) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            return std::string(ip);
        }
        return "unknown";
    }

private:
    static std::string to_lower_ascii(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
            return static_cast<char>(c);
        });
        return s;
    }

    static std::string base64_decode(const std::string& encoded) {
        static const std::string base64_chars = 
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        
        std::string decoded;
        std::vector<int> T(256, -1);
        for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
        
        int val = 0, valb = -8;
        for (unsigned char c : encoded) {
            if (T[c] == -1) break;
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0) {
                decoded.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return decoded;
    }

    static std::string get_header_ci(const Request& req, const std::string& key) {
        const std::string k = to_lower_ascii(key);
        for (const auto& kv : req.headers) {
            if (to_lower_ascii(kv.first) == k) return kv.second;
        }
        return "";
    }

    static bool request_accepts_gzip(const Request& req) {
        const std::string ae = to_lower_ascii(get_header_ci(req, "Accept-Encoding"));
        // Minimal match; browsers typically send: gzip, deflate, br
        return ae.find("gzip") != std::string::npos;
    }

    static bool gzip_compress(const std::string& input, std::string& output) {
        output.clear();
        if (input.empty()) return true;

        z_stream zs;
        std::memset(&zs, 0, sizeof(zs));

        // windowBits = 15 + 16 produces gzip wrapper.
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return false;
        }

        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
        zs.avail_in = static_cast<uInt>(input.size());

        std::string out;
        out.resize(64 * 1024);
        zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
        zs.avail_out = static_cast<uInt>(out.size());

        int ret = Z_OK;
        while (ret == Z_OK) {
            ret = deflate(&zs, Z_FINISH);
            if (ret == Z_OK) {
                const size_t used = out.size() - zs.avail_out;
                output.append(out.data(), used);
                out.resize(out.size() * 2);
                zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
                zs.avail_out = static_cast<uInt>(out.size());
            }
        }

        if (ret != Z_STREAM_END) {
            deflateEnd(&zs);
            output.clear();
            return false;
        }

        const size_t used = out.size() - zs.avail_out;
        output.append(out.data(), used);
        deflateEnd(&zs);
        return true;
    }

    std::shared_ptr<SocketWrapper> wrap_socket(int client_fd) {
        if (use_https_ && ssl_ctx_) {
            SSL* ssl = SSL_new(ssl_ctx_);
            SSL_set_fd(ssl, client_fd);
            
            if (SSL_accept(ssl) <= 0) {
                SSL_free(ssl);
                ::close(client_fd);
                return nullptr;
            }
            
            return std::make_shared<SSLSocket>(client_fd, ssl);
        } else {
            return std::make_shared<PlainSocket>(client_fd);
        }
    }
    
    void handle_client(int client_fd) {
        auto socket = wrap_socket(client_fd);
        if (!socket) return;
        
        // Set socket timeout
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        // Read request with size limit to prevent DoS
        char buffer[8192];
        std::string request_data;
        ssize_t bytes_read;
        const size_t MAX_REQUEST_SIZE = 10 * 1024 * 1024;  // 10MB limit (generous for config files)
        
        // Read until we have complete headers
        while ((bytes_read = socket->read(buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            request_data += buffer;
            
            // Enforce size limit
            if (request_data.size() > MAX_REQUEST_SIZE) {
                Response res;
                res.status = 413;
                res.set_content("Request Entity Too Large", "text/plain");
                send_response(socket, Request(), res);
                socket->close();
                return;
            }
            
            // Check if we have complete headers
            size_t header_end = request_data.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                // Parse Content-Length to see if we need to read more body data
                size_t cl_pos = request_data.find("Content-Length:");
                if (cl_pos != std::string::npos && cl_pos < header_end) {
                    size_t cl_end = request_data.find("\r\n", cl_pos);
                    std::string cl_str = request_data.substr(cl_pos + 15, cl_end - cl_pos - 15);
                    // Trim whitespace
                    while (!cl_str.empty() && (cl_str[0] == ' ' || cl_str[0] == '\t')) cl_str.erase(0, 1);
                    
                    size_t content_length = std::stoull(cl_str);
                    size_t body_start = header_end + 4;
                    size_t body_received = (request_data.size() > body_start) ? (request_data.size() - body_start) : 0;
                    
                    // Continue reading until we have the complete body
                    while (body_received < content_length) {
                        bytes_read = socket->read(buffer, sizeof(buffer) - 1);
                        if (bytes_read <= 0) break;
                        buffer[bytes_read] = '\0';
                        request_data += buffer;
                        body_received += bytes_read;
                        
                        if (request_data.size() > MAX_REQUEST_SIZE) {
                            Response res;
                            res.status = 413;
                            res.set_content("Request Entity Too Large", "text/plain");
                            send_response(socket, Request(), res);
                            socket->close();
                            return;
                        }
                    }
                }
                break;
            }
        }
        
        if (request_data.empty()) {
            socket->close();
            return;
        }
        
        Request req = parse_request(request_data);
        Response res;
        
        // Allow /health without authentication
        bool skip_auth = (req.path == "/health");
        
        // Determine required access level and check authentication
        bool requires_admin = (req.path.find("/api/admin/") == 0);
        std::string client_ip = get_client_ip(socket->fd());
        
        // Only check auth if: (1) auth is enabled for non-admin paths, OR (2) this is an admin path
        bool needs_auth_check = (auth_enabled_ && !skip_auth) || requires_admin;
        
        if (needs_auth_check) {
            std::string auth = get_header_ci(req, "Authorization");
            std::string decoded_creds = "";
            std::string attempted_user = "none";
            
            if (!auth.empty() && auth.substr(0, 6) == "Basic ") {
                decoded_creds = auth.substr(6);
                std::string decoded = base64_decode(decoded_creds);
                size_t colon = decoded.find(':');
                if (colon != std::string::npos) {
                    attempted_user = decoded.substr(0, colon);
                }
            }
            
            bool is_admin = (!admin_credentials_.empty() && !decoded_creds.empty() && decoded_creds == admin_credentials_);
            bool is_info = (auth_enabled_ && !auth_credentials_.empty() && !decoded_creds.empty() && decoded_creds == auth_credentials_);
            bool authenticated = is_admin || (is_info && !requires_admin) || (!auth_enabled_ && !requires_admin);
            
            // Track login attempt - only log FIRST authentication per session (not every request)
            // Skip logging if credentials are empty (e.g., logout)
            if (!decoded_creds.empty() && !attempted_user.empty()) {
                std::string session_key = client_ip + ":" + attempted_user;
                static std::unordered_set<std::string> logged_sessions;
                static std::mutex session_mutex;
                
                bool should_log = false;
                {
                    std::lock_guard<std::mutex> session_lock(session_mutex);
                    if (logged_sessions.find(session_key) == logged_sessions.end()) {
                        logged_sessions.insert(session_key);
                        should_log = true;
                        
                        // Prevent unbounded growth - clear old sessions periodically
                        if (logged_sessions.size() > 1000) {
                            logged_sessions.clear();
                        }
                    }
                }
                
                if (should_log) {
                    std::lock_guard<std::mutex> lock(login_history_mutex_);
                    LoginAttempt attempt;
                    attempt.timestamp = time(nullptr);
                    attempt.username = attempted_user;
                    attempt.client_ip = client_ip;
                    attempt.success = authenticated;
                    attempt.access_level = authenticated ? (is_admin ? "admin" : "info") : "failed";
                    
                    login_history_.push_back(attempt);
                    if (login_history_.size() > MAX_LOGIN_HISTORY) {
                        login_history_.pop_front();
                    }
                    
                    // Only log to console if authentication actually failed with provided credentials
                    if (!authenticated) {
                        BOOST_LOG_TRIVIAL(error) << "[Web Plugin]\tAuthentication failed - user: " << attempted_user << " from " << client_ip;
                    }
                }
            }
            
            if (!authenticated) {
                
                res.status = 401;
                res.set_header("WWW-Authenticate", "Basic realm=\"Trunk-Recorder\"");
                res.set_content("Unauthorized", "text/plain");
                send_response(socket, req, res);
                socket->close();
                return;
            }
        }
        
        // Check if this is an SSE request
        bool is_sse = false;
        for (const auto& sse_path : sse_paths_) {
            if (req.path == sse_path) {
                is_sse = true;
                break;
            }
        }
        
        if (is_sse) {
            std::string client_ip = get_client_ip(socket->fd());
            handle_sse_client(socket, req, client_ip);
            return;
        }
        
        // Find and execute route handler
        auto method_routes = routes_.find(req.method);
        if (method_routes != routes_.end()) {
            auto handler = method_routes->second.find(req.path);
            if (handler != method_routes->second.end()) {
                handler->second(req, res);
            } else {
                res.status = 404;
                res.set_content("Not Found", "text/plain");
            }
        } else {
            res.status = 405;
            res.set_content("Method Not Allowed", "text/plain");
        }
        
        send_response(socket, req, res);
        socket->close();
    }
    
    void handle_sse_client(std::shared_ptr<SocketWrapper> socket, const Request& req, const std::string& client_ip) {
        // Extract username from Basic Auth if present
        std::string username = "anonymous";
        std::string auth = get_header_ci(req, "Authorization");
        if (!auth.empty() && auth.substr(0, 6) == "Basic ") {
            std::string decoded = base64_decode(auth.substr(6));
            size_t colon = decoded.find(':');
            if (colon != std::string::npos) {
                username = decoded.substr(0, colon);
            }
        }

        // Check if this is a raw stream (like Gephi)
        bool is_raw_stream = false;
        for (const auto& path : raw_stream_paths_) {
            if (req.path == path) {
                is_raw_stream = true;
                break;
            }
        }

        // Send appropriate headers
        std::string response;
        if (is_raw_stream) {
            // Raw stream - just HTTP 200 with chunked encoding
            response = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n";
        } else {
            // SSE - needs text/event-stream
            response = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n";
        }
        
        if (socket->write(response.c_str(), response.length()) <= 0) {
            socket->close();
            return;
        }
        
        // Add to SSE clients (and raw stream clients if applicable)
        auto client = std::make_shared<SSEClient>(socket, client_ip, username, req.path);
        {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            sse_clients_.push_back(client);
            if (is_raw_stream) {
                raw_stream_clients_.push_back(client);
            }
        }
        
        // Log connection
        BOOST_LOG_TRIVIAL(info) << "[Web Plugin]\t" << (is_raw_stream ? "Raw stream" : "SSE") << " session started - user: " << username << " from " << client_ip << " path: " << req.path;
        
        // Send initial keepalive (only for SSE, not raw streams)
        if (!is_raw_stream) {
            client->send_event("", "connected");
        } else if (raw_stream_connect_notify_) {
            // Notify plugin that a raw stream client connected (just sets a flag)
            raw_stream_connect_notify_();
        }
        
        // Keep connection alive until client disconnects
        char dummy[1];
        while (running_ && client->connected) {
            struct pollfd pfd;
            pfd.fd = socket->fd();
            pfd.events = POLLIN;
            
            int ret = poll(&pfd, 1, 1000);
            if (ret > 0) {
                // Client sent something (probably closed)
                ssize_t n = socket->read(dummy, 1);
                if (n <= 0) {
                    break;
                }
            }
        }
        
        // Remove from SSE clients and raw stream clients (if applicable)
        {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            sse_clients_.erase(
                std::remove_if(sse_clients_.begin(), sse_clients_.end(),
                    [&client](const std::shared_ptr<SSEClient>& c) { return c.get() == client.get(); }),
                sse_clients_.end());
            if (is_raw_stream) {
                raw_stream_clients_.erase(
                    std::remove_if(raw_stream_clients_.begin(), raw_stream_clients_.end(),
                        [&client](const std::shared_ptr<SSEClient>& c) { return c.get() == client.get(); }),
                    raw_stream_clients_.end());
            }
        }
        
        // Log disconnection
        BOOST_LOG_TRIVIAL(info) << "[Web Plugin]\tSSE session ended - user: " << client->username << " from " << client->client_ip;
        
        client->close();
    }
    
    Request parse_request(const std::string& data) {
        Request req;
        std::istringstream stream(data);
        std::string line;
        
        // Parse request line
        if (std::getline(stream, line)) {
            size_t pos1 = line.find(' ');
            size_t pos2 = line.find(' ', pos1 + 1);
            if (pos1 != std::string::npos && pos2 != std::string::npos) {
                req.method = line.substr(0, pos1);
                std::string path_and_query = line.substr(pos1 + 1, pos2 - pos1 - 1);
                
                // Parse query string
                size_t query_pos = path_and_query.find('?');
                if (query_pos != std::string::npos) {
                    req.path = path_and_query.substr(0, query_pos);
                    std::string query = path_and_query.substr(query_pos + 1);
                    // Parse query parameters
                    std::istringstream query_stream(query);
                    std::string param;
                    while (std::getline(query_stream, param, '&')) {
                        size_t eq_pos = param.find('=');
                        if (eq_pos != std::string::npos) {
                            req.params[param.substr(0, eq_pos)] = param.substr(eq_pos + 1);
                        }
                    }
                } else {
                    req.path = path_and_query;
                }
            }
        }
        
        // Parse headers
        while (std::getline(stream, line) && line != "\r" && !line.empty()) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                // Trim whitespace and \r
                while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
                req.headers[key] = value;
            }
        }
        
        // Body (if any)
        size_t header_end = data.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            req.body = data.substr(header_end + 4);
        }
        
        return req;
    }
    
    void send_response(std::shared_ptr<SocketWrapper> socket, const Request& req, const Response& res) {
        Response final_res = res;

        // Opportunistic gzip for large bodies (e.g., /api/status) when the client accepts it.
        // Keep this conservative and avoid compressing tiny responses.
        const bool already_encoded = final_res.headers.find("Content-Encoding") != final_res.headers.end();
        if (!already_encoded && final_res.status == 200 && final_res.body.size() >= 16 * 1024 && request_accepts_gzip(req)) {
            std::string gz;
            if (gzip_compress(final_res.body, gz) && !gz.empty() && gz.size() < final_res.body.size()) {
                final_res.body.swap(gz);
                final_res.headers["Content-Encoding"] = "gzip";
                // Ensure intermediates cache correctly.
                if (final_res.headers.find("Vary") == final_res.headers.end()) {
                    final_res.headers["Vary"] = "Accept-Encoding";
                }
                // Any existing Content-Length is now invalid.
                final_res.headers.erase("Content-Length");
            }
        }

        std::ostringstream stream;
        
        stream << "HTTP/1.1 " << final_res.status << " " << status_text(final_res.status) << "\r\n";
        stream << "Server: tr-web/1.0\r\n";
        stream << "Connection: close\r\n";
        
        for (const auto& header : final_res.headers) {
            stream << header.first << ": " << header.second << "\r\n";
        }
        
        if (!final_res.body.empty() && final_res.headers.find("Content-Length") == final_res.headers.end()) {
            stream << "Content-Length: " << final_res.body.length() << "\r\n";
        }
        
        stream << "\r\n";
        stream << final_res.body;
        
        std::string response = stream.str();
        socket->write(response.c_str(), response.length());
    }
    
    static std::string status_text(int status) {
        switch (status) {
            case 200: return "OK";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 500: return "Internal Server Error";
            default: return "Unknown";
        }
    }
    
    std::atomic<bool> running_;
    int server_fd_;
    std::thread server_thread_;
    
    std::map<std::string, std::map<std::string, Handler>> routes_;
    std::vector<std::string> sse_paths_;
    std::vector<std::string> raw_stream_paths_;  // Track raw stream paths separately
    
    std::mutex sse_mutex_;
    std::vector<std::shared_ptr<SSEClient>> sse_clients_;
    // Track raw stream clients (for /graph-stream)
    std::vector<std::shared_ptr<SSEClient>> raw_stream_clients_;
    public:
        // Return the number of connected raw stream (graphstream) clients
        size_t raw_stream_client_count() {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            return raw_stream_clients_.size();
        }

        // Remove a raw stream client from the list
        void remove_raw_stream_client(const std::shared_ptr<SSEClient>& client) {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            raw_stream_clients_.erase(
                std::remove_if(raw_stream_clients_.begin(), raw_stream_clients_.end(),
                    [&client](const std::shared_ptr<SSEClient>& c) { return c.get() == client.get(); }),
                raw_stream_clients_.end());
        }
    
    std::function<void()> raw_stream_connect_notify_;  // Fast notification for raw stream connects
    
    bool auth_enabled_ = false;
    std::string auth_credentials_;
    std::string admin_credentials_;
    
    // Login history tracking
    std::mutex login_history_mutex_;
    std::deque<LoginAttempt> login_history_;
    static const size_t MAX_LOGIN_HISTORY = 50;
    
    // HTTPS/SSL
    SSL_CTX* ssl_ctx_;
    bool use_https_;
};

} // namespace httplib

#endif // TR_WEB_HTTPLIB_H
