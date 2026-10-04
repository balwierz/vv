// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// DuckDB database files (.duckdb, .ddb, or a .db with DuckDB's header): one tab
// per table and view, read through libduckdb's C API (optional at build time:
// VV_HAVE_DUCKDB, DuckDB >= 1.5.6 for its stable Arrow export). The database
// is opened read-only, with extension auto-install / auto-load and external
// file access off, so a view in a file cannot make vv read other files or the
// network. A tab's chunks are pages of the table (LIMIT / OFFSET; DuckDB
// preserves insertion order) holding only the columns asked for.

#include "internal.hpp"

#if VV_HAVE_DUCKDB
#include <arrow/c/abi.h>
#include <arrow/c/bridge.h>
#include <duckdb.h>
#endif

// DuckDB's main header starts at byte 8 with "DUCK".
bool is_duckdb_file(const std::string& path) {
    char m[12] = {0};
    std::ifstream f(path, std::ios::binary);
    f.read(m, sizeof m);
    return f.gcount() == (std::streamsize)sizeof m && std::memcmp(m + 8, "DUCK", 4) == 0;
}

#if !VV_HAVE_DUCKDB

std::string open_duckdb_source(const std::string& path, const Config&, std::unique_ptr<TabularSource>*) {
    return "'" + path + "': this vv was built without DuckDB support (libduckdb >= 1.5.6)";
}

#else

namespace {

constexpr int64_t kRowsPerChunk = 65536;

std::string quote_ident(const std::string& s) {
    std::string q = "\"";
    for (char c : s) { if (c == '"') q += '"'; q += c; }
    return q + "\"";
}

// The open database: one connection, used by one query at a time.
struct Db {
    duckdb_database db = nullptr;
    duckdb_connection con = nullptr;
    duckdb_arrow_options opts = nullptr;
    std::mutex mu;
    ~Db() {
        if (opts) duckdb_destroy_arrow_options(&opts);
        if (con) duckdb_disconnect(&con);
        if (db) duckdb_close(&db);
    }
};

std::string take_error(duckdb_error_data* e) {
    std::string msg = duckdb_error_data_has_error(*e) ? duckdb_error_data_message(*e) : "";
    duckdb_destroy_error_data(e);
    return msg;
}

// Run `sql` and return its result as an Arrow table (every column).
arrow::Result<std::shared_ptr<arrow::Table>> query(Db& d, const std::string& sql) {
    std::lock_guard<std::mutex> lk(d.mu);
    duckdb_result res;
    if (duckdb_query(d.con, sql.c_str(), &res) == DuckDBError) {
        std::string why = duckdb_result_error(&res) ? duckdb_result_error(&res) : "query failed";
        duckdb_destroy_result(&res);
        return arrow::Status::IOError(why);
    }
    struct Free { duckdb_result* r; ~Free() { duckdb_destroy_result(r); } } free_res{&res};
    const idx_t n = duckdb_column_count(&res);
    std::vector<duckdb_logical_type> types(n);
    std::vector<const char*> names(n);
    for (idx_t c = 0; c < n; ++c) {
        types[c] = duckdb_column_logical_type(&res, c);
        names[c] = duckdb_column_name(&res, c);
    }
    struct FreeTypes {
        std::vector<duckdb_logical_type>& t;
        ~FreeTypes() { for (auto& x : t) duckdb_destroy_logical_type(&x); }
    } free_types{types};
    struct ArrowSchema cs;
    duckdb_error_data err = duckdb_to_arrow_schema(d.opts, types.data(), names.data(), n, &cs);
    if (auto m = take_error(&err); !m.empty()) return arrow::Status::IOError("DuckDB to Arrow: ", m);
    ARROW_ASSIGN_OR_RAISE(auto schema, arrow::ImportSchema(&cs));
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (;;) {
        duckdb_data_chunk chunk = duckdb_fetch_chunk(res);
        if (!chunk) break;
        struct ArrowArray ca;
        duckdb_error_data e2 = duckdb_data_chunk_to_arrow(d.opts, chunk, &ca);
        duckdb_destroy_data_chunk(&chunk);
        if (auto m = take_error(&e2); !m.empty()) return arrow::Status::IOError("DuckDB to Arrow: ", m);
        ARROW_ASSIGN_OR_RAISE(auto b, arrow::ImportRecordBatch(&ca, schema));
        batches.push_back(std::move(b));
    }
    if (batches.empty()) return arrow::Table::MakeEmpty(schema);
    return arrow::Table::FromRecordBatches(schema, batches);
}

struct TabSpec { std::string schema, name, kind; };   // kind: "table" / "view"

std::string ref(const TabSpec& t) { return quote_ident(t.schema) + "." + quote_ident(t.name); }
std::string label(const TabSpec& t) { return t.schema == "main" ? t.name : t.schema + "." + t.name; }

class DuckSource : public TabularSource {
    std::string path_;
    std::shared_ptr<Db> db_;
    std::shared_ptr<const std::vector<TabSpec>> tabs_;
    size_t idx_;
    std::shared_ptr<arrow::Schema> schema_;
    mutable int64_t total_ = -1;
    mutable arrow::Status status_;
    std::string error_;                  // a tab whose query fails (a view over a blocked file, ...)
    const TabSpec& spec() const { return (*tabs_)[idx_]; }

public:
    DuckSource(std::string path, std::shared_ptr<Db> db, std::shared_ptr<const std::vector<TabSpec>> tabs,
               size_t idx, std::shared_ptr<arrow::Schema> schema)
        : path_(std::move(path)), db_(std::move(db)), tabs_(std::move(tabs)), idx_(idx),
          schema_(std::move(schema)) {}

