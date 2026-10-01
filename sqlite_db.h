// sqlite_db.h - small RAII wrapper for the vendored SQLite (third_party/sqlite)
// Depends only on <sqlite3.h>. Errors are returned, with last_error(); nothing throws.

#ifndef TR_WEB_SQLITE_DB_H
#define TR_WEB_SQLITE_DB_H

#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace sqlite_db
{
    // A prepared statement. Bind parameters are 1-based, columns 0-based (as in SQLite).
    class Statement
    {
    public:
        Statement() = default;
        Statement(sqlite3 *db, const std::string &sql)
        {
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            {
                error_ = sqlite3_errmsg(db);
                stmt_ = nullptr;
            }
        }
        ~Statement() { sqlite3_finalize(stmt_); }
        Statement(const Statement &) = delete;
        Statement &operator=(const Statement &) = delete;
        Statement(Statement &&o) noexcept : stmt_(o.stmt_), error_(std::move(o.error_)) { o.stmt_ = nullptr; }
        Statement &operator=(Statement &&o) noexcept
        {
            if (this != &o)
            {
                sqlite3_finalize(stmt_);
                stmt_ = o.stmt_;
                error_ = std::move(o.error_);
                o.stmt_ = nullptr;
            }
            return *this;
        }

        bool ok() const { return stmt_ != nullptr; }
        const std::string &error() const { return error_; }

        Statement &bind(int i, int64_t v) { sqlite3_bind_int64(stmt_, i, v); return *this; }
        Statement &bind(int i, int v) { sqlite3_bind_int64(stmt_, i, v); return *this; }
        Statement &bind(int i, bool v) { sqlite3_bind_int(stmt_, i, v ? 1 : 0); return *this; }
        Statement &bind(int i, double v) { sqlite3_bind_double(stmt_, i, v); return *this; }
        Statement &bind(int i, const std::string &v) { sqlite3_bind_text(stmt_, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); return *this; }
        Statement &bind_null(int i) { sqlite3_bind_null(stmt_, i); return *this; }

        // SQLITE_ROW, SQLITE_DONE or an error code
        int step() { return sqlite3_step(stmt_); }

        // Run a statement that returns no rows, then reset it for reuse
        bool run()
        {
            int rc = sqlite3_step(stmt_);
            sqlite3_reset(stmt_);
            sqlite3_clear_bindings(stmt_);
            return rc == SQLITE_DONE;
        }

        void reset()
        {
            sqlite3_reset(stmt_);
            sqlite3_clear_bindings(stmt_);
        }

        int64_t col_int(int c) const { return sqlite3_column_int64(stmt_, c); }
        double col_double(int c) const { return sqlite3_column_double(stmt_, c); }
        bool col_null(int c) const { return sqlite3_column_type(stmt_, c) == SQLITE_NULL; }
        std::string col_text(int c) const
        {
            const unsigned char *t = sqlite3_column_text(stmt_, c);
            return t ? std::string(reinterpret_cast<const char *>(t), sqlite3_column_bytes(stmt_, c)) : std::string();
        }

    private:
        sqlite3_stmt *stmt_ = nullptr;
        std::string error_;
    };

    class Database
    {
    public:
        Database() = default;
        ~Database() { close(); }
        Database(const Database &) = delete;
        Database &operator=(const Database &) = delete;

        // Open (creating if needed) with WAL journaling and full sync: a committed transaction
        // survives power loss. Returns false if the file can't be opened or isn't a database.
        bool open(const std::string &path)
        {
            close();
            if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            {
                error_ = db_ ? sqlite3_errmsg(db_) : "out of memory";
                close();
                return false;
            }
            sqlite3_busy_timeout(db_, 5000);
            // The first real read happens here, so a file that isn't a database fails now
            if (!exec("PRAGMA journal_mode=WAL") || !exec("PRAGMA synchronous=FULL"))
            {
                close();
                return false;
            }
            return true;
        }

        void close()
        {
            if (db_)
            {
                sqlite3_close_v2(db_);
                db_ = nullptr;
            }
        }

        bool is_open() const { return db_ != nullptr; }
        sqlite3 *handle() const { return db_; }
        const std::string &last_error() const { return error_; }

        bool exec(const std::string &sql)
        {
            char *msg = nullptr;
            if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK)
            {
                error_ = msg ? msg : sqlite3_errmsg(db_);
                sqlite3_free(msg);
                return false;
            }
            return true;
        }

        Statement prepare(const std::string &sql)
        {
            Statement s(db_, sql);
            if (!s.ok())
                error_ = s.error();
            return s;
        }

        // Single integer result of a query (e.g. PRAGMA user_version); `fallback` on error
        int64_t query_int(const std::string &sql, int64_t fallback = -1)
        {
            Statement s = prepare(sql);
            if (!s.ok() || s.step() != SQLITE_ROW)
                return fallback;
            return s.col_int(0);
        }

        std::string query_text(const std::string &sql)
        {
            Statement s = prepare(sql);
            if (!s.ok() || s.step() != SQLITE_ROW)
                return std::string();
            return s.col_text(0);
        }

        // Online, consistent copy of the whole database into `dest_path` (overwritten)
        bool backup_to(const std::string &dest_path)
        {
            sqlite3 *dest = nullptr;
            if (sqlite3_open_v2(dest_path.c_str(), &dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
            {
                error_ = dest ? sqlite3_errmsg(dest) : "out of memory";
                sqlite3_close_v2(dest);
                return false;
            }
            sqlite3_backup *b = sqlite3_backup_init(dest, "main", db_, "main");
            bool ok = false;
            if (b)
            {
                ok = sqlite3_backup_step(b, -1) == SQLITE_DONE;
                sqlite3_backup_finish(b);
            }
            if (!ok)
                error_ = sqlite3_errmsg(dest);
            // The copy inherits WAL mode; a backup should be one self-contained file
            if (ok)
                sqlite3_exec(dest, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
            sqlite3_close_v2(dest);
            return ok;
        }

    private:
        sqlite3 *db_ = nullptr;
        std::string error_;
    };

    // BEGIN IMMEDIATE ... COMMIT, rolled back unless commit() succeeds
    class Transaction
    {
    public:
        explicit Transaction(Database &db) : db_(db) { active_ = db_.exec("BEGIN IMMEDIATE"); }
        ~Transaction()
        {
            if (active_)
                db_.exec("ROLLBACK");
        }
        bool began() const { return active_; }
        bool commit()
        {
            if (!active_)
                return false;
            bool ok = db_.exec("COMMIT");
            active_ = !ok; // a failed COMMIT still needs the ROLLBACK
            return ok;
        }

    private:
        Database &db_;
        bool active_ = false;
    };
}

#endif // TR_WEB_SQLITE_DB_H
