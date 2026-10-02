// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// SQLite databases, one tab per table.

#include "internal.hpp"

// ── SQLite source ─────────────────────────────────────────────────────────────
//
// One SqliteSource = one table in a SQLite file. Multi-table databases
// expand into one tab per table via SqliteSource::open_sibling_tables()
// which shares the underlying sqlite3 handle through a shared_ptr.
//
// Type-affinity mapping follows the SQLite documentation's substring
// rules (https://sqlite.org/datatype3.html#determination_of_column_affinity):
//   contains "INT"               → int64
//   contains CHAR/CLOB/TEXT      → string
//   contains BLOB / blank        → binary
//   contains REAL/FLOA/DOUB      → double
//   anything else (NUMERIC, …)   → string
// Column accessors (sqlite3_column_int64 / _double / _text / _blob)
// gracefully convert at read time, so a declared-INT column that
// actually holds a string still produces a sensible integer (or 0).
//
// The "anything else" bucket (NUMERIC affinity) covers NUMERIC, DECIMAL,
// DATE, DATETIME, TIMESTAMP, BOOLEAN, … — declarations that may legitimately
// store text (ISO dates), 64-bit integers beyond a double's 2^53 exact range,
// or booleans. Reading those through sqlite3_column_double silently corrupts
// them ('2026-06-10' → 2026.0; a big id rounded). SQLite is dynamically typed
// and such a column can hold mixed storage classes, so the only lossless fixed
// Arrow type is string: sqlite3_column_text renders each value faithfully.

static arrow::Type::type sqlite_type_to_arrow(const std::string& declared) {
    std::string u = declared;
    for (auto& c : u) c = (char)std::toupper((unsigned char)c);
    if (u.find("INT")  != std::string::npos) return arrow::Type::INT64;
    if (u.find("CHAR") != std::string::npos
     || u.find("CLOB") != std::string::npos
     || u.find("TEXT") != std::string::npos) return arrow::Type::STRING;
    if (u.find("BLOB") != std::string::npos || u.empty()) return arrow::Type::BINARY;
    if (u.find("REAL") != std::string::npos
     || u.find("FLOA") != std::string::npos
     || u.find("DOUB") != std::string::npos) return arrow::Type::DOUBLE;
    return arrow::Type::STRING;   // NUMERIC affinity — preserve verbatim
}
// Type id -> Arrow type, for the three places that carry a type id around
// instead of a DataType: the SQLite column mapper, the NumPy dtype mapper and
// ExpandedSource's declared-key schema.
//
// This MUST cover every id its callers can produce. It used to handle only
// INT64/DOUBLE/BINARY and fall through to utf8(), which meant the schema said
// `string` while the chunk carried the real array — an inconsistency Arrow does
// not check on the write path. `--parquet` failed loudly, but `--arrow` exited
// 0 and wrote an IPC file nothing could read back ("buffer_index out of
// range"), for 7 of the 9 NumPy dtypes and for every VCF Flag INFO key.
std::shared_ptr<arrow::DataType> arrow_type_for_id(arrow::Type::type t) {
    switch (t) {
        case arrow::Type::BOOL:   return arrow::boolean();
        case arrow::Type::INT8:   return arrow::int8();
        case arrow::Type::INT16:  return arrow::int16();
        case arrow::Type::INT32:  return arrow::int32();
        case arrow::Type::INT64:  return arrow::int64();
        case arrow::Type::UINT8:  return arrow::uint8();
        case arrow::Type::UINT16: return arrow::uint16();
        case arrow::Type::UINT32: return arrow::uint32();
        case arrow::Type::UINT64: return arrow::uint64();
        case arrow::Type::FLOAT:  return arrow::float32();
        case arrow::Type::DOUBLE: return arrow::float64();
        case arrow::Type::BINARY: return arrow::binary();
        default:                  return arrow::utf8();
    }
}