    static std::string open(const std::string& path, std::shared_ptr<Db> db,
                            std::shared_ptr<const std::vector<TabSpec>> tabs, size_t idx,
                            std::unique_ptr<TabularSource>* out) {
        auto t = query(*db, "SELECT * FROM " + ref((*tabs)[idx]) + " LIMIT 0");
        if (!t.ok()) {
            // Kept as a tab that shows why (first line of DuckDB's message),
            // so the other tabs open and an export of this one fails.
            std::string why = t.status().message();
            why = why.substr(0, why.find('\n'));
            if (idx == 0) return "'" + path + "': " + label((*tabs)[idx]) + ": " + why;
            auto s = std::make_unique<DuckSource>(path, std::move(db), std::move(tabs), idx,
                                                  arrow::schema({arrow::field("error", arrow::utf8())}));
            s->error_ = why;
            s->status_ = arrow::Status::IOError(why);
            s->total_ = 1;
            *out = std::move(s);
            return "";
        }
        *out = std::make_unique<DuckSource>(path, std::move(db), std::move(tabs), idx, (*t)->schema());
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        if (total_ < 0 && status_.ok()) {
            auto t = query(*db_, "SELECT count(*) FROM " + ref(spec()));
            if (!t.ok()) { status_ = t.status(); return 0; }
            auto c = (*t)->column(0)->chunk(0);
            total_ = c->length() > 0 && c->type_id() == arrow::Type::INT64
                         ? std::static_pointer_cast<arrow::Int64Array>(c)->Value(0) : 0;
        }
        return std::max<int64_t>(total_, 0);
    }
    int num_chunks() const override {
        return (int)((total_rows() + kRowsPerChunk - 1) / kRowsPerChunk);
    }
    ChunkMeta chunk_meta(int i) const override {
        const int64_t first = (int64_t)i * kRowsPerChunk;
        return {first, std::max<int64_t>(0, std::min(kRowsPerChunk, total_rows() - first))};
    }
    arrow::Status read_status() const override { return status_; }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        if (!error_.empty()) {
            arrow::StringBuilder b;
            ARROW_RETURN_NOT_OK(b.Append(error_));
            std::shared_ptr<arrow::Array> a;
            ARROW_RETURN_NOT_OK(b.Finish(&a));
            if (cols.empty()) *out = arrow::Table::Make(arrow::schema({}), arrow::ArrayVector{}, 1);
            else *out = arrow::Table::Make(schema_, {a});
            return arrow::Status::OK();
        }
        std::string sel;
        for (int c : cols) {
            if (c < 0 || c >= schema_->num_fields()) return arrow::Status::IndexError("column ", c);
            sel += (sel.empty() ? "" : ", ") + quote_ident(schema_->field(c)->name());
        }
        if (sel.empty()) {                    // no columns: just the row count
            *out = arrow::Table::Make(arrow::schema({}), arrow::ArrayVector{}, chunk_meta(i).num_rows);
            return arrow::Status::OK();
        }
        auto t = query(*db_, "SELECT " + sel + " FROM " + ref(spec()) + " LIMIT " + std::to_string(kRowsPerChunk) +
                                 " OFFSET " + std::to_string((int64_t)i * kRowsPerChunk));
        if (!t.ok()) {
            if (status_.ok()) status_ = t.status();
            return t.status();
        }
        // Keep the source schema's field names / types (a query of a subset
        // returns the same columns).
        arrow::FieldVector fields;
        for (int c : cols) fields.push_back(schema_->field(c));
        auto one = (*t)->CombineChunks();
        if (!one.ok()) return one.status();
        *out = arrow::Table::Make(arrow::schema(fields), (*one)->columns(), (*one)->num_rows());
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    std::string tab_label() const override { return label(spec()); }
    std::string footer() const override {
        std::string s = "Format: DuckDB  |  " + std::string(spec().kind == "view" ? "View: " : "Table: ") +
                        label(spec());
        const int64_t n = total_rows();
        if (!error_.empty()) s += "  |  error: " + error_;
        else if (status_.ok()) s += "  |  Rows: " + std::to_string(n);
        if (idx_ == 0 && tabs_->size() > 1) s += "  |  +" + std::to_string(tabs_->size() - 1) + " more tab(s)";
        return s;
    }
    std::vector<std::unique_ptr<TabularSource>> expand_tabs() const override {
        std::vector<std::unique_ptr<TabularSource>> v;
        if (idx_ != 0) return v;
        for (size_t k = 1; k < tabs_->size(); ++k) {
            std::unique_ptr<TabularSource> s;
            if (auto e = open(path_, db_, tabs_, k, &s); !e.empty()) {
                std::fprintf(stderr, "vv: %s\n", e.c_str());
                continue;
            }
            v.push_back(std::move(s));
        }
        return v;
    }
};

}  // namespace

