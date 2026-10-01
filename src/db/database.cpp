#include <hoardor/db/database.hpp>

#include <sqlite3.h>

#include <utility>

namespace hoardor::db {

namespace {

Error error_from(sqlite3* db, int rc) {
    return Error{rc, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)};
}

}

// ---------------------------------------------------------------- Statement

Statement::Statement(Statement&& other) noexcept
    : db_(other.db_), stmt_(std::exchange(other.stmt_, nullptr)), bind_error_(other.bind_error_) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        db_ = other.db_;
        stmt_ = std::exchange(other.stmt_, nullptr);
        bind_error_ = other.bind_error_;
    }
    return *this;
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

void Statement::note_bind(int rc) {
    if (rc != SQLITE_OK && bind_error_ == 0) bind_error_ = rc;
}

Statement& Statement::bind(int index, std::int64_t value) {
    note_bind(sqlite3_bind_int64(stmt_, index, value));
    return *this;
}

Statement& Statement::bind(int index, double value) {
    note_bind(sqlite3_bind_double(stmt_, index, value));
    return *this;
}

Statement& Statement::bind(int index, std::string_view text) {
    // SQLITE_TRANSIENT: SQLite copies the text, so the caller's buffer may go away.
    note_bind(sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    return *this;
}

Statement& Statement::bind_null(int index) {
    note_bind(sqlite3_bind_null(stmt_, index));
    return *this;
}

Result<bool> Statement::step() {
    if (bind_error_ != 0) {
        const int rc = std::exchange(bind_error_, 0);
        return std::unexpected(Error{rc, std::string("bind failed: ") + sqlite3_errstr(rc)});
    }
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return std::unexpected(error_from(db_, rc));
}

Result<void> Statement::run() {
    while (true) {
        auto row = step();
        if (!row) return std::unexpected(row.error());
        if (!*row) return {};
    }
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    bind_error_ = 0;
}

std::int64_t Statement::column_int64(int column) const { return sqlite3_column_int64(stmt_, column); }

double Statement::column_double(int column) const { return sqlite3_column_double(stmt_, column); }

std::string Statement::column_text(int column) const {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, column));
    const int bytes = sqlite3_column_bytes(stmt_, column);
    return text ? std::string(text, static_cast<std::size_t>(bytes)) : std::string();
}

bool Statement::column_is_null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

// ---------------------------------------------------------------- Database

Result<Database> Database::open_uri(const std::string& name, int flags, bool wal, Options options) {
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(name.c_str(), &raw, flags, nullptr);
    Database database(raw);  // owns the handle even when opening failed
    if (rc != SQLITE_OK) return std::unexpected(error_from(raw, rc));

    sqlite3_extended_result_codes(raw, 1);
    sqlite3_busy_timeout(raw, options.busy_timeout_ms);
    if (wal) {
        // WAL: readers never wait for the writer. NORMAL sync is safe with WAL and much faster than FULL.
        if (auto r = database.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"); !r) return std::unexpected(r.error());
    }
    if (auto r = database.exec("PRAGMA foreign_keys=ON;"); !r) return std::unexpected(r.error());
    return database;
}

Result<Database> Database::open(const std::filesystem::path& file, Options options) {
    const std::u8string utf8 = file.u8string();
    return open_uri(std::string(utf8.begin(), utf8.end()), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, true, options);
}

Result<Database> Database::open_in_memory(Options options) {
    return open_uri(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, false, options);
}

Database::Database(Database&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        sqlite3_close_v2(db_);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Database::~Database() { sqlite3_close_v2(db_); }

Result<void> Database::exec(std::string_view sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return {};
    Error error{rc, message ? message : sqlite3_errstr(rc)};
    sqlite3_free(message);
    return std::unexpected(std::move(error));
}

Result<Statement> Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return std::unexpected(error_from(db_, rc));
    }
    return Statement(db_, stmt);
}

std::int64_t Database::last_insert_id() const { return sqlite3_last_insert_rowid(db_); }

std::int64_t Database::changes() const { return sqlite3_changes64(db_); }

// ---------------------------------------------------------------- Transaction

Result<Transaction> Transaction::begin(Database& database) {
    // IMMEDIATE takes the write lock now, so two writers wait (busy_timeout)
    // instead of failing halfway through.
    if (auto r = database.exec("BEGIN IMMEDIATE"); !r) return std::unexpected(r.error());
    return Transaction(database);
}

Transaction::Transaction(Transaction&& other) noexcept : database_(std::exchange(other.database_, nullptr)) {}

Transaction::~Transaction() {
    if (database_) (void)database_->exec("ROLLBACK");
}

Result<void> Transaction::commit() {
    if (!database_) return std::unexpected(Error{SQLITE_MISUSE, "transaction already finished"});
    auto r = database_->exec("COMMIT");
    if (r) database_ = nullptr;
    return r;
}

// ---------------------------------------------------------------- Migrations

Result<void> migrate(Database& database, std::string_view component, std::span<const Migration> migrations) {
    if (auto r = database.exec("CREATE TABLE IF NOT EXISTS db_migrations ("
                               "component TEXT PRIMARY KEY, version INTEGER NOT NULL)");
        !r) {
        return r;
    }

    std::int64_t current = 0;
    {
        auto select = database.prepare("SELECT version FROM db_migrations WHERE component = ?");
        if (!select) return std::unexpected(select.error());
        select->bind(1, component);
        auto row = select->step();
        if (!row) return std::unexpected(row.error());
        if (*row) current = select->column_int64(0);
    }

    for (const Migration& migration : migrations) {
        if (migration.version <= current) continue;
        auto tx = Transaction::begin(database);
        if (!tx) return std::unexpected(tx.error());
        if (auto r = database.exec(migration.sql); !r) {
            return std::unexpected(Error{r.error().code, std::string(component) + " migration " +
                                                             std::to_string(migration.version) + ": " + r.error().message});
        }
        auto record = database.prepare("INSERT INTO db_migrations (component, version) VALUES (?, ?) "
                                       "ON CONFLICT (component) DO UPDATE SET version = excluded.version");
        if (!record) return std::unexpected(record.error());
        record->bind(1, component).bind(2, static_cast<std::int64_t>(migration.version));
        if (auto r = record->run(); !r) return r;
        if (auto r = tx->commit(); !r) return r;
        current = migration.version;
    }
    return {};
}

}