// Quote a SQLite identifier (table name), escaping any embedded double quote by
// doubling it, per SQL. Table names come from the database's own catalog, so a
// table created as `a"b` would otherwise build the malformed — and, for an
// untrusted .sqlite, injectable — SQL `"a"b"`.
static std::string sqlite_quote_ident(const std::string& id) {
    std::string out = "\"";
    for (char c : id) { if (c == '"') out += '"'; out += c; }
    out += '"';
    return out;
}

class SqliteSource : public TabularSource {
    std::string                              path_;
    std::string                              table_;
    std::shared_ptr<sqlite3>                 db_;
    std::shared_ptr<arrow::Schema>           schema_;
    std::vector<arrow::Type::type>           col_types_;
    mutable int64_t                          total_rows_     = -1;  // lazy COUNT(*)
    mutable bool                             total_counted_  = false;
    std::vector<std::string>                 sibling_tables_;  // others in same DB

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>             batch_first_row_;
    mutable std::vector<int64_t>             batch_num_rows_;   // kept after eviction
    mutable int64_t                          rows_so_far_ = 0;
    mutable bool                             all_read_   = false;
    mutable bool                             retain_all_  = false;
    mutable bool                             evicted_any_ = false;
    mutable arrow::Status                    read_status_;   // sticky stream error
    mutable sqlite3_stmt*                    stmt_       = nullptr;

    static constexpr int BATCH_SIZE = 4096;

    static std::shared_ptr<arrow::ArrayBuilder> make_builder(arrow::Type::type t) {
        switch (t) {
            case arrow::Type::INT64:  return std::make_shared<arrow::Int64Builder>();
            case arrow::Type::DOUBLE: return std::make_shared<arrow::DoubleBuilder>();
            case arrow::Type::BINARY: return std::make_shared<arrow::BinaryBuilder>();
            default:                  return std::make_shared<arrow::StringBuilder>();
        }
    }

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();
        int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        std::vector<std::shared_ptr<arrow::ArrayBuilder>> builders;
        builders.reserve(col_types_.size());
        for (auto t : col_types_) builders.push_back(make_builder(t));