std::string open_duckdb_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    auto db = std::make_shared<Db>();
    duckdb_config config;
    if (duckdb_create_config(&config) == DuckDBError) return "'" + path + "': DuckDB: cannot create a config";
    duckdb_set_config(config, "access_mode", "READ_ONLY");
    duckdb_set_config(config, "autoinstall_known_extensions", "false");
    duckdb_set_config(config, "autoload_known_extensions", "false");
    duckdb_set_config(config, "enable_external_access", "false");
    duckdb_set_config(config, "threads", std::to_string(std::max(1, effective_threads(cfg))).c_str());
    char* err = nullptr;
    const duckdb_state st = duckdb_open_ext(path.c_str(), &db->db, config, &err);
    duckdb_destroy_config(&config);
    if (st == DuckDBError) {
        std::string why = err ? err : "cannot open";
        if (err) duckdb_free(err);
        return "'" + path + "': DuckDB " + std::string(duckdb_library_version()) + ": " + why;
    }
    if (duckdb_connect(db->db, &db->con) == DuckDBError) return "'" + path + "': DuckDB: cannot connect";
    duckdb_connection_get_arrow_options(db->con, &db->opts);
    // Tables, then views, of the file's own database (not the temp / system ones).
    auto cat = query(*db,
        "SELECT * FROM ("
        "SELECT schema_name, table_name AS name, 'table' AS kind FROM duckdb_tables() "
        "WHERE database_name = current_database() AND NOT internal AND NOT temporary "
        "UNION ALL "
        "SELECT schema_name, view_name, 'view' FROM duckdb_views() "
        "WHERE database_name = current_database() AND NOT internal AND NOT temporary"
        ") ORDER BY kind, schema_name <> 'main', schema_name, name");
    if (!cat.ok()) return "'" + path + "': DuckDB: " + cat.status().message();
    auto tabs = std::make_shared<std::vector<TabSpec>>();
    auto combined = (*cat)->CombineChunks();
    if (!combined.ok()) return "'" + path + "': DuckDB: " + combined.status().message();
    auto t = *combined;
    for (int64_t r = 0; r < t->num_rows(); ++r)
        tabs->push_back({cell_to_string(*t->column(0)->chunk(0), r), cell_to_string(*t->column(1)->chunk(0), r),
                         cell_to_string(*t->column(2)->chunk(0), r)});
    if (tabs->empty()) return "'" + path + "': the DuckDB database has no tables or views";
    return DuckSource::open(path, std::move(db), std::move(tabs), 0, out);
}

#endif
