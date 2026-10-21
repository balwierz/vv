// SPDX-License-Identifier: MIT
#include "formats/store.hpp"

#include <cstdio>

namespace {
using namespace h5v;

class CountingStore final : public Store {
public:
    int64_t         entries_per_row = 3;
    mutable int64_t matrix_values = 0, matrix_indices = 0, largest_read = 0;
    NodeKind        kind(const std::string& p) const override {
        if (p == "/" || p == "/X" || p == "/obs" || p == "/var") return NodeKind::Group;
        return info(p) ? NodeKind::Array : NodeKind::Missing;
    }
    std::vector<std::string> children(const std::string& p) const override {
        if (p == "/") return {"X", "obs", "var"};
        if (p == "/X") return {"data", "indices", "indptr"};
        if (p == "/obs" || p == "/var") return {"_index"};
        return {};
    }
    bool        has_attr(const std::string&, const char*) const override { return false; }
    std::string attr_string(const std::string& p, const char* a) const override {
        if (std::string(a) == "_index" && (p == "/obs" || p == "/var")) return "_index";
        if (std::string(a) != "encoding-type") return "";
        if (p == "/") return "anndata";
        if (p == "/X") return "csr_matrix";
        if (p == "/obs" || p == "/var") return "dataframe";
        return "";
    }
    std::vector<int64_t> attr_ints(const std::string& p, const char* a) const override {
        return p == "/X" && std::string(a) == "shape" ? std::vector<int64_t>{100000, 300}
                                                      : std::vector<int64_t>{};
    }
    bool attr_bool(const std::string&, const char*) const override { return false; }
    std::optional<ArrayInfo> info(const std::string& p) const override {
        if (p == "/X/indptr") return ArrayInfo{{100001}, VClass::Int, "int64", 8};
        if (p == "/X/indices")
            return ArrayInfo{{100000 * entries_per_row}, VClass::Int, "int64", 8};
        if (p == "/X/data")
            return ArrayInfo{{100000 * entries_per_row}, VClass::Float, "float64", 8};
        if (p == "/obs/_index") return ArrayInfo{{100000}, VClass::String, "string", 8};
        if (p == "/var/_index") return ArrayInfo{{300}, VClass::String, "string", 8};
        return std::nullopt;
    }
    arrow::Result<std::shared_ptr<arrow::Array>> read_column(const std::string& p, int64_t off,
                                                             int64_t len) const override {
        if (p != "/obs/_index" && p != "/var/_index")
            return arrow::Status::Invalid("unexpected column ", p);
        arrow::StringBuilder b;
        for (int64_t i = 0; i < len; ++i)
            ARROW_RETURN_NOT_OK(
                b.Append((p == "/obs/_index" ? "cell" : "gene") + std::to_string(off + i)));
        std::shared_ptr<arrow::Array> a;
        ARROW_RETURN_NOT_OK(b.Finish(&a));
        return a;
    }
    arrow::Status read_i64(const std::string& p, int64_t off, int64_t len,
                           int64_t* out) const override {
        largest_read = std::max(largest_read, len);
        if (p != "/X/indptr" && p != "/X/indices")
            return arrow::Status::Invalid("unexpected read ", p);
        if (p == "/X/indices") matrix_indices += len;
        for (int64_t i = 0; i < len; ++i)
            out[i] = p == "/X/indptr" ? entries_per_row * (off + i)
                                      : ((off + i) % 3 == 2 ? 299 : (off + i) % 3);
        return arrow::Status::OK();
    }
    arrow::Status read_f64(const std::string& p, int64_t off, int64_t len,
                           double* out) const override {
        if (p != "/X/data") return arrow::Status::Invalid("unexpected read ", p);
        largest_read = std::max(largest_read, len);
        matrix_values += len;
        for (int64_t i = 0; i < len; ++i) out[i] = ((off + i) / 3) * 10 + (off + i) % 3 + 1;
        return arrow::Status::OK();
    }
    arrow::Status read_block_i64(const std::string&, int64_t, int64_t, int64_t, int64_t,
                                 int64_t*) const override {
        return arrow::Status::Invalid("unexpected dense read");
    }
    arrow::Status read_block_f64(const std::string&, int64_t, int64_t, int64_t, int64_t,
                                 double*) const override {
        return arrow::Status::Invalid("unexpected dense read");
    }
    std::optional<StorageInfo> storage(const std::string&) const override { return std::nullopt; }
    std::vector<HierarchyRow>  hierarchy() const override { return {}; }
    int64_t                    total_bytes() const override { return -1; }
    std::string                format_name() const override { return "test Store"; }
    std::string read_why(const std::string&) const override { return "unexpected read"; }
};

bool check() {
    auto                           store = std::make_shared<CountingStore>();
    std::unique_ptr<TabularSource> src;
    if (!open_store_source(store, "test.h5ad", &src).empty()) return false;
    auto           tabs   = src->expand_tabs();
    TabularSource* matrix = nullptr;
    TabularSource* obs    = nullptr;
    for (auto& t : tabs) {
        if (t->tab_label() == "X (preview)") matrix = t.get();
        if (t->tab_label() == "obs") obs = t.get();
    }
    if (!matrix || !obs || matrix->schema()->num_fields() != 201 ||
        matrix->total_rows() != 100000 || obs->total_rows() != 100000 || !matrix->random_access() ||
        matrix->preview_limit().rows_capped() || !matrix->preview_limit().cols_capped() ||
        store->matrix_values || store->matrix_indices)
        return false;
    std::shared_ptr<arrow::Table> table;
    if (!matrix->read_first(20, {0, 1}, &table).ok() || table->num_rows() != 20 ||
        store->matrix_values != 60 || store->matrix_indices != 60 ||
        cell_to_string(*table->column(1)->chunk(0), 19) != "191")
        return false;
    const auto before = store->matrix_values;
    const int  last   = matrix->num_chunks() - 1;
    const auto meta   = matrix->chunk_meta(last);
    if (!matrix->read_chunk(last, {1}, &table).ok() || table->num_rows() != meta.num_rows ||
        store->matrix_values - before != 3 * meta.num_rows ||
        cell_to_string(*table->column(0)->chunk(0), meta.num_rows - 1) != "999991" ||
        store->largest_read > 65536)
        return false;
    // Label-only projection must not read sparse entries.
    const auto entries = store->matrix_values;
    if (!matrix->read_first(10, {0}, &table).ok() || store->matrix_values != entries) return false;
    if (!matrix->read_first(300, {1}, &table).ok() || table->num_rows() != 300) return false;
    // One exceptionally dense row still uses bounded physical read buffers.
    auto heavy             = std::make_shared<CountingStore>();
    heavy->entries_per_row = 100000;
    if (!open_store_source(heavy, "heavy.h5ad", &src).empty()) return false;
    auto heavy_tabs = src->expand_tabs();
    for (auto& t : heavy_tabs) {
        if (t->tab_label() != "X (preview)") continue;
        if (!t->read_first(1, {1}, &table).ok() || table->num_rows() != 1 ||
            heavy->matrix_values != 100000 || heavy->matrix_indices != 100000 ||
            heavy->largest_read > 65536)
            return false;
        return true;
    }
    return false;
}
}  // namespace

int main() {
    if (!check()) {
        std::fprintf(stderr, "HDF5 viewport metadata/projection/deep-read check failed\n");
        return 1;
    }
    std::puts("ok: metadata-only shape, projected head, direct last block and label-only reads");
    return 0;
}