        int count = 0;
        while (count < cap) {
            int rc = sqlite3_step(stmt_);
            if (rc == SQLITE_DONE) { all_read_ = true; break; }
            if (rc != SQLITE_ROW) {
                // A step error mid-scan: record it stickily and stop so
                // ensure()'s (void)advance() loop can't spin forever.
                read_status_ = arrow::Status::IOError(
                    std::string("SQLite step error: ") + sqlite3_errmsg(db_.get()));
                all_read_ = true;
                return read_status_;
            }
            for (int i = 0; i < (int)col_types_.size(); ++i) {
                if (sqlite3_column_type(stmt_, i) == SQLITE_NULL) {
                    ARROW_RETURN_NOT_OK(builders[i]->AppendNull());
                    continue;
                }
                switch (col_types_[i]) {
                    case arrow::Type::INT64: {
                        auto b = static_cast<arrow::Int64Builder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(sqlite3_column_int64(stmt_, i)));
                        break;
                    }
                    case arrow::Type::DOUBLE: {
                        auto b = static_cast<arrow::DoubleBuilder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(sqlite3_column_double(stmt_, i)));
                        break;
                    }
                    case arrow::Type::BINARY: {
                        const void* p = sqlite3_column_blob(stmt_, i);
                        int n = sqlite3_column_bytes(stmt_, i);
                        auto b = static_cast<arrow::BinaryBuilder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(reinterpret_cast<const uint8_t*>(p), n));
                        break;
                    }
                    default: {
                        const unsigned char* p = sqlite3_column_text(stmt_, i);
                        int n = sqlite3_column_bytes(stmt_, i);
                        auto b = static_cast<arrow::StringBuilder*>(builders[i].get());
                        ARROW_RETURN_NOT_OK(b->Append(
                            reinterpret_cast<const char*>(p), n));
                        break;
                    }
                }
            }
            ++count;
        }
        if (count == 0) return arrow::Status::OK();

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        arrays.reserve(builders.size());
        for (auto& b : builders) {
            std::shared_ptr<arrow::Array> a;
            ARROW_RETURN_NOT_OK(b->Finish(&a));
            arrays.push_back(a);
        }
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_,
                      retain_all_, evicted_any_,
                      arrow::RecordBatch::Make(schema_, count, arrays));
        return arrow::Status::OK();
    }

    static std::string build_one(const std::string& path,
                                  std::shared_ptr<sqlite3> db,
                                  const std::string& table,
                                  const std::vector<std::string>& siblings,
                                  std::unique_ptr<SqliteSource>* out) {
        auto self = std::make_unique<SqliteSource>();
        self->path_  = path;
        self->table_ = table;
        self->db_    = db;
        self->sibling_tables_ = siblings;

        // Schema from PRAGMA table_info. Quote the table name (escaping embedded
        // quotes) in case it contains spaces or special chars.
        std::string q = "PRAGMA table_info(" + sqlite_quote_ident(table) + ")";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db.get(), q.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return std::string("SQLite prepare failed: ") + sqlite3_errmsg(db.get());
        }
        arrow::FieldVector fields;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* name = (const char*)sqlite3_column_text(st, 1);
            const char* type = (const char*)sqlite3_column_text(st, 2);
            int notnull      = sqlite3_column_int(st, 3);
            auto at = sqlite_type_to_arrow(type ? type : "");
            fields.push_back(arrow::field(name ? name : "",
                                          arrow_type_for_id(at), notnull == 0));
            self->col_types_.push_back(at);
        }
        sqlite3_finalize(st);
        if (fields.empty())
            return "SQLite table '" + table + "' has no columns (or doesn't exist)";
        self->schema_ = arrow::schema(fields);

        // Total row count is computed lazily (see total_rows()) — a COUNT(*) is
        // a full table scan, wasteful for an `-n` preview or a thumbnail that
        // never asks for the total.

        // Prepare the streaming SELECT and read the first batch.
        std::string sq = "SELECT * FROM " + sqlite_quote_ident(table);
        if (sqlite3_prepare_v2(db.get(), sq.c_str(), -1, &self->stmt_, nullptr) != SQLITE_OK)
            return std::string("SQLite prepare failed: ") + sqlite3_errmsg(db.get());

        // Rows are read on demand, so read_first reads only the rows it
        // returns; a step error is reported by the first read (read_status).
        *out = std::move(self);
        return "";
    }

