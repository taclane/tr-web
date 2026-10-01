// httplib.h - Minimal Header-Only HTTP/HTTPS Server for tr-web
// ============================================================

#ifndef TR_WEB_HTTPLIB_H
#define TR_WEB_HTTPLIB_H

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include <zlib.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

// OpenSSL for HTTPS support
#include <openssl/err.h>
#include <openssl/ssl.h>

// Boost for logging
#include <boost/log/trivial.hpp>

namespace httplib
{

    // ============================================================
    // Base64 encoding/decoding for Basic Auth
    // ============================================================

    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    inline std::string base64_encode(const std::string &in)
    {
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : in)
        {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0)
            {
                out.push_back(base64_chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6)
            out.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4)
            out.push_back('=');
        return out;
    }

    inline std::string base64_decode(const std::string &in)
    {
        std::string out;
        std::vector<int> T(256, -1);
        for (int i = 0; i < 64; i++)
            T[base64_chars[i]] = i;
        int val = 0, valb = -8;
        for (unsigned char c : in)
        {
            if (T[c] == -1)
                break;
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0)
            {
                out.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    // ============================================================
    // Socket wrapper for unified HTTP/HTTPS handling
    // ============================================================

    class SocketWrapper
    {
    public:
        virtual ~SocketWrapper() = default;
        virtual ssize_t read(void *buf, size_t len) = 0;
        virtual ssize_t write(const void *buf, size_t len) = 0;
        virtual void close() = 0;
        virtual int fd() const = 0;
        virtual bool is_valid() const = 0;

        // Wake a thread blocked on this socket without releasing the fd (so it can't be reused)
        void shutdown_io()
        {
            if (fd() >= 0)
                ::shutdown(fd(), SHUT_RDWR);
        }

        // Applies to plain and TLS sockets alike (OpenSSL writes through the same fd)
        void set_timeouts(int recv_seconds, int send_seconds)
        {
            if (fd() < 0)
                return;
            struct timeval tv;
            tv.tv_usec = 0;
            tv.tv_sec = recv_seconds;
            setsockopt(fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            tv.tv_sec = send_seconds;
            setsockopt(fd(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
    };

    // Write the whole buffer. A plain socket returns a partial count when SO_SNDTIMEO expires
    // mid-buffer; <= 0 means error or timeout.
    inline bool write_all(SocketWrapper &socket, const char *data, size_t len)
    {
        while (len > 0)
        {
            ssize_t n = socket.write(data, len);
            if (n <= 0)
                return false;
            data += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }

    // OpenSSL writes with write(): a closed peer raises SIGPIPE, which kills trunk-recorder.
    // Block it in threads that write to clients (new threads inherit the mask).
    inline void block_sigpipe_in_this_thread()
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }

    class PlainSocket : public SocketWrapper
    {
    public:
        explicit PlainSocket(int fd) : fd_(fd), valid_(fd >= 0) {}

        ~PlainSocket()
        {
            close();
        }

        ssize_t read(void *buf, size_t len) override
        {
            return ::recv(fd_, buf, len, 0);
        }

        ssize_t write(const void *buf, size_t len) override
        {
            return ::send(fd_, buf, len, MSG_NOSIGNAL);
        }

        void close() override
        {
            if (fd_ >= 0)
            {
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

    class SSLSocket : public SocketWrapper
    {
    public:
        SSLSocket(int fd, SSL *ssl) : fd_(fd), ssl_(ssl), valid_(fd >= 0 && ssl != nullptr) {}

        ~SSLSocket()
        {
            close();
        }

        ssize_t read(void *buf, size_t len) override
        {
            if (!ssl_)
                return -1;
            int ret = SSL_read(ssl_, buf, len);
            if (ret <= 0)
            {
                int err = SSL_get_error(ssl_, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                {
                    return 0; // Would block
                }
                return -1;
            }
            return ret;
        }

        ssize_t write(const void *buf, size_t len) override
        {
            if (!ssl_)
                return -1;
            int ret = SSL_write(ssl_, buf, len);
            if (ret <= 0)
            {
                int err = SSL_get_error(ssl_, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                {
                    return 0; // Would block
                }
                return -1;
            }
            return ret;
        }

        void close() override
        {
            if (ssl_)
            {
                SSL_shutdown(ssl_);
                SSL_free(ssl_);
                ssl_ = nullptr;
            }
            if (fd_ >= 0)
            {
                ::close(fd_);
                fd_ = -1;
            }
            valid_ = false;
        }

        int fd() const override { return fd_; }
        bool is_valid() const override { return valid_; }

    private:
        int fd_;
        SSL *ssl_;
        bool valid_;
    };

    // ============================================================
    // HTTP Request
    // ============================================================

    // Header names are case-insensitive (RFC 9110); proxies and HTTP/2 gateways often lowercase them
    struct CaseInsensitiveLess
    {
        bool operator()(const std::string &a, const std::string &b) const
        {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                                [](unsigned char x, unsigned char y)
                                                { return std::tolower(x) < std::tolower(y); });
        }
    };

    struct Request
    {
        std::string method;
        std::string path;
        std::map<std::string, std::string, CaseInsensitiveLess> headers;
        std::string body;
        std::map<std::string, std::string> params;
        std::string remote_addr; // Peer address of the TCP connection (not proxy headers)

        bool has_header(const std::string &key) const
        {
            return headers.find(key) != headers.end();
        }

        std::string get_header(const std::string &key) const
        {
            auto it = headers.find(key);
            return (it != headers.end()) ? it->second : "";
        }
    };

    // ============================================================
    // HTTP Response
    // ============================================================

    struct Response
    {
        int status = 200;
        std::map<std::string, std::string> headers;
        std::string body;

        void set_header(const std::string &key, const std::string &value)
        {
            headers[key] = value;
        }

        void set_content(const std::string &content, const std::string &content_type)
        {
            body = content;
            headers["Content-Type"] = content_type;
        }
    };

    // ============================================================
    // SSE Client connection
    // ============================================================

    struct SSEClient
    {
        std::shared_ptr<SocketWrapper> socket;
        std::atomic<bool> connected;
        std::mutex write_mutex;
        std::string client_ip;
        std::string username;
        std::string path; // Track which path this client is connected to
        bool is_raw_stream = false;
        // Raw stream clients start out waiting for the plugin's initial graph dump
        std::atomic<bool> needs_initial_state{false};

        explicit SSEClient(std::shared_ptr<SocketWrapper> sock, const std::string &ip = "", const std::string &user = "", const std::string &p = "")
            : socket(sock), connected(true), client_ip(ip), username(user), path(p) {
            // Short send timeout so a stalled client is dropped instead of stalling broadcasts
            socket->set_timeouts(5, 2);
        }

        // Send raw bytes (no SSE framing), serialized with send_event()
        bool send_raw(const std::string &data)
        {
            if (!connected || !socket->is_valid())
                return false;
            std::lock_guard<std::mutex> lock(write_mutex);
            if (!write_all(*socket, data.data(), data.size()))
            {
                connected = false;
                return false;
            }
            return true;
        }

        bool send_event(const std::string &event, const std::string &data)
        {
            if (!connected || !socket->is_valid())
                return false;
            std::lock_guard<std::mutex> lock(write_mutex);

            std::string message;
            if (!event.empty())
            {
                message += "event: " + event + "\n";
            }

            // Split data by newlines
            std::istringstream stream(data);
            std::string line;
            while (std::getline(stream, line))
            {
                message += "data: " + line + "\n";
            }
            message += "\n";

            // Any failed or partial write drops the client: a partial frame corrupts the stream
            if (!write_all(*socket, message.data(), message.size()))
            {
                connected = false;
                return false;
            }
            return true;
        }

        // Called from other threads: mark dead and wake the owning handler thread, which closes
        void mark_dead()
        {
            connected = false;
            socket->shutdown_io();
        }

        // Other threads may still hold this client, so only shut the socket down; its
        // destructor frees the fd and TLS state after the last user.
        void close()
        {
            connected = false;
            socket->shutdown_io();
        }

        // Serialized with writes: OpenSSL can't read and write one connection from two threads
        ssize_t read(void *buf, size_t len)
        {
            std::lock_guard<std::mutex> lock(write_mutex);
            return socket->read(buf, len);
        }
    };

    // ============================================================
    // Route handler type
    // ============================================================

    using Handler = std::function<void(const Request &, Response &)>;

    // ============================================================
    // HTTP/HTTPS Server
    // ============================================================

    class Server
    {
    public:
        // Login history tracking structure
        struct LoginAttempt
        {
            time_t timestamp;
            std::string username;
            std::string client_ip;
            bool success;
            std::string access_level; // "info", "admin", or "failed"
        };

        Server() : running_(false), server_fd_(-1), ssl_ctx_(nullptr), use_https_(false) {}

        ~Server()
        {
            stop();
            if (ssl_ctx_)
            {
                SSL_CTX_free(ssl_ctx_);
            }
        }

        // Enable HTTPS with certificate and key files
        bool set_https(const std::string &cert_file, const std::string &key_file)
        {
            // Initialize OpenSSL
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();

            // Create SSL context
            const SSL_METHOD *method = TLS_server_method();
            ssl_ctx_ = SSL_CTX_new(method);
            if (!ssl_ctx_)
            {
                return false;
            }

            // Set minimum TLS version to 1.2
            SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);

            // Load certificate
            if (SSL_CTX_use_certificate_file(ssl_ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
            {
                SSL_CTX_free(ssl_ctx_);
                ssl_ctx_ = nullptr;
                return false;
            }

            // Load private key
            if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
            {
                SSL_CTX_free(ssl_ctx_);
                ssl_ctx_ = nullptr;
                return false;
            }

            // Verify private key matches certificate
            if (!SSL_CTX_check_private_key(ssl_ctx_))
            {
                SSL_CTX_free(ssl_ctx_);
                ssl_ctx_ = nullptr;
                return false;
            }

            use_https_ = true;
            return true;
        }

        bool is_https() const { return use_https_; }

        // Cap on simultaneous connections (each one holds a thread, streams included)
        void set_max_connections(int max_connections)
        {
            max_connections_ = max_connections > 0 ? max_connections : 1;
        }

        // Get login history (thread-safe)
        std::vector<LoginAttempt> get_login_history()
        {
            std::lock_guard<std::mutex> lock(login_history_mutex_);
            return std::vector<LoginAttempt>(login_history_.begin(), login_history_.end());
        }

        // Track login attempt manually (for session-based auth)
        // `when` defaults to now; history restored at startup passes the original time
        void track_login_attempt(const std::string &username, const std::string &client_ip, bool success, const std::string &access_level, time_t when = 0)
        {
            std::lock_guard<std::mutex> lock(login_history_mutex_);
            LoginAttempt attempt;
            attempt.timestamp = when ? when : time(nullptr);
            attempt.username = username;
            attempt.client_ip = client_ip;
            attempt.success = success;
            attempt.access_level = access_level;

            login_history_.push_back(attempt);
            if (login_history_.size() > MAX_LOGIN_HISTORY)
            {
                login_history_.pop_front();
            }
        }

        // Route handlers
        void Get(const std::string &path, Handler handler)
        {
            routes_["GET"][path] = handler;
        }

        void Post(const std::string &path, Handler handler)
        {
            routes_["POST"][path] = handler;
        }

        // Register an SSE endpoint
        void SSE(const std::string &path)
        {
            sse_paths_.push_back(path);
        }

        // Register a raw stream endpoint (like SSE but without "data:" prefix)
        void RawStream(const std::string &path)
        {
            sse_paths_.push_back(path);        // Still needs to be in sse_paths_ to be handled
            raw_stream_paths_.push_back(path); // Track separately for format detection
        }

        // Set fast notification for raw stream connections (just sets a flag, no heavy work!)
        void set_raw_stream_connect_notify(std::function<void()> notify)
        {
            raw_stream_connect_notify_ = notify;
        }

        // Set authentication callback for SSE/RawStream endpoints
        void set_sse_auth_callback(std::function<bool(const Request&)> callback)
        {
            sse_auth_callback_ = callback;
        }

        // Set username extraction callback for SSE logging
        void set_sse_username_callback(std::function<std::string(const Request&)> callback)
        {
            sse_username_callback_ = callback;
        }

        // Send an SSE event to all SSE clients. Writes to a snapshot, outside sse_mutex_.
        void broadcast_sse(const std::string &event, const std::string &data)
        {
            std::vector<std::shared_ptr<SSEClient>> failed;
            for (auto &client : snapshot_clients([](const SSEClient &c)
                                                 { return !c.is_raw_stream; }))
            {
                if (!client->send_event(event, data))
                    failed.push_back(client);
            }
            drop_clients(failed);
        }

        // Send raw data to clients on a specific path (for raw streaming like Gephi)
        void broadcast_raw_to_path(const std::string &path, const std::string &data)
        {
            std::vector<std::shared_ptr<SSEClient>> failed;
            for (auto &client : snapshot_clients([&path](const SSEClient &c)
                                                 { return c.path == path; }))
            {
                if (!client->send_raw(data))
                    failed.push_back(client);
            }
            drop_clients(failed);
        }

        // Send raw data only to clients on `path` still waiting for their initial state,
        // so existing clients are not sent the full dump again
        void send_raw_initial_state(const std::string &path, const std::string &data)
        {
            std::vector<std::shared_ptr<SSEClient>> failed;
            for (auto &client : snapshot_clients([&path](const SSEClient &c)
                                                 { return c.path == path && c.needs_initial_state; }))
            {
                client->needs_initial_state = false;
                if (!client->send_raw(data))
                    failed.push_back(client);
            }
            drop_clients(failed);
        }

        bool raw_clients_awaiting_initial_state()
        {
            return !snapshot_clients([](const SSEClient &c)
                                     { return c.is_raw_stream && c.needs_initial_state; })
                        .empty();
        }

        // Lock-free: called from trunk-recorder's own threads on every plugin callback
        size_t sse_client_count() const
        {
            return sse_client_count_.load();
        }

        // Start server (blocking)
        bool listen(const std::string &host, int port)
        {
            // Connection threads are started from this thread and inherit its signal mask
            block_sigpipe_in_this_thread();

            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (host == "0.0.0.0" || host.empty())
            {
                addr.sin_addr.s_addr = INADDR_ANY;
            }
            else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
            {
                BOOST_LOG_TRIVIAL(error) << "[Web Plugin]\tInvalid bind address: " << host << " (IPv4 address expected)";
                return false;
            }

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                return false;

            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || ::listen(fd, 16) < 0)
            {
                BOOST_LOG_TRIVIAL(error) << "[Web Plugin]\tCannot listen on " << host << ":" << port << ": " << strerror(errno);
                ::close(fd);
                return false;
            }

            server_fd_ = fd;
            running_ = true;
            time_t last_reject_log = 0;

            while (running_)
            {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLIN;

                int ret = poll(&pfd, 1, 100); // 100ms timeout for checking running_
                if (ret > 0 && (pfd.revents & POLLIN))
                {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
                    if (client_fd < 0)
                        continue;

                    if (active_connections_.load() >= max_connections_)
                    {
                        ::close(client_fd);
                        time_t now = time(nullptr);
                        if (now - last_reject_log >= 60)
                        {
                            last_reject_log = now;
                            BOOST_LOG_TRIVIAL(warning) << "[Web Plugin]\tConnection limit (" << max_connections_ << ") reached; refusing new connections";
                        }
                        continue;
                    }

                    ++active_connections_;
                    try
                    {
                        std::thread(&Server::handle_client, this, client_fd).detach();
                    }
                    catch (const std::system_error &e)
                    {
                        --active_connections_;
                        ::close(client_fd);
                        BOOST_LOG_TRIVIAL(error) << "[Web Plugin]\tCannot start connection thread: " << e.what();
                    }
                }
            }

            // Only this thread closes the listening socket
            server_fd_ = -1;
            ::close(fd);
            return true;
        }

        // Start server in background thread
        bool listen_async(const std::string &host, int port)
        {
            server_thread_ = std::thread([this, host, port]()
                                         { listen(host, port); });
            // Give it a moment to start
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return running_;
        }

        // Stop accepting, drop stream clients and wait (bounded) for connection threads, which
        // call back into the plugin
        void stop()
        {
            running_ = false;

            {
                std::lock_guard<std::mutex> lock(sse_mutex_);
                for (auto &client : sse_clients_)
                {
                    client->mark_dead();
                }
            }

            if (server_thread_.joinable())
            {
                server_thread_.join();
            }

            // Threads exit within one poll interval or the 5 s receive timeout
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
            while (active_connections_.load() > 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (active_connections_.load() > 0)
            {
                BOOST_LOG_TRIVIAL(warning) << "[Web Plugin]\t" << active_connections_.load() << " connection thread(s) still running at shutdown";
            }
        }

        bool is_running() const { return running_; }

        // Get client IP address from socket file descriptor
        static std::string get_client_ip(int fd)
        {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == 0)
            {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
                return std::string(ip);
            }
            return "unknown";
        }

    private:
        template <typename Pred>
        std::vector<std::shared_ptr<SSEClient>> snapshot_clients(Pred pred)
        {
            std::vector<std::shared_ptr<SSEClient>> out;
            std::lock_guard<std::mutex> lock(sse_mutex_);
            for (auto &client : sse_clients_)
            {
                if (client->connected && pred(*client))
                    out.push_back(client);
            }
            return out;
        }

        // Unregister failed clients and wake their handler threads (which close the sockets)
        void drop_clients(const std::vector<std::shared_ptr<SSEClient>> &failed)
        {
            if (failed.empty())
                return;
            std::lock_guard<std::mutex> lock(sse_mutex_);
            for (auto &client : failed)
            {
                client->mark_dead();
                remove_client_locked(client);
            }
        }

        void remove_client_locked(const std::shared_ptr<SSEClient> &client)
        {
            sse_clients_.erase(std::remove(sse_clients_.begin(), sse_clients_.end(), client), sse_clients_.end());
            raw_stream_clients_.erase(std::remove(raw_stream_clients_.begin(), raw_stream_clients_.end(), client), raw_stream_clients_.end());
            sse_client_count_ = sse_clients_.size();
            raw_stream_client_count_ = raw_stream_clients_.size();
        }

        static std::string to_lower_ascii(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                           {
      if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
      return static_cast<char>(c); });
            return s;
        }

        static std::string base64_decode(const std::string &encoded)
        {
            static const std::string base64_chars =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

            std::string decoded;
            std::vector<int> T(256, -1);
            for (int i = 0; i < 64; i++)
                T[base64_chars[i]] = i;

            int val = 0, valb = -8;
            for (unsigned char c : encoded)
            {
                if (T[c] == -1)
                    break;
                val = (val << 6) + T[c];
                valb += 6;
                if (valb >= 0)
                {
                    decoded.push_back(char((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
            return decoded;
        }

        static std::string get_header_ci(const Request &req, const std::string &key)
        {
            return req.get_header(key);
        }

        // Parse Content-Length from the raw header block. Returns false if the header is
        // present but not a plain non-negative integer.
        static bool parse_content_length(const std::string &header_block, size_t &out)
        {
            out = 0;
            std::istringstream stream(header_block);
            std::string line;
            while (std::getline(stream, line))
            {
                size_t colon = line.find(':');
                if (colon == std::string::npos || to_lower_ascii(line.substr(0, colon)) != "content-length")
                    continue;
                std::string value = line.substr(colon + 1);
                size_t first = value.find_first_not_of(" \t");
                size_t last = value.find_last_not_of(" \t\r\n");
                if (first == std::string::npos)
                    return false;
                value = value.substr(first, last - first + 1);
                if (value.empty() || value.size() > 18 || value.find_first_not_of("0123456789") != std::string::npos)
                    return false;
                out = static_cast<size_t>(std::stoull(value));
                return true;
            }
            return true;
        }

        static bool is_json_content_type(const Request &req)
        {
            const std::string ct = to_lower_ascii(req.get_header("Content-Type"));
            return ct.compare(0, 16, "application/json") == 0;
        }

        void send_error(std::shared_ptr<SocketWrapper> socket, const Request &req, int status, const std::string &msg)
        {
            Response res;
            res.status = status;
            res.set_content(msg, "text/plain");
            send_response(socket, req, res);
            socket->close();
        }

        static bool request_accepts_gzip(const Request &req)
        {
            const std::string ae = to_lower_ascii(get_header_ci(req, "Accept-Encoding"));
            // Minimal match; browsers typically send: gzip, deflate, br
            return ae.find("gzip") != std::string::npos;
        }

        static bool gzip_compress(const std::string &input, std::string &output)
        {
            output.clear();
            if (input.empty())
                return true;

            z_stream zs;
            std::memset(&zs, 0, sizeof(zs));

            // windowBits = 15 + 16 produces gzip wrapper.
            if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            {
                return false;
            }

            zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
            zs.avail_in = static_cast<uInt>(input.size());

            std::string out;
            out.resize(64 * 1024);
            zs.next_out = reinterpret_cast<Bytef *>(&out[0]);
            zs.avail_out = static_cast<uInt>(out.size());

            int ret = Z_OK;
            while (ret == Z_OK)
            {
                ret = deflate(&zs, Z_FINISH);
                if (ret == Z_OK)
                {
                    const size_t used = out.size() - zs.avail_out;
                    output.append(out.data(), used);
                    out.resize(out.size() * 2);
                    zs.next_out = reinterpret_cast<Bytef *>(&out[0]);
                    zs.avail_out = static_cast<uInt>(out.size());
                }
            }

            if (ret != Z_STREAM_END)
            {
                deflateEnd(&zs);
                output.clear();
                return false;
            }

            const size_t used = out.size() - zs.avail_out;
            output.append(out.data(), used);
            deflateEnd(&zs);
            return true;
        }

        std::shared_ptr<SocketWrapper> wrap_socket(int client_fd)
        {
            // Set before the TLS handshake, or a silent client holds this thread forever
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (use_https_ && ssl_ctx_)
            {
                SSL *ssl = SSL_new(ssl_ctx_);
                SSL_set_fd(ssl, client_fd);

                if (SSL_accept(ssl) <= 0)
                {
                    SSL_free(ssl);
                    ::close(client_fd);
                    return nullptr;
                }

                return std::make_shared<SSLSocket>(client_fd, ssl);
            }
            else
            {
                return std::make_shared<PlainSocket>(client_fd);
            }
        }

        // No exception may escape: std::terminate would stop trunk-recorder
        void handle_client(int client_fd)
        {
            // listen() counted this connection before starting the thread
            struct ConnectionGuard
            {
                std::atomic<int> &count;
                ~ConnectionGuard() { --count; }
            } guard{active_connections_};

            std::shared_ptr<SocketWrapper> socket;
            try
            {
                socket = wrap_socket(client_fd);
                if (!socket)
                    return;
                handle_request(socket);
            }
            catch (const std::exception &e)
            {
                BOOST_LOG_TRIVIAL(error) << "[Web Plugin]\tUnhandled exception while serving request: " << e.what();
                if (socket)
                {
                    try
                    {
                        send_error(socket, Request(), 500, "Internal Server Error");
                    }
                    catch (...)
                    {
                    }
                }
            }
            catch (...)
            {
                BOOST_LOG_TRIVIAL(error) << "[Web Plugin]\tUnknown exception while serving request";
                if (socket)
                    socket->close();
            }
        }

        void handle_request(std::shared_ptr<SocketWrapper> socket)
        {
            // Applied before authentication; the largest body is a config file
            static constexpr size_t MAX_HEADER_BYTES = 64 * 1024;
            static constexpr size_t MAX_BODY_BYTES = 1024 * 1024;
            // Bounds the whole request: each read resets the 5 s receive timeout
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            auto expired = [&]
            { return std::chrono::steady_clock::now() > deadline; };

            char buffer[8192];
            std::string request_data;
            ssize_t bytes_read;
            bool headers_complete = false;

            // Read until we have complete headers
            while ((bytes_read = socket->read(buffer, sizeof(buffer))) > 0)
            {
                request_data.append(buffer, static_cast<size_t>(bytes_read));

                size_t header_end = request_data.find("\r\n\r\n");
                if (header_end == std::string::npos)
                {
                    if (request_data.size() > MAX_HEADER_BYTES)
                    {
                        send_error(socket, Request(), 431, "Request Header Fields Too Large");
                        return;
                    }
                    if (expired())
                    {
                        socket->close();
                        return;
                    }
                    continue;
                }
                if (header_end > MAX_HEADER_BYTES)
                {
                    send_error(socket, Request(), 431, "Request Header Fields Too Large");
                    return;
                }
                headers_complete = true;

                // Parse Content-Length to see if we need to read more body data
                size_t content_length = 0;
                if (!parse_content_length(request_data.substr(0, header_end), content_length))
                {
                    send_error(socket, Request(), 400, "Bad Request");
                    return;
                }
                if (content_length > MAX_BODY_BYTES)
                {
                    send_error(socket, Request(), 413, "Request Entity Too Large");
                    return;
                }

                size_t body_start = header_end + 4;
                size_t body_received = request_data.size() - body_start;
                if (body_received > MAX_BODY_BYTES)
                {
                    send_error(socket, Request(), 413, "Request Entity Too Large");
                    return;
                }

                // Continue reading until we have the complete body
                while (body_received < content_length)
                {
                    bytes_read = socket->read(buffer, sizeof(buffer));
                    if (bytes_read <= 0 || expired())
                    {
                        // Timed out or closed before the declared body arrived
                        send_error(socket, Request(), 400, "Bad Request (incomplete body)");
                        return;
                    }
                    request_data.append(buffer, static_cast<size_t>(bytes_read));
                    body_received += static_cast<size_t>(bytes_read);
                }
                break;
            }

            if (!headers_complete)
            {
                socket->close();
                return;
            }

            Request req = parse_request(request_data);
            req.remote_addr = get_client_ip(socket->fd());
            Response res;

            // JSON-only POSTs force a CORS preflight (never approved), so other sites can't post
            if (req.method == "POST" && !is_json_content_type(req))
            {
                send_error(socket, req, 415, "Unsupported Media Type (expected application/json)");
                return;
            }

            // Check if this is an SSE request
            bool is_sse = false;
            for (const auto &sse_path : sse_paths_)
            {
                if (req.path == sse_path)
                {
                    is_sse = true;
                    break;
                }
            }

            if (is_sse)
            {
                // Authenticate SSE/RawStream connections if callback is set
                if (sse_auth_callback_)
                {
                    if (!sse_auth_callback_(req))
                    {
                        res.status = 401;
                        
                        // Only send WWW-Authenticate if Basic Auth was attempted (avoid browser dialogs for session auth)
                        std::string auth_header = get_header_ci(req, "Authorization");
                        bool has_basic_auth = (!auth_header.empty() && auth_header.find("Basic ") == 0);
                        if (has_basic_auth)
                        {
                            res.set_header("WWW-Authenticate", "Basic realm=\"Trunk-Recorder\"");
                        }
                        
                        res.set_content("Unauthorized", "text/plain");
                        send_response(socket, req, res);
                        socket->close();
                        return;
                    }
                }

                handle_sse_client(socket, req);
                return;
            }

            // Find and execute route handler
            auto method_routes = routes_.find(req.method);
            if (method_routes != routes_.end())
            {
                auto handler = method_routes->second.find(req.path);
                if (handler != method_routes->second.end())
                {
                    handler->second(req, res);
                }
                else
                {
                    res.status = 404;
                    res.set_content("Not Found", "text/plain");
                }
            }
            else
            {
                res.status = 405;
                res.set_content("Method Not Allowed", "text/plain");
            }

            send_response(socket, req, res);
            socket->close();
        }

        void handle_sse_client(std::shared_ptr<SocketWrapper> socket, const Request &req)
        {
            const std::string &client_ip = req.remote_addr;

            // Extract username using callback if available, otherwise try Basic Auth
            std::string username = "anonymous";
            if (sse_username_callback_)
            {
                username = sse_username_callback_(req);
            }
            else
            {
                // Fallback to Basic Auth parsing
                std::string auth = get_header_ci(req, "Authorization");
                if (!auth.empty() && auth.substr(0, 6) == "Basic ")
                {
                    std::string decoded = base64_decode(auth.substr(6));
                    size_t colon = decoded.find(':');
                    if (colon != std::string::npos)
                    {
                        username = decoded.substr(0, colon);
                    }
                }
            }

            // Check if this is a raw stream (like Gephi)
            bool is_raw_stream = std::find(raw_stream_paths_.begin(), raw_stream_paths_.end(), req.path) != raw_stream_paths_.end();

            // No Access-Control-Allow-Origin: other sites can't read the streams
            const std::string response = is_raw_stream
                                             ? "HTTP/1.1 200 OK\r\n"
                                               "Content-Type: application/json\r\n"
                                               "Cache-Control: no-cache\r\n"
                                               "Connection: keep-alive\r\n"
                                               "\r\n"
                                             : "HTTP/1.1 200 OK\r\n"
                                               "Content-Type: text/event-stream\r\n"
                                               "Cache-Control: no-cache\r\n"
                                               "Connection: keep-alive\r\n"
                                               "\r\n";

            if (!write_all(*socket, response.data(), response.size()))
            {
                socket->close();
                return;
            }

            auto client = std::make_shared<SSEClient>(socket, client_ip, username, req.path);
            client->is_raw_stream = is_raw_stream;
            client->needs_initial_state = is_raw_stream;

            // Unregister and close on every exit path, including exceptions
            struct Registration
            {
                Server &server;
                std::shared_ptr<SSEClient> client;
                ~Registration()
                {
                    {
                        std::lock_guard<std::mutex> lock(server.sse_mutex_);
                        server.remove_client_locked(client);
                    }
                    BOOST_LOG_TRIVIAL(info) << "[Web Plugin]\t" << (client->is_raw_stream ? "Raw stream" : "SSE") << " session ended - user: " << client->username << " from " << client->client_ip;
                    client->close();
                }
            };
            {
                std::lock_guard<std::mutex> lock(sse_mutex_);
                sse_clients_.push_back(client);
                if (is_raw_stream)
                {
                    raw_stream_clients_.push_back(client);
                }
                sse_client_count_ = sse_clients_.size();
                raw_stream_client_count_ = raw_stream_clients_.size();
            }
            Registration registration{*this, client};

            BOOST_LOG_TRIVIAL(info) << "[Web Plugin]\t" << (is_raw_stream ? "Raw stream" : "SSE") << " session started - user: " << username << " from " << client_ip << " path: " << req.path;

            // Send initial keepalive (only for SSE, not raw streams)
            if (!is_raw_stream)
            {
                client->send_event("", "connected");
            }
            else if (raw_stream_connect_notify_)
            {
                // Notify plugin that a raw stream client connected (just sets a flag)
                raw_stream_connect_notify_();
            }

            // Hold until the client disconnects or is dropped (mark_dead() wakes poll)
            char dummy[1];
            while (running_ && client->connected)
            {
                struct pollfd pfd;
                pfd.fd = socket->fd();
                pfd.events = POLLIN;

                int ret = poll(&pfd, 1, 1000);
                if (ret > 0)
                {
                    // Client sent something (probably closed)
                    ssize_t n = client->read(dummy, 1);
                    if (n <= 0)
                    {
                        break;
                    }
                }
            }
        }

        Request parse_request(const std::string &data)
        {
            Request req;
            std::istringstream stream(data);
            std::string line;

            // Parse request line
            if (std::getline(stream, line))
            {
                size_t pos1 = line.find(' ');
                size_t pos2 = line.find(' ', pos1 + 1);
                if (pos1 != std::string::npos && pos2 != std::string::npos)
                {
                    req.method = line.substr(0, pos1);
                    std::string path_and_query = line.substr(pos1 + 1, pos2 - pos1 - 1);

                    // Parse query string
                    size_t query_pos = path_and_query.find('?');
                    if (query_pos != std::string::npos)
                    {
                        req.path = path_and_query.substr(0, query_pos);
                        std::string query = path_and_query.substr(query_pos + 1);
                        // Parse query parameters
                        std::istringstream query_stream(query);
                        std::string param;
                        while (std::getline(query_stream, param, '&'))
                        {
                            size_t eq_pos = param.find('=');
                            if (eq_pos != std::string::npos)
                            {
                                req.params[param.substr(0, eq_pos)] = param.substr(eq_pos + 1);
                            }
                        }
                    }
                    else
                    {
                        req.path = path_and_query;
                    }
                }
            }

            // Parse headers
            while (std::getline(stream, line) && line != "\r" && !line.empty())
            {
                size_t pos = line.find(':');
                if (pos != std::string::npos)
                {
                    std::string key = line.substr(0, pos);
                    std::string value = line.substr(pos + 1);
                    // Trim whitespace and \r
                    while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
                        value.erase(0, 1);
                    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
                        value.pop_back();
                    req.headers[key] = value;
                }
            }

            // Body (if any)
            size_t header_end = data.find("\r\n\r\n");
            if (header_end != std::string::npos)
            {
                req.body = data.substr(header_end + 4);
            }

            return req;
        }

        void send_response(std::shared_ptr<SocketWrapper> socket, const Request &req, const Response &res)
        {
            Response final_res = res;

            // Gzip large bodies when the client accepts it
            const bool already_encoded = final_res.headers.find("Content-Encoding") != final_res.headers.end();
            if (!already_encoded && final_res.status == 200 && final_res.body.size() >= 16 * 1024 && request_accepts_gzip(req))
            {
                std::string gz;
                if (gzip_compress(final_res.body, gz) && !gz.empty() && gz.size() < final_res.body.size())
                {
                    final_res.body.swap(gz);
                    final_res.headers["Content-Encoding"] = "gzip";
                    // Ensure intermediates cache correctly.
                    if (final_res.headers.find("Vary") == final_res.headers.end())
                    {
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

            for (const auto &header : final_res.headers)
            {
                stream << header.first << ": " << header.second << "\r\n";
            }

            if (!final_res.body.empty() && final_res.headers.find("Content-Length") == final_res.headers.end())
            {
                stream << "Content-Length: " << final_res.body.length() << "\r\n";
            }

            stream << "\r\n";
            stream << final_res.body;

            std::string response = stream.str();
            write_all(*socket, response.data(), response.size());
        }

        static std::string status_text(int status)
        {
            switch (status)
            {
            case 200:
                return "OK";
            case 301:
                return "Moved Permanently";
            case 302:
                return "Found";
            case 304:
                return "Not Modified";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 405:
                return "Method Not Allowed";
            case 413:
                return "Payload Too Large";
            case 415:
                return "Unsupported Media Type";
            case 431:
                return "Request Header Fields Too Large";
            case 429:
                return "Too Many Requests";
            case 500:
                return "Internal Server Error";
            default:
                return "Unknown";
            }
        }

        std::atomic<bool> running_;
        int server_fd_;
        std::thread server_thread_;

        std::map<std::string, std::map<std::string, Handler>> routes_;
        std::vector<std::string> sse_paths_;
        std::vector<std::string> raw_stream_paths_; // Track raw stream paths separately

        std::mutex sse_mutex_;
        std::vector<std::shared_ptr<SSEClient>> sse_clients_;
        // Track raw stream clients (for /graph-stream)
        std::vector<std::shared_ptr<SSEClient>> raw_stream_clients_;
        std::function<void()> raw_stream_connect_notify_;
        std::function<bool(const Request&)> sse_auth_callback_;
        std::function<std::string(const Request&)> sse_username_callback_;

        // Mirrors of the list sizes, readable without sse_mutex_
        std::atomic<size_t> sse_client_count_{0};
        std::atomic<size_t> raw_stream_client_count_{0};

        std::atomic<int> active_connections_{0};
        int max_connections_ = 64;

    public:
        // Return the number of connected raw stream (graphstream) clients (lock-free)
        size_t raw_stream_client_count() const
        {
            return raw_stream_client_count_.load();
        }

    private:
        // Login history tracking
        std::mutex login_history_mutex_;
        std::deque<LoginAttempt> login_history_;
        static const size_t MAX_LOGIN_HISTORY = 50;

        // HTTPS/SSL
        SSL_CTX *ssl_ctx_;
        bool use_https_;
    };

} // namespace httplib

#endif // TR_WEB_HTTPLIB_H
