// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Cooler Hi-C contact matrices (.cool, and .mcool with one cooler per
// resolution under /resolutions/<bp>), an HDF5 layout:
//   /chroms/{name,length}            one row per chromosome
//   /bins/{chrom,start,end,weight?…} one row per bin; chrom indexes /chroms
//   /pixels/{bin1_id,bin2_id,count}  the stored (upper-triangle) entries
//   /indexes/{chrom_offset,bin1_offset}
// Tabs: summary, chroms, then per resolution the pixels joined to their bin
// coordinates (as `cooler dump --join`, plus `balanced` when the bins carry a
// weight) and the bins. Bins and pixels are streamed in hyperslab chunks; -r
// selects the region × region submatrix through the bin1 index.

#include "internal.hpp"

namespace {

constexpr int64_t kRowsPerChunk = 1 << 20;

struct Hid {
    hid_t id = -1;
    herr_t (*close)(hid_t) = nullptr;
    Hid() = default;
    Hid(hid_t i, herr_t (*c)(hid_t)) : id(i), close(c) {}
    Hid(const Hid&) = delete;
    Hid& operator=(const Hid&) = delete;
    ~Hid() { if (id >= 0 && close) close(id); }
    explicit operator bool() const { return id >= 0; }
};

using H5File = std::shared_ptr<hid_t>;

H5File open_file(const std::string& path) {
    h5v::register_hdf5_filters();
    hid_t f = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (f < 0) return nullptr;
    return H5File(new hid_t(f), [](hid_t* p) { H5Fclose(*p); delete p; });
}

bool exists(hid_t loc, const std::string& name) {
    return H5Lexists(loc, name.c_str(), H5P_DEFAULT) > 0 &&
           H5Oexists_by_name(loc, name.c_str(), H5P_DEFAULT) > 0;
}

int64_t int_attr(hid_t obj, const char* name, int64_t fallback = -1) {
    if (H5Aexists(obj, name) <= 0) return fallback;
    Hid a(H5Aopen(obj, name, H5P_DEFAULT), H5Aclose);
    if (!a) return fallback;
    Hid sp(H5Aget_space(a.id), H5Sclose);
    if (H5Sget_simple_extent_npoints(sp.id) != 1) return fallback;
    Hid t(H5Aget_type(a.id), H5Tclose);
    const H5T_class_t c = H5Tget_class(t.id);
    if (c == H5T_INTEGER) {
        int64_t v = 0;
        return H5Aread(a.id, H5T_NATIVE_INT64, &v) >= 0 ? v : fallback;
    }
    if (c == H5T_FLOAT) {
        double v = 0;
        if (H5Aread(a.id, H5T_NATIVE_DOUBLE, &v) < 0 || !(std::fabs(v) < 9e18)) return fallback;
        return (int64_t)v;
    }
    return fallback;
}

// Length of a 1-D dataset, -1 when absent or not 1-D.
int64_t len_1d(hid_t loc, const std::string& name) {
    if (!exists(loc, name)) return -1;
    Hid d(H5Dopen2(loc, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!d) return -1;
    Hid s(H5Dget_space(d.id), H5Sclose);
    if (H5Sget_simple_extent_ndims(s.id) != 1) return -1;
    hsize_t n = 0;
    H5Sget_simple_extent_dims(s.id, &n, nullptr);
    return (int64_t)n;
}

// Elements [lo, lo + n) of a 1-D numeric dataset, converted to T. An HDF5
// enum (cooler writes bins/chrom as one) is read as its integer values.
template <typename T>
std::string read_1d(hid_t loc, const std::string& name, int64_t lo, int64_t n, std::vector<T>* out) {
    out->assign((size_t)std::max<int64_t>(n, 0), T{});
    if (n <= 0) return "";
    Hid d(H5Dopen2(loc, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!d) return "cannot open dataset '" + name + "'";
    Hid ft(H5Dget_type(d.id), H5Tclose);
    const H5T_class_t cls = H5Tget_class(ft.id);
    if (cls != H5T_INTEGER && cls != H5T_FLOAT && cls != H5T_ENUM)
        return "'" + name + "' is not numeric";
    Hid fs(H5Dget_space(d.id), H5Sclose);
    hsize_t start = (hsize_t)lo, cnt = (hsize_t)n, dim = 0;
    H5Sget_simple_extent_dims(fs.id, &dim, nullptr);
    if (start + cnt > dim) return "'" + name + "' is shorter than expected";
    H5Sselect_hyperslab(fs.id, H5S_SELECT_SET, &start, nullptr, &cnt, nullptr);
    Hid ms(H5Screate_simple(1, &cnt, nullptr), H5Sclose);
    herr_t st;
    if (cls == H5T_ENUM) {
        Hid base(H5Tget_super(ft.id), H5Tclose);
        Hid native(H5Tget_native_type(ft.id, H5T_DIR_ASCEND), H5Tclose);
        const size_t sz = H5Tget_size(native.id);
        const bool sgn = H5Tget_sign(base.id) == H5T_SGN_2;
        if (sz != 1 && sz != 2 && sz != 4 && sz != 8) return "'" + name + "': unsupported enum size";
        std::vector<uint8_t> raw((size_t)n * sz);
        st = H5Dread(d.id, native.id, ms.id, fs.id, H5P_DEFAULT, raw.data());
        for (int64_t k = 0; st >= 0 && k < n; ++k) {
            const uint8_t* p = raw.data() + (size_t)k * sz;
            int64_t v = 0;
            switch (sz) {
                case 1: v = sgn ? (int64_t)*(const int8_t*)p : (int64_t)*p; break;
                case 2: { int16_t x; std::memcpy(&x, p, 2); v = sgn ? x : (uint16_t)x; break; }
                case 4: { int32_t x; std::memcpy(&x, p, 4); v = sgn ? x : (int64_t)(uint32_t)x; break; }
                default: std::memcpy(&v, p, 8); break;
            }
            (*out)[(size_t)k] = (T)v;
        }
    } else {
        const hid_t mt = std::is_floating_point_v<T> ? H5T_NATIVE_DOUBLE
                       : sizeof(T) == 4 ? H5T_NATIVE_INT32 : H5T_NATIVE_INT64;
        static_assert(std::is_same_v<T, double> || std::is_same_v<T, int32_t> ||
                      std::is_same_v<T, int64_t>);
        st = H5Dread(d.id, mt, ms.id, fs.id, H5P_DEFAULT, out->data());
    }
    if (st < 0) return h5v::h5_read_failure(d.id);
    return "";
}

std::vector<std::string> read_strings(hid_t loc, const std::string& name, std::string* err) {
    std::vector<std::string> v;
    Hid d(H5Dopen2(loc, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!d) { *err = "cannot open '" + name + "'"; return v; }
    Hid ft(H5Dget_type(d.id), H5Tclose);
    Hid fs(H5Dget_space(d.id), H5Sclose);
    if (H5Tget_class(ft.id) != H5T_STRING || H5Sget_simple_extent_ndims(fs.id) != 1) {
        *err = "'" + name + "' is not a 1-D string dataset";
        return v;
    }
    hsize_t n = 0;
    H5Sget_simple_extent_dims(fs.id, &n, nullptr);
    if (H5Tis_variable_str(ft.id) > 0) {
        Hid mt(H5Tcopy(H5T_C_S1), H5Tclose);
        H5Tset_size(mt.id, H5T_VARIABLE);
        std::vector<char*> p((size_t)n, nullptr);
        if (H5Dread(d.id, mt.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, p.data()) < 0) {
            *err = h5v::h5_read_failure(d.id);
            return v;
        }
        for (char* s : p) v.emplace_back(s ? s : "");
        H5Dvlen_reclaim(mt.id, fs.id, H5P_DEFAULT, p.data());
    } else {
        const size_t w = H5Tget_size(ft.id);
        std::vector<char> buf((size_t)n * w);
        if (H5Dread(d.id, ft.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data()) < 0) {
            *err = h5v::h5_read_failure(d.id);
            return v;
        }
        for (hsize_t k = 0; k < n; ++k) {
            const char* s = buf.data() + k * w;
            v.emplace_back(s, strnlen(s, w));
        }
    }
    return v;
}

// Link names of a group, in name order.
std::vector<std::string> child_names(hid_t g) {
    std::vector<std::string> v;
    H5G_info_t info;
    if (H5Gget_info(g, &info) < 0) return v;
    for (hsize_t k = 0; k < info.nlinks; ++k) {
        const ssize_t n = H5Lget_name_by_idx(g, ".", H5_INDEX_NAME, H5_ITER_INC, k, nullptr, 0, H5P_DEFAULT);
        if (n <= 0) continue;
        std::string s((size_t)n + 1, '\0');
        H5Lget_name_by_idx(g, ".", H5_INDEX_NAME, H5_ITER_INC, k, s.data(), s.size(), H5P_DEFAULT);
        s.resize((size_t)n);
        v.push_back(std::move(s));
    }
    return v;
}

std::string bp_label(int64_t bp) {
    if (bp >= 1000000 && bp % 1000000 == 0) return std::to_string(bp / 1000000) + "Mb";
    if (bp >= 1000 && bp % 1000 == 0) return std::to_string(bp / 1000) + "kb";
    return std::to_string(bp) + "bp";
}

// One cooler (the file root, or /resolutions/<bp> of an .mcool): its
// chromosomes and bins, read once and shared by its tabs.
struct Cooler {
    H5File      file;
    std::string root;                 // "/" or "/resolutions/<bp>"
    int64_t     binsize = -1, nnz = 0;
    std::vector<std::string> chroms;
    std::vector<int64_t>     lengths;
    std::vector<int32_t>     bin_chrom;    // -1: out of range in the file
    std::vector<int64_t>     bin_start, bin_end;
    std::vector<int64_t>     chrom_offset; // nchroms + 1, clamped and monotone
    std::vector<double>      weight;       // empty: not balanced
    std::shared_ptr<arrow::Array> chrom_dict;
    std::string error;
    bool loaded = false;

    int64_t nbins() const { return (int64_t)bin_start.size(); }
    std::string path(const std::string& sub) const {
        return root == "/" ? "/" + sub : root + "/" + sub;
    }

    // Read chroms and bins; "" or an error (also kept in `error`).
    const std::string& load() {
        if (loaded) return error;
        loaded = true;
        hid_t f = *file;
        // Dataset sizes are declared by the file (HDF5 allows huge unallocated
        // datasets), so an allocation can fail: report it, do not terminate.
        try {
        error = [&]() -> std::string {
            std::string e;
            chroms = read_strings(f, path("chroms/name"), &e);
            if (!e.empty()) return e;
            if (auto er = read_1d(f, path("chroms/length"), 0, (int64_t)chroms.size(), &lengths); !er.empty())
                return er;
            const int64_t n = len_1d(f, path("bins/start"));
            if (n < 0 || len_1d(f, path("bins/end")) != n || len_1d(f, path("bins/chrom")) != n)
                return "bins/chrom, bins/start and bins/end must be 1-D of one length";
            if (auto er = read_1d(f, path("bins/chrom"), 0, n, &bin_chrom); !er.empty()) return er;
            if (auto er = read_1d(f, path("bins/start"), 0, n, &bin_start); !er.empty()) return er;
            if (auto er = read_1d(f, path("bins/end"), 0, n, &bin_end); !er.empty()) return er;
            for (auto& c : bin_chrom)
                if (c < 0 || c >= (int32_t)chroms.size()) c = -1;
            if (len_1d(f, path("bins/weight")) == n)
                if (auto er = read_1d(f, path("bins/weight"), 0, n, &weight); !er.empty()) return er;
            nnz = std::max<int64_t>(0, len_1d(f, path("pixels/bin1_id")));
            if (len_1d(f, path("pixels/bin2_id")) != nnz || len_1d(f, path("pixels/count")) != nnz)
                return "pixels/bin1_id, bin2_id and count must be 1-D of one length";
            // chrom_offset (indexes) or, when absent, from bins/chrom.
            const int64_t nc = (int64_t)chroms.size();
            if (len_1d(f, path("indexes/chrom_offset")) == nc + 1) {
                if (auto er = read_1d(f, path("indexes/chrom_offset"), 0, nc + 1, &chrom_offset); !er.empty())
                    return er;
            } else {
                chrom_offset.assign((size_t)nc + 1, 0);
                for (int32_t c : bin_chrom) if (c >= 0) ++chrom_offset[(size_t)c + 1];
                for (size_t k = 1; k < chrom_offset.size(); ++k) chrom_offset[k] += chrom_offset[k - 1];
            }
            for (size_t k = 0; k < chrom_offset.size(); ++k)
                chrom_offset[k] = std::clamp<int64_t>(chrom_offset[k], k ? chrom_offset[k - 1] : 0, n);
            arrow::StringBuilder b;
            ARROW_UNUSED(b.AppendValues(chroms));
            if (!b.Finish(&chrom_dict).ok()) return "out of memory";
            return "";
        }();
        } catch (const std::bad_alloc&) {
            error = "out of memory reading chroms / bins (declared sizes too large?)";
        }
        if (!error.empty()) error = "'" + root + "': " + error;
        return error;
    }

    // Bin-id ranges [lo, hi) overlapping the region windows, in window order.
    std::vector<std::pair<int64_t, int64_t>> region_bins(const std::vector<Region>& ws) const {
        std::vector<std::pair<int64_t, int64_t>> out;
        for (const Region& w : ws) {
            const auto it = std::find(chroms.begin(), chroms.end(), w.chrom);
            if (it == chroms.end()) continue;
            const size_t c = (size_t)(it - chroms.begin());
            int64_t lo = chrom_offset[c], hi = chrom_offset[c + 1];
            // Bins of a chromosome are sorted by start: first bin ending after
            // the window start, first bin starting at or after its end.
            const int64_t ws_ = w.start == INT64_MIN ? INT64_MIN : w.start;
            const int64_t we_ = w.end;
            int64_t a = std::partition_point(bin_end.begin() + lo, bin_end.begin() + hi,
                                             [&](int64_t e) { return e <= ws_; }) - bin_end.begin();
            int64_t b = std::partition_point(bin_start.begin() + a, bin_start.begin() + hi,
                                             [&](int64_t s) { return s < we_; }) - bin_start.begin();
            if (a < b) out.emplace_back(a, b);
        }
        return out;
    }

    std::shared_ptr<arrow::Array> chrom_column(const std::vector<int32_t>& codes) const {
        arrow::Int32Builder b;
        ARROW_UNUSED(b.Reserve((int64_t)codes.size()));
        for (int32_t c : codes) {
            if (c < 0) b.UnsafeAppendNull();
            else b.UnsafeAppend(c);
        }
        std::shared_ptr<arrow::Array> idx;
        ARROW_UNUSED(b.Finish(&idx));
        auto r = arrow::DictionaryArray::FromArrays(arrow::dictionary(arrow::int32(), arrow::utf8()), idx,
                                                    chrom_dict);
        return r.ok() ? *r : nullptr;
    }
};

std::shared_ptr<arrow::Array> int64_array(const std::vector<int64_t>& v, const std::vector<bool>* valid = nullptr) {
    arrow::Int64Builder b;
    ARROW_UNUSED(b.Reserve((int64_t)v.size()));
    for (size_t k = 0; k < v.size(); ++k) {
        if (valid && !(*valid)[k]) b.UnsafeAppendNull();
        else b.UnsafeAppend(v[k]);
    }
    std::shared_ptr<arrow::Array> a;
    ARROW_UNUSED(b.Finish(&a));
    return a;
}

std::shared_ptr<arrow::Array> double_array(const std::vector<double>& v) {
    arrow::DoubleBuilder b;
    ARROW_UNUSED(b.Reserve((int64_t)v.size()));
    for (double x : v) {
        if (std::isnan(x)) b.UnsafeAppendNull();
        else b.UnsafeAppend(x);
    }
    std::shared_ptr<arrow::Array> a;
    ARROW_UNUSED(b.Finish(&a));
    return a;
}

bool float_dataset(hid_t loc, const std::string& name) {
    Hid d(H5Dopen2(loc, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!d) return false;
    Hid t(H5Dget_type(d.id), H5Tclose);
    return H5Tget_class(t.id) == H5T_FLOAT;
}

std::shared_ptr<arrow::Table> select(const std::shared_ptr<arrow::Schema>& schema,
                                     const std::vector<std::shared_ptr<arrow::Array>>& all,
                                     const std::vector<int>& cols, int64_t rows) {
    arrow::FieldVector fields;
    arrow::ArrayVector arrays;
    for (int c : cols) {
        if (c < 0 || c >= (int)all.size()) continue;
        fields.push_back(schema->field(c));
        arrays.push_back(all[(size_t)c]);
    }
    return arrow::Table::Make(arrow::schema(fields), arrays, rows);
}

// Shared by the bins and pixels tabs: the cooler, the region, the footer.
class CoolerTab : public TabularSource {
protected:
    std::string path_, label_;
    std::shared_ptr<Cooler> c_;
    std::vector<Region> windows_;
    bool region_ = false;
    std::shared_ptr<arrow::Schema> schema_;
    mutable bool init_ = false;
    mutable arrow::Status status_;
    std::string what_() const {
        return bp_label(c_->binsize) + " bins";
    }
public:
    CoolerTab(std::string path, std::string label, std::shared_ptr<Cooler> c, const std::string& region)
        : path_(std::move(path)), label_(std::move(label)), c_(std::move(c)) {
        if (!region.empty()) { windows_ = parse_region_list(region); region_ = true; }
    }
    const std::string& path() const override { return path_; }
    std::string tab_label() const override { return label_; }
    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    arrow::Status read_status() const override { return status_; }
    bool region_applied() const override { return region_; }
};

// /bins: chrom, start, end and every other 1-D numeric dataset of nbins rows.
class BinsSource : public CoolerTab {
    std::vector<std::string> extra_;             // dataset names beyond chrom/start/end
    std::vector<bool>        extra_float_;
    std::vector<std::pair<int64_t, int64_t>> ranges_;  // bin ranges shown
    std::vector<int64_t>     range_first_;           // first row of each range
    int64_t rows_ = 0;

    void init() const {
        if (init_) return;
        init_ = true;
        auto* self = const_cast<BinsSource*>(this);
        if (const std::string& e = c_->load(); !e.empty()) { self->status_ = arrow::Status::IOError(e); return; }
        if (region_) {
            auto ws = windows_;
            resolve_region_chroms(ws, [&](const std::string& n) {
                return std::find(c_->chroms.begin(), c_->chroms.end(), n) != c_->chroms.end();
            }, path_);
            self->ranges_ = c_->region_bins(ws);
        } else if (c_->nbins() > 0) {
            self->ranges_ = {{0, c_->nbins()}};
        }
        for (const auto& r : ranges_) { self->range_first_.push_back(rows_); self->rows_ += r.second - r.first; }
    }
public:
    BinsSource(std::string path, std::string label, std::shared_ptr<Cooler> c, const std::string& region)
        : CoolerTab(std::move(path), std::move(label), std::move(c), region) {
        arrow::FieldVector f{arrow::field("chrom", arrow::dictionary(arrow::int32(), arrow::utf8())),
                             arrow::field("start", arrow::int64()), arrow::field("end", arrow::int64())};
        hid_t g = H5Gopen2(*c_->file, c_->path("bins").c_str(), H5P_DEFAULT);
        if (g >= 0) {
            const int64_t n = len_1d(g, "start");
            for (const std::string& name : child_names(g)) {
                if (name == "chrom" || name == "start" || name == "end" || len_1d(g, name) != n) continue;
                Hid d(H5Dopen2(g, name.c_str(), H5P_DEFAULT), H5Dclose);
                Hid t(H5Dget_type(d.id), H5Tclose);
                const H5T_class_t cls = H5Tget_class(t.id);
                if (cls != H5T_INTEGER && cls != H5T_FLOAT) continue;
                extra_.push_back(name);
                extra_float_.push_back(cls == H5T_FLOAT);
                f.push_back(arrow::field(name, cls == H5T_FLOAT ? arrow::float64() : arrow::int64()));
            }
            H5Gclose(g);
        }
        schema_ = arrow::schema(f);
    }
    int64_t total_rows() const override { init(); return rows_; }
    int num_chunks() const override { init(); return (int)((rows_ + kRowsPerChunk - 1) / kRowsPerChunk); }
    ChunkMeta chunk_meta(int i) const override {
        const int64_t first = (int64_t)i * kRowsPerChunk;
        return {first, std::max<int64_t>(0, std::min(kRowsPerChunk, rows_ - first))};
    }
    arrow::Status read_status() const override { init(); return status_; }
    std::string footer() const override {
        init();
        return "Format: Cooler bins  |  " + what_() + "  |  " + std::to_string(c_->nbins()) + " bins" +
               (c_->weight.empty() ? "" : "  |  balanced (weight)");
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        init();
        ARROW_RETURN_NOT_OK(status_);
        const ChunkMeta m = chunk_meta(i);
        std::vector<int32_t> chrom;
        std::vector<int64_t> start, end;
        std::vector<std::vector<int64_t>> ei(extra_.size());
        std::vector<std::vector<double>>  ef(extra_.size());
        hid_t f = *c_->file;
        // Walk the ranges covering rows [m.first_row, m.first_row + m.num_rows).
        size_t r = (size_t)(std::upper_bound(range_first_.begin(), range_first_.end(), m.first_row) -
                            range_first_.begin()) - 1;
        int64_t row = m.first_row, left = m.num_rows;
        while (left > 0 && r < ranges_.size()) {
            const int64_t lo = ranges_[r].first + (row - range_first_[r]);
            const int64_t n = std::min(left, ranges_[r].second - lo);
            for (int64_t b = lo; b < lo + n; ++b) {
                chrom.push_back(c_->bin_chrom[(size_t)b]);
                start.push_back(c_->bin_start[(size_t)b]);
                end.push_back(c_->bin_end[(size_t)b]);
            }
            for (size_t k = 0; k < extra_.size(); ++k) {
                std::string e;
                if (extra_float_[k]) {
                    std::vector<double> v;
                    e = read_1d(f, c_->path("bins/" + extra_[k]), lo, n, &v);
                    ef[k].insert(ef[k].end(), v.begin(), v.end());
                } else {
                    std::vector<int64_t> v;
                    e = read_1d(f, c_->path("bins/" + extra_[k]), lo, n, &v);
                    ei[k].insert(ei[k].end(), v.begin(), v.end());
                }
                if (!e.empty()) return arrow::Status::IOError(e);
            }
            row += n; left -= n; ++r;
        }
        std::vector<std::shared_ptr<arrow::Array>> all{c_->chrom_column(chrom), int64_array(start),
                                                       int64_array(end)};
        for (size_t k = 0; k < extra_.size(); ++k)
            all.push_back(extra_float_[k] ? double_array(ef[k]) : int64_array(ei[k]));
        *out = select(schema_, all, cols, (int64_t)chrom.size());
        return arrow::Status::OK();
    }
};

// /pixels joined to bin coordinates. Without a region, chunk k is pixels
// [k·N, (k+1)·N). With one, the candidate pixels are those whose bin1 lies in
// a region bin range (contiguous through indexes/bin1_offset); a chunk is one
// slice of up to N candidates, filtered to bin2 in the region too, so chunk
// sizes are known only once read (the source is then forward-scanned).
class PixelsSource : public CoolerTab {
    struct Slice { int64_t p0, p1, first_row, rows; };
    std::vector<std::pair<int64_t, int64_t>> bin_ranges_;
    std::vector<std::pair<int64_t, int64_t>> cand_;       // candidate pixel ranges
    mutable std::vector<Slice> slices_;
    mutable size_t cand_i_ = 0;                            // next candidate range to slice
    mutable int64_t cand_pos_ = 0;
    mutable bool done_ = false;
    bool count_float_ = false, balanced_ = false;

    bool in_region(int64_t b) const {
        for (const auto& r : bin_ranges_) if (b >= r.first && b < r.second) return true;
        return false;
    }
    void init() const {
        if (init_) return;
        init_ = true;
        auto* self = const_cast<PixelsSource*>(this);
        if (const std::string& e = c_->load(); !e.empty()) { self->status_ = arrow::Status::IOError(e); return; }
        if (!region_) {
            for (int64_t p = 0; p < c_->nnz; p += kRowsPerChunk) {
                const int64_t n = std::min(kRowsPerChunk, c_->nnz - p);
                slices_.push_back({p, p + n, p, n});
            }
            done_ = true;
            return;
        }
        auto ws = windows_;
        resolve_region_chroms(ws, [&](const std::string& n) {
            return std::find(c_->chroms.begin(), c_->chroms.end(), n) != c_->chroms.end();
        }, path_);
        self->bin_ranges_ = c_->region_bins(ws);
        const int64_t nb = c_->nbins();
        std::vector<int64_t> off;   // nbins + 1, sized like the bins already read
        if (len_1d(*c_->file, c_->path("indexes/bin1_offset")) == nb + 1) {
            if (auto e = read_1d(*c_->file, c_->path("indexes/bin1_offset"), 0, nb + 1, &off); !e.empty()) {
                self->status_ = arrow::Status::IOError(e);
                return;
            }
        } else {
            self->status_ = arrow::Status::IOError("'" + c_->root + "': no indexes/bin1_offset for -r");
            return;
        }
        for (size_t k = 0; k < off.size(); ++k)
            off[k] = std::clamp<int64_t>(off[k], k ? off[k - 1] : 0, c_->nnz);
        for (const auto& r : bin_ranges_)
            if (off[(size_t)r.first] < off[(size_t)r.second])
                self->cand_.emplace_back(off[(size_t)r.first], off[(size_t)r.second]);
        if (cand_.empty()) done_ = true;
    }
    arrow::Status read_ids(int64_t p0, int64_t n, std::vector<int64_t>* b1, std::vector<int64_t>* b2) const {
        if (auto e = read_1d(*c_->file, c_->path("pixels/bin1_id"), p0, n, b1); !e.empty())
            return arrow::Status::IOError(e);
        if (auto e = read_1d(*c_->file, c_->path("pixels/bin2_id"), p0, n, b2); !e.empty())
            return arrow::Status::IOError(e);
        return arrow::Status::OK();
    }
    // Slice the next run of candidates and count its rows in the region.
    void advance() const {
        while (!done_ && status_.ok()) {
            if (cand_i_ >= cand_.size()) { done_ = true; return; }
            const auto [lo, hi] = cand_[cand_i_];
            const int64_t p0 = std::max(lo, cand_pos_);
            const int64_t n = std::min(kRowsPerChunk, hi - p0);
            cand_pos_ = p0 + n;
            if (cand_pos_ >= hi) { ++cand_i_; cand_pos_ = 0; }
            std::vector<int64_t> b1, b2;
            if (auto st = read_ids(p0, n, &b1, &b2); !st.ok()) {
                const_cast<PixelsSource*>(this)->status_ = st;
                return;
            }
            int64_t rows = 0;
            for (int64_t k = 0; k < n; ++k) rows += in_region(b2[(size_t)k]);
            if (rows == 0) continue;
            const int64_t first = slices_.empty() ? 0 : slices_.back().first_row + slices_.back().rows;
            slices_.push_back({p0, p0 + n, first, rows});
            return;
        }
    }
public:
    PixelsSource(std::string path, std::string label, std::shared_ptr<Cooler> c, const std::string& region)
        : CoolerTab(std::move(path), std::move(label), std::move(c), region) {
        count_float_ = float_dataset(*c_->file, c_->path("pixels/count"));
        balanced_ = len_1d(*c_->file, c_->path("bins/weight")) == len_1d(*c_->file, c_->path("bins/start"));
        const auto chrom_t = arrow::dictionary(arrow::int32(), arrow::utf8());
        arrow::FieldVector f{arrow::field("chrom1", chrom_t), arrow::field("start1", arrow::int64()),
                             arrow::field("end1", arrow::int64()), arrow::field("chrom2", chrom_t),
                             arrow::field("start2", arrow::int64()), arrow::field("end2", arrow::int64()),
                             arrow::field("count", count_float_ ? arrow::float64() : arrow::int64())};
        if (balanced_) f.push_back(arrow::field("balanced", arrow::float64()));
        schema_ = arrow::schema(f);
    }
    int64_t total_rows() const override {
        init();
        if (!done_) return -1;
        return slices_.empty() ? 0 : slices_.back().first_row + slices_.back().rows;
    }
    int num_chunks() const override { init(); return (int)slices_.size(); }
    void ensure(int i) override {
        init();
        while ((int)slices_.size() <= i && !done_ && status_.ok()) advance();
    }
    ChunkMeta chunk_meta(int i) const override {
        if (i < 0 || i >= (int)slices_.size()) return {0, 0};
        return {slices_[(size_t)i].first_row, slices_[(size_t)i].rows};
    }
    arrow::Status read_status() const override { init(); return status_; }
    std::string footer() const override {
        init();
        std::string s = "Format: Cooler pixels  |  " + what_() + "  |  " + std::to_string(c_->nnz) +
                        " stored pixels (upper triangle)";
        if (balanced_) s += "  |  balanced = count \xc3\x97 weight1 \xc3\x97 weight2";
        return s;
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        ARROW_RETURN_NOT_OK(status_);
        if (i < 0 || i >= (int)slices_.size()) return arrow::Status::IndexError("chunk ", i);
        const Slice s = slices_[(size_t)i];
        const int64_t n = s.p1 - s.p0;
        std::vector<int64_t> b1, b2, ci;
        std::vector<double> cf;
        ARROW_RETURN_NOT_OK(read_ids(s.p0, n, &b1, &b2));
        const std::string cnt = c_->path("pixels/count");
        if (auto e = count_float_ ? read_1d(*c_->file, cnt, s.p0, n, &cf) : read_1d(*c_->file, cnt, s.p0, n, &ci);
            !e.empty())
            return arrow::Status::IOError(e);
        const int64_t nb = c_->nbins();
        std::vector<int32_t> c1, c2;
        std::vector<int64_t> s1, e1, s2, e2, count_i;
        std::vector<bool> ok1, ok2;
        std::vector<double> count_f, bal;
        for (int64_t k = 0; k < n; ++k) {
            const int64_t x = b1[(size_t)k], y = b2[(size_t)k];
            if (region_ && !in_region(y)) continue;
            const bool vx = x >= 0 && x < nb, vy = y >= 0 && y < nb;   // ids are untrusted
            c1.push_back(vx ? c_->bin_chrom[(size_t)x] : -1);
            s1.push_back(vx ? c_->bin_start[(size_t)x] : 0);
            e1.push_back(vx ? c_->bin_end[(size_t)x] : 0);
            c2.push_back(vy ? c_->bin_chrom[(size_t)y] : -1);
            s2.push_back(vy ? c_->bin_start[(size_t)y] : 0);
            e2.push_back(vy ? c_->bin_end[(size_t)y] : 0);
            ok1.push_back(vx);
            ok2.push_back(vy);
            const double v = count_float_ ? cf[(size_t)k] : (double)ci[(size_t)k];
            if (count_float_) count_f.push_back(v);
            else count_i.push_back(ci[(size_t)k]);
            if (balanced_)
                bal.push_back(vx && vy ? v * c_->weight[(size_t)x] * c_->weight[(size_t)y] : NAN);
        }
        std::vector<std::shared_ptr<arrow::Array>> all{
            c_->chrom_column(c1), int64_array(s1, &ok1), int64_array(e1, &ok1),
            c_->chrom_column(c2), int64_array(s2, &ok2), int64_array(e2, &ok2),
            count_float_ ? double_array(count_f) : int64_array(count_i)};
        if (balanced_) all.push_back(double_array(bal));
        *out = select(schema_, all, cols, (int64_t)c1.size());
        return arrow::Status::OK();
    }
};

struct Summary {
    std::shared_ptr<arrow::Table> summary, chroms;
};

// Summary rows (field, value) and the chroms table.
std::string build_summary(const std::string& path, const std::vector<std::shared_ptr<Cooler>>& cs,
                          bool mcool, Summary* out) {
    hid_t f = *cs[0]->file;
    std::vector<std::pair<std::string, std::string>> rows;
    auto add = [&](const std::string& k, const std::string& v) { if (!v.empty()) rows.emplace_back(k, v); };
    Hid g0(H5Oopen(f, cs[0]->root.c_str(), H5P_DEFAULT), H5Oclose);
    add("format", mcool ? "Cooler (multi-resolution .mcool)" : "Cooler");
    add("format-version", std::to_string(int_attr(g0.id, "format-version")));
    if (mcool) {
        std::string res;
        for (const auto& c : cs) res += (res.empty() ? "" : ", ") + bp_label(c->binsize);
        add("resolutions", res);
    }
    add("genome-assembly", h5v::read_string_attr(g0.id, "genome-assembly"));
    add("storage-mode", h5v::read_string_attr(g0.id, "storage-mode"));
    add("chromosomes", std::to_string(cs[0]->chroms.size()));
    for (const auto& c : cs) {
        std::string v = std::to_string(c->nbins()) + " bins, " + std::to_string(c->nnz) + " pixels";
        Hid g(H5Oopen(f, c->root.c_str(), H5P_DEFAULT), H5Oclose);
        if (H5Aexists(g.id, "sum") > 0) v += ", sum " + std::to_string(int_attr(g.id, "sum"));
        v += c->weight.empty() ? ", not balanced" : ", balanced";
        add(mcool ? bp_label(c->binsize) : bp_label(c->binsize) + " bins", v);
    }
    add("generated-by", h5v::read_string_attr(g0.id, "generated-by"));
    add("creation-date", h5v::read_string_attr(g0.id, "creation-date"));
    add("metadata", h5v::read_string_attr(g0.id, "metadata"));
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) add("file size", std::to_string((int64_t)st.st_size) + " bytes");
    arrow::StringBuilder kb, vb;
    for (const auto& [k, v] : rows) { ARROW_UNUSED(kb.Append(k)); ARROW_UNUSED(vb.Append(v)); }
    std::shared_ptr<arrow::Array> ka, va;
    ARROW_UNUSED(kb.Finish(&ka));
    ARROW_UNUSED(vb.Finish(&va));
    out->summary = arrow::Table::Make(arrow::schema({arrow::field("field", arrow::utf8()),
                                                     arrow::field("value", arrow::utf8())}),
                                      {ka, va});
    arrow::StringBuilder nb;
    ARROW_UNUSED(nb.AppendValues(cs[0]->chroms));
    std::shared_ptr<arrow::Array> na;
    ARROW_UNUSED(nb.Finish(&na));
    out->chroms = arrow::Table::Make(arrow::schema({arrow::field("name", arrow::utf8()),
                                                    arrow::field("length", arrow::int64())}),
                                     {na, int64_array(cs[0]->lengths)});
    return "";
}

class CoolerSummarySource : public WorkbookSource {
public:
    CoolerSummarySource(std::shared_ptr<arrow::Table> t, std::string path, std::string footer,
                        std::string label, std::function<std::vector<std::unique_ptr<TabularSource>>()> sib)
        : WorkbookSource(std::move(t), std::move(path), std::move(footer)), label_(std::move(label)),
          siblings_(std::move(sib)) {}
    std::string tab_label() const override { return label_; }
    std::vector<std::unique_ptr<TabularSource>> open_sibling_sheets() const override {
        return siblings_ ? siblings_() : std::vector<std::unique_ptr<TabularSource>>{};
    }
    // -r selects the region in this file's pixels and bins tabs.
    void set_region(bool r) { region_ = r; }
    bool region_applied() const override { return region_; }
private:
    bool region_ = false;
    std::string label_;
    std::function<std::vector<std::unique_ptr<TabularSource>>()> siblings_;
};

}  // namespace

bool is_cooler_file(const std::string& path) {
    H5File f;
    {
        H5E_auto2_t fn; void* data;
        H5Eget_auto2(H5E_DEFAULT, &fn, &data);
        H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
        f = open_file(path);
        H5Eset_auto2(H5E_DEFAULT, fn, data);
    }
    if (!f) return false;
    const std::string fmt = h5v::read_string_attr(*f, "format");
    return fmt == "HDF5::Cooler" || fmt == "HDF5::MCOOL";
}

std::string open_cooler_source(const std::string& path, const Config& cfg, std::unique_ptr<TabularSource>* out) {
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    H5File f = open_file(path);
    if (!f) return "'" + path + "': cannot open as HDF5";
    const std::string fmt = h5v::read_string_attr(*f, "format");
    const bool mcool = fmt == "HDF5::MCOOL" || (fmt.empty() && exists(*f, "resolutions"));
    std::vector<std::shared_ptr<Cooler>> cs;
    if (mcool) {
        if (!exists(*f, "resolutions")) return "'" + path + "': .mcool without /resolutions";
        Hid g(H5Gopen2(*f, "resolutions", H5P_DEFAULT), H5Gclose);
        for (const auto& n : child_names(g.id)) {
            auto c = std::make_shared<Cooler>();
            c->file = f;
            c->root = "/resolutions/" + n;
            Hid rg(H5Gopen2(*f, c->root.c_str(), H5P_DEFAULT), H5Gclose);
            if (!rg) continue;
            c->binsize = int_attr(rg.id, "bin-size", std::atoll(n.c_str()));
            cs.push_back(std::move(c));
        }
        std::sort(cs.begin(), cs.end(), [](const auto& a, const auto& b) { return a->binsize < b->binsize; });
    } else {
        auto c = std::make_shared<Cooler>();
        c->file = f;
        c->root = "/";
        c->binsize = int_attr(*f, "bin-size");
        cs.push_back(std::move(c));
    }
    if (cs.empty()) return "'" + path + "': no resolutions in the .mcool";
    for (const auto& c : cs)
        if (auto e = c->load(); !e.empty()) return "'" + path + "': " + e;
    Summary sm;
    build_summary(path, cs, mcool, &sm);
    const std::string region = cfg.region;
    auto siblings = [path, cs, mcool, region, chroms = sm.chroms]() {
        std::vector<std::unique_ptr<TabularSource>> v;
        v.push_back(std::make_unique<CoolerSummarySource>(chroms, path, "Format: Cooler chromosomes", "chroms",
                                                          nullptr));
        for (const auto& c : cs)
            v.push_back(std::make_unique<PixelsSource>(path, mcool ? "pixels@" + bp_label(c->binsize) : "pixels",
                                                       c, region));
        for (const auto& c : cs)
            v.push_back(std::make_unique<BinsSource>(path, mcool ? "bins@" + bp_label(c->binsize) : "bins", c,
                                                     region));
        return v;
    };
    std::string footer = std::string("Format: Cooler") + (mcool ? " (.mcool)" : "") + " (summary)";
    auto first = std::make_unique<CoolerSummarySource>(sm.summary, path, footer, "summary", siblings);
    first->set_region(!region.empty());
    *out = std::move(first);
    return "";
}