public:
    ~SqliteSource() {
        if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
    }

    // Open the first table of a SQLite file as a SqliteSource. Remembers
    // the other user-table names so the caller can later request siblings
    // via open_sibling_tables() (for the multi-tab TUI path).
    static std::string open_first(const std::string& path,
                                   std::unique_ptr<SqliteSource>* out) {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(path.c_str(), &raw,
                            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            std::string err = raw ? sqlite3_errmsg(raw) : "(null handle)";
            if (raw) sqlite3_close(raw);
            return "Cannot open '" + path + "' as SQLite: " + err;
        }
        std::shared_ptr<sqlite3> db(raw, [](sqlite3* p){ sqlite3_close(p); });

        // List user tables (skip the sqlite_* internal tables).
        sqlite3_stmt* lst = nullptr;
        if (sqlite3_prepare_v2(db.get(),
                "SELECT name FROM sqlite_master "
                "WHERE type='table' AND name NOT LIKE 'sqlite_%' "
                "ORDER BY name", -1, &lst, nullptr) != SQLITE_OK) {
            return std::string("SQLite list-tables failed: ") + sqlite3_errmsg(db.get());
        }
        std::vector<std::string> tables;
        while (sqlite3_step(lst) == SQLITE_ROW)
            tables.emplace_back((const char*)sqlite3_column_text(lst, 0));
        sqlite3_finalize(lst);
        if (tables.empty())
            return "'" + path + "': SQLite database has no user tables";

        std::vector<std::string> siblings(tables.begin() + 1, tables.end());
        return build_one(path, db, tables.front(), siblings, out);
    }

    // Build SqliteSource instances for every table OTHER than the one
    // returned by open_first(). The shared sqlite3 handle is reused.
    std::vector<std::unique_ptr<TabularSource>> open_sibling_tables() const {
        std::vector<std::unique_ptr<TabularSource>> out;
        for (const auto& t : sibling_tables_) {
            std::unique_ptr<SqliteSource> s;
            std::string err = build_one(path_, db_, t, {}, &s);
            if (!err.empty()) {
                std::fprintf(stderr, "vv: SQLite table '%s': %s\n",
                             t.c_str(), err.c_str());
                continue;
            }
            out.push_back(std::move(s));
        }
        return out;
    }
    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        return open_sibling_tables();
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        // Once we've streamed the whole table the exact count is free.
        if (all_read_) return rows_so_far_;
        // Otherwise run COUNT(*) once, on first ask — so a preview / thumbnail
        // that never needs the total doesn't trigger a full table scan.
        if (!total_counted_) {
            total_counted_ = true;
            std::string cq = "SELECT COUNT(*) FROM " + sqlite_quote_ident(table_);
            sqlite3_stmt* st = nullptr;
            if (db_ && sqlite3_prepare_v2(db_.get(), cq.c_str(), -1, &st,
                                          nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW)
                    total_rows_ = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
            }
        }
        return total_rows_ >= 0 ? total_rows_ : (all_read_ ? rows_so_far_ : -1);
    }
    int     num_chunks() const override { return (int)batches_.size(); }
    arrow::Status read_status() const override { return read_status_; }
    ChunkMeta chunk_meta(int i) const override {
        return {batch_first_row_[i], batch_num_rows_[i]};
    }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i) (void)advance();
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError(
                "chunk ", i, " was released by the streaming window");
        *out = batch_slice_to_table(*batches_[i], col_indices, schema_);
        return arrow::Status::OK();
    }
    // The first `rows` rows, reading only as many records as that needs
    // (open() read one): a batch not yet read is read capped at the
    // shortfall, not as a whole 4096-record / byte-budget batch. Each chunk
    // is collected as soon as it exists, as the base read_first does.
    arrow::Status read_first(int64_t rows, const std::vector<int>& col_indices,
                              std::shared_ptr<arrow::Table>* out) override {
        std::shared_ptr<arrow::Table> acc;
        for (int c = 0; !acc || acc->num_rows() < rows; ++c) {
            const int64_t got = acc ? acc->num_rows() : 0;
            while (!all_read_ && c >= (int)batches_.size())
                ARROW_RETURN_NOT_OK(advance(rows - got));
            if (c >= (int)batches_.size()) break;
            std::shared_ptr<arrow::Table> chunk;
            ARROW_RETURN_NOT_OK(read_chunk(c, col_indices, &chunk));
            if (!acc) { acc = chunk; continue; }
            ARROW_ASSIGN_OR_RAISE(acc, arrow::ConcatenateTables({acc, chunk}));
        }
        if (acc && acc->num_rows() > rows) acc = acc->Slice(0, rows);
        *out = acc;
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    std::string tab_label() const override { return table_; }
    std::string footer() const override {
        std::string s = "Format: SQLite  |  Table: " + table_;
        // total_rows() runs the lazy COUNT(*); the footer is a "real" view
        // (table / TUI) where the count is wanted, unlike a thumbnail.
        int64_t tr = total_rows();
        if (tr >= 0) s += "  |  Rows: " + std::to_string(tr);
        if (!sibling_tables_.empty())
            s += "  |  +" + std::to_string(sibling_tables_.size()) + " more table(s)";
        return s;
    }
};

std::string open_sqlite_source(const std::string& path, std::unique_ptr<TabularSource>* out) {
    std::unique_ptr<SqliteSource> s;
    std::string e = SqliteSource::open_first(path, &s);
    if (e.empty()) *out = std::move(s);
    return e;
}
// The other tables of the database `src` was opened from (none when it is not
// a SQLite table).
std::vector<std::unique_ptr<TabularSource>> sqlite_sibling_tables(TabularSource* src) {
    std::vector<std::unique_ptr<TabularSource>> v;
    if (auto* sq = dynamic_cast<SqliteSource*>(src))
        for (auto& s : sq->open_sibling_tables()) v.push_back(std::move(s));
    return v;
}
