// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// HDF5, AnnData, Loom and 10x Cell Ranger HDF5.

#include "internal.hpp"

// ── HDF5 / AnnData viewer (`.h5ad` / `.h5` / `.hdf5` / `.loom`) ──────────────
//
// One Hdf5Source instance == one TUI tab. Tab 0 is either:
//   - For AnnData: a "summary" tab (shape, X encoding, layer count, …).
//   - For generic HDF5: a "hierarchy" tab — one row per H5Lvisit object
//     with path / kind / shape / dtype / compression / n_attrs columns.
// Sibling tabs (one per AnnData component or one per 1-/2-D HDF5 dataset)
// are constructed lazily via the existing WorkbookSource sibling-expansion
// path in main(). The HDF5 file handle is refcounted across siblings so
// the file stays open exactly as long as any tab references it (mirrors
// SqliteSource's shared_ptr<sqlite3> trick).
//
// All read-time densification happens through small helpers that build
// arrow::Tables column by column from hyperslab selections — Arrow
// columns become typed (int64 / double / utf8) per HDF5 datatype class,
// so `--filter`, `--describe`, sort, and search all work out of the box.

// ── LZF (HDF5 filter 32000) ────────────────────────────────────────────────
//
// h5py's `compression="lzf"` (used by `write_h5ad(compression="lzf")`) stores
// chunks with LZF, registered as HDF5 filter 32000 inside h5py's own process.
// libhdf5 does not include it, so vv registers a decoder for it (decoding only:
// vv never writes HDF5).
//
// LZF stream: a sequence of items, each starting with a control byte `c`.
//   c < 32   literal run: the next c+1 input bytes are copied to the output.
//   c >= 32  back reference: length L = c >> 5 (if 7, add the next byte), offset
//            ((c & 31) << 8) + next byte; copy L+2 bytes starting offset+1 bytes
//            back in the output (the source may overlap the destination).
namespace h5lzf {

// Decode `in` into `out`. Returns the decoded length, or 0 on failure: with
// *out_too_small set when `out` ran out of room, otherwise for a malformed
// stream (truncated item, or a back reference before the start of the output).
// Every access is bounds-checked; chunk bytes come from the file.
size_t decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len,
              bool* out_too_small) {
    *out_too_small = false;
    size_t ip = 0, op = 0;
    while (ip < in_len) {
        unsigned ctrl = in[ip++];
        if (ctrl < 32) {                              // literal run
            size_t n = (size_t)ctrl + 1;
            if (in_len - ip < n) return 0;
            if (out_len - op < n) { *out_too_small = true; return 0; }
            std::memcpy(out + op, in + ip, n);
            ip += n; op += n;
        } else {                                      // back reference
            size_t len = ctrl >> 5;
            size_t off = (size_t)(ctrl & 31) << 8;
            if (ip >= in_len) return 0;
            if (len == 7) {
                len += in[ip++];
                if (ip >= in_len) return 0;
            }
            off += in[ip++];
            len += 2;
            if (off >= op) return 0;                  // reference before output start
            if (out_len - op < len) { *out_too_small = true; return 0; }
            size_t ref = op - off - 1;
            for (size_t k = 0; k < len; ++k) out[op++] = out[ref++];
        }
    }
    return op;
}

}  // namespace h5lzf

namespace h5v {

// RAII for HDF5 ids. H5Fclose etc. are idempotent on negative ids, so
// the deleter handles the "open failed" case naturally.
struct H5Closer {
    int (*fn)(hid_t);
    void operator()(hid_t* p) const noexcept {
        if (p) { if (*p >= 0) fn(*p); delete p; }
    }
};
using H5FilePtr  = std::shared_ptr<hid_t>;
template <typename Fn>
static std::unique_ptr<hid_t, H5Closer> own(hid_t id, Fn closer) {
    return std::unique_ptr<hid_t, H5Closer>(new hid_t(id), H5Closer{closer});
}

// Look up a string attribute on an HDF5 object. Returns "" when absent
// or non-string.
std::string read_string_attr(hid_t obj, const char* name) {
    if (H5Aexists(obj, name) <= 0) return std::string{};
    hid_t a = H5Aopen(obj, name, H5P_DEFAULT);
    if (a < 0) return std::string{};
    // H5Aread writes one element per dataspace point, so an array-valued
    // attribute (npoints > 1) would overflow the single-element buffers below
    // (and, in the variable-length case, leak every pointer past the first).
    // We only support a scalar string here; treat anything else as absent.
    hid_t sp = H5Aget_space(a);
    hssize_t npoints = (sp >= 0) ? H5Sget_simple_extent_npoints(sp) : -1;
    if (sp >= 0) H5Sclose(sp);
    if (npoints != 1) { H5Aclose(a); return std::string{}; }
    hid_t t = H5Aget_type(a);
    H5T_class_t cls = H5Tget_class(t);
    std::string out;
    if (cls == H5T_STRING) {
        if (H5Tis_variable_str(t)) {
            char* buf = nullptr;
            hid_t mt = H5Tcopy(H5T_C_S1);
            H5Tset_size(mt, H5T_VARIABLE);
            H5Tset_cset(mt, H5T_CSET_UTF8);
            if (H5Aread(a, mt, &buf) >= 0 && buf) {
                out = buf;
                H5free_memory(buf);
            }
            H5Tclose(mt);
        } else {
            size_t sz = H5Tget_size(t);
            std::string buf(sz, '\0');
            if (H5Aread(a, t, buf.data()) >= 0) {
                size_t n = strnlen(buf.data(), sz);
                out.assign(buf.data(), n);
            }
        }
    }
    H5Tclose(t);
    H5Aclose(a);
    return out;
}

// Read up to two int64s from a "shape"-style attribute (e.g. [n_rows, n_cols])
// into out[2]. H5Aread writes one value per dataspace point, so reading
// straight into a fixed two-slot buffer overflows the stack when a malformed
// or hostile file declares a shape attribute with more than two elements.
// Size the read buffer to the attribute's actual point count and copy back
// only the first two; leave out = {0, 0} for absent / empty / absurd shapes.
static void read_shape2(hid_t obj, const char* name, int64_t out[2]) {
    out[0] = 0; out[1] = 0;
    if (H5Aexists(obj, name) <= 0) return;
    hid_t a = H5Aopen(obj, name, H5P_DEFAULT);
    if (a < 0) return;
    hid_t sp = H5Aget_space(a);
    hssize_t n = (sp >= 0) ? H5Sget_simple_extent_npoints(sp) : -1;
    if (sp >= 0) H5Sclose(sp);
    // A real shape has a handful of dims; reject empty / negative / absurd
    // counts rather than allocate on an attacker-controlled length.
    if (n >= 1 && n <= 1024) {
        std::vector<int64_t> tmp((size_t)n, 0);
        if (H5Aread(a, H5T_NATIVE_INT64, tmp.data()) >= 0) {
            out[0] = tmp[0];
            if (n >= 2) out[1] = tmp[1];
        }
    }
    H5Aclose(a);
}

// Whether a child link exists directly under `parent`.
static bool link_exists(hid_t parent, const char* name) {
    return H5Lexists(parent, name, H5P_DEFAULT) > 0;
}

// Whether the named child is a group (rather than dataset / datatype).
static bool is_group(hid_t parent, const char* name) {
    VV_H5O_INFO_T info;
    if (VV_H5Oget_info_by_name(parent, name, &info, H5O_INFO_BASIC,
                              H5P_DEFAULT) < 0) return false;
    return info.type == H5O_TYPE_GROUP;
}

// Why `dset` cannot be read: the first filter in its pipeline that this HDF5
// build cannot decode (third-party codecs such as LZF or Blosc, which a Python
// writer registers in its own process), else a generic read failure.
static std::string h5_read_why(hid_t dset) {
    std::string why = "read failed";
    hid_t dcpl = H5Dget_create_plist(dset);
    if (dcpl < 0) return why;
    int nf = H5Pget_nfilters(dcpl);
    for (int i = 0; i < nf; ++i) {
        unsigned flags = 0, fcfg = 0;
        size_t nelm = 0;
        char fname[64] = {0};
        H5Z_filter_t id = H5Pget_filter2(dcpl, (unsigned)i, &flags, &nelm, nullptr,
                                         sizeof fname, fname, &fcfg);
        if (id >= 0 && H5Zfilter_avail(id) <= 0) {
            why = "compression filter '" + std::string(fname[0] ? fname : "unnamed") +
                  "' (HDF5 filter id " + std::to_string((int)id) +
                  ") is not available in this build";
            break;
        }
    }
    H5Pclose(dcpl);
    return why;
}

// "'<dataset path>': cannot read HDF5 dataset: <why>".
std::string h5_read_failure(hid_t dset) {
    std::string name = "?";
    ssize_t n = H5Iget_name(dset, nullptr, 0);
    if (n > 0) {
        std::string s((size_t)n + 1, '\0');
        H5Iget_name(dset, s.data(), s.size());
        s.resize((size_t)n);
        name = s;
    }
    return "'" + name + "': cannot read HDF5 dataset: " + h5_read_why(dset);
}

// First dataset-read failure while building the current tab (see build_table).
// A failed H5Dread leaves the caller's zero-initialised buffer untouched, so the
// readers stop and return an error; the failure is also recorded here because
// several callers treat a failed column read as "skip this column" and the
// obs/var row labels are read best-effort — build_table then fails the tab
// instead of showing a table with columns silently zeroed or missing.
static thread_local std::string t_h5_read_error;
// Set by a tab builder that reads only part of a dataset (the preview caps):
// the shown and full shape. Collected by Hdf5Source::build_table, the same way
// as t_h5_read_error, so the cap sites need no extra parameters.
static thread_local PreviewLimit t_h5_preview;
static void h5_note_preview(int64_t shown_rows, int64_t full_rows,
                            int64_t shown_cols, int64_t full_cols) {
    t_h5_preview = PreviewLimit{shown_rows, full_rows, shown_cols, full_cols};
}

// H5Dread that records the first failure in t_h5_read_error.
static herr_t h5_read(hid_t dset, hid_t memtype, hid_t ms, hid_t fs, void* buf) {
    herr_t st = H5Dread(dset, memtype, ms, fs, H5P_DEFAULT, buf);
    if (st < 0 && t_h5_read_error.empty()) t_h5_read_error = h5_read_failure(dset);
    return st;
}

// HDF5 filter callback (H5Z_func_t). h5py records the uncompressed chunk size
// as cd_values[2]; start from it and double the buffer while the chunk decodes
// larger, up to HDF5's 4 GiB chunk limit. Returns the decoded size, 0 on error
// (HDF5 then fails the read, which h5_read reports).
static size_t h5_lzf_filter(unsigned flags, size_t cd_nelmts,
                            const unsigned cd_values[], size_t nbytes,
                            size_t* buf_size, void** buf) {
    if (!(flags & H5Z_FLAG_REVERSE)) return 0;        // no encoder
    const size_t kMaxChunk = (size_t)0xFFFFFFFFu;
    size_t cap = (cd_nelmts >= 3 && cd_values[2] != 0) ? (size_t)cd_values[2]
                                                        : *buf_size;
    if (cap == 0) cap = nbytes * 2 + 64;
    for (;;) {
        void* out = H5allocate_memory(cap, false);
        if (!out) return 0;
        bool too_small = false;
        size_t n = h5lzf::decode(static_cast<const uint8_t*>(*buf), nbytes,
                                 static_cast<uint8_t*>(out), cap, &too_small);
        if (n > 0) {
            H5free_memory(*buf);
            *buf = out;
            *buf_size = cap;
            return n;
        }
        H5free_memory(out);
        if (!too_small || cap >= kMaxChunk) return 0;
        cap = (cap > kMaxChunk / 2) ? kMaxChunk : cap * 2;
    }
}

// Register the LZF decoder with libhdf5 once per process, unless a plugin for
// filter 32000 is already available (e.g. via HDF5_PLUGIN_PATH).
void register_hdf5_filters() {
    static const bool done = [] {
        static const H5Z_class2_t lzf = {
            H5Z_CLASS_T_VERS, (H5Z_filter_t)32000,
            /*encoder_present=*/0, /*decoder_present=*/1,
            "lzf", nullptr, nullptr, h5_lzf_filter,
        };
        if (H5Zfilter_avail(32000) <= 0) H5Zregister(&lzf);
        return true;
    }();
    (void)done;
}

// Human-readable description of an HDF5 datatype.
static std::string dtype_to_string(hid_t t) {
    H5T_class_t cls = H5Tget_class(t);
    size_t size = H5Tget_size(t);
    switch (cls) {
        case H5T_INTEGER: {
            H5T_sign_t s = H5Tget_sign(t);
            return (s == H5T_SGN_NONE ? "uint" : "int") +
                   std::to_string(size * 8);
        }
        case H5T_FLOAT:    return "float" + std::to_string(size * 8);
        case H5T_STRING:   return H5Tis_variable_str(t) ? "string"
                                                          : "string[" +
                              std::to_string(size) + "]";
        case H5T_COMPOUND: return "compound";
        case H5T_ENUM:     return "enum";
        case H5T_REFERENCE:return "ref";
        case H5T_OPAQUE:   return "opaque";
        case H5T_BITFIELD: return "bitfield";
        default:           return "?";
    }
}

// Format a dimension list like "10000 × 2" for the hierarchy table.
static std::string shape_to_string(const std::vector<hsize_t>& dims) {
    std::string s;
    for (size_t i = 0; i < dims.size(); ++i) {
        if (i) s += " \xc3\x97 ";   // ×
        s += std::to_string(dims[i]);
    }
    return s;
}

// Read all immediate children of a group. Order: as returned by HDF5.
static std::vector<std::string> list_children(hid_t group) {
    std::vector<std::string> names;
    H5G_info_t info;
    if (H5Gget_info(group, &info) < 0) return names;
    for (hsize_t i = 0; i < info.nlinks; ++i) {
        ssize_t len = H5Lget_name_by_idx(group, ".", H5_INDEX_NAME,
                                           H5_ITER_INC, i, nullptr, 0,
                                           H5P_DEFAULT);
        if (len <= 0) continue;
        std::string nm((size_t)len, '\0');
        H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i,
                            nm.data(), len + 1, H5P_DEFAULT);
        names.push_back(std::move(nm));
    }
    return names;
}

// ── Generic-HDF5 hierarchy walker ───────────────────────────────────────────
//
// One row per object reachable from the root via H5Lvisit_by_name. Skips
// soft-link cycles. Builds an arrow::Table directly (string columns).

struct HierarchyRow {
    std::string path;
    std::string kind;        // "Group" | "Dataset"
    std::string shape;       // empty for groups
    std::string dtype;       // empty for groups
    int         n_attrs = 0;
};

struct HierarchyState {
    std::vector<HierarchyRow> rows;
    int max_depth = 1024;     // safety
};

static herr_t hierarchy_cb(hid_t loc_id, const char* name,
                            const VV_H5L_INFO_T* /*linfo*/, void* data) {
    auto* st = static_cast<HierarchyState*>(data);
    if ((int)st->rows.size() > 1000000) return -1;  // 1 M nodes hard cap
    VV_H5O_INFO_T info;
    if (VV_H5Oget_info_by_name(loc_id, name, &info, H5O_INFO_BASIC | H5O_INFO_NUM_ATTRS,
                              H5P_DEFAULT) < 0) return 0;
    HierarchyRow row;
    row.path = std::string("/") + name;
    row.n_attrs = (int)info.num_attrs;
    if (info.type == H5O_TYPE_GROUP) {
        row.kind = "Group";
    } else if (info.type == H5O_TYPE_DATASET) {
        row.kind = "Dataset";
        hid_t d = H5Dopen2(loc_id, name, H5P_DEFAULT);
        if (d >= 0) {
            hid_t s = H5Dget_space(d);
            int   nd = H5Sget_simple_extent_ndims(s);
            std::vector<hsize_t> dims((size_t)nd);
            if (nd > 0) H5Sget_simple_extent_dims(s, dims.data(), nullptr);
            row.shape = shape_to_string(dims);
            hid_t t = H5Dget_type(d);
            row.dtype = dtype_to_string(t);
            H5Tclose(t);
            H5Sclose(s);
            H5Dclose(d);
        }
    } else {
        row.kind = "Other";
    }
    st->rows.push_back(std::move(row));
    return 0;
}

static std::shared_ptr<arrow::Table>
build_hierarchy_table(hid_t file_id) {
    HierarchyState st;
    // Add the root.
    HierarchyRow root{"/", "Group", "", "", 0};
    VV_H5O_INFO_T rinfo;
    if (VV_H5Oget_info(file_id, &rinfo, H5O_INFO_BASIC | H5O_INFO_NUM_ATTRS) >= 0)
        root.n_attrs = (int)rinfo.num_attrs;
    st.rows.push_back(std::move(root));
    VV_H5Lvisit(file_id, H5_INDEX_NAME, H5_ITER_NATIVE, hierarchy_cb, &st);
    arrow::StringBuilder b_path, b_kind, b_shape, b_dtype;
    arrow::Int32Builder  b_attrs;
    for (const auto& r : st.rows) {
        (void)b_path.Append(r.path);
        (void)b_kind.Append(r.kind);
        (void)b_shape.Append(r.shape);
        (void)b_dtype.Append(r.dtype);
        (void)b_attrs.Append(r.n_attrs);
    }
    std::shared_ptr<arrow::Array> a_path, a_kind, a_shape, a_dtype, a_attrs;
    (void)b_path.Finish(&a_path);
    (void)b_kind.Finish(&a_kind);
    (void)b_shape.Finish(&a_shape);
    (void)b_dtype.Finish(&a_dtype);
    (void)b_attrs.Finish(&a_attrs);
    auto schema = arrow::schema({
        arrow::field("path",    arrow::utf8()),
        arrow::field("kind",    arrow::utf8()),
        arrow::field("shape",   arrow::utf8()),
        arrow::field("dtype",   arrow::utf8()),
        arrow::field("n_attrs", arrow::int32()),
    });
    return arrow::Table::Make(schema, {a_path, a_kind, a_shape, a_dtype, a_attrs});
}

// Render a small HDF5 dataset's value(s) as a display string for the uns tab:
// scalars and short 1-D arrays show their actual values (joined with ", "),
// anything larger / multi-dimensional shows a "<dtype>  <shape>" descriptor.
// Mirrors read_1d_dataset_table's type handling but also accepts 0-D (scalar)
// datasets — which uns is full of (a title string, an int n_pcs, a float
// threshold) and read_1d_dataset_table rejects.
static std::string h5_value_to_string(hid_t dset, int max_elems = 10) {
    hid_t space = H5Dget_space(dset);
    int nd = H5Sget_simple_extent_ndims(space);
    hssize_t np = H5Sget_simple_extent_npoints(space);
    std::vector<hsize_t> dims(nd > 0 ? (size_t)nd : 0);
    if (nd > 0) H5Sget_simple_extent_dims(space, dims.data(), nullptr);
    H5Sclose(space);
    hid_t t = H5Dget_type(dset);
    H5T_class_t cls = H5Tget_class(t);
    size_t tsz = H5Tget_size(t);
    auto descriptor = [&]() {
        std::string d = dtype_to_string(t);
        if (nd > 0) d += "  " + shape_to_string(dims);
        return d;
    };
    std::string out;
    if (np < 0 || np > max_elems || nd > 1) {
        out = descriptor();
    } else {
        size_t n = (size_t)np;
        // uns is informational: an entry that cannot be decoded shows why in
        // its value cell instead of failing the whole tab.
        herr_t st = 0;
        if (cls == H5T_INTEGER) {
            std::vector<int64_t> buf(n);
            if (n) st = H5Dread(dset, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
            for (size_t i = 0; i < n; ++i) { if (i) out += ", "; out += std::to_string(buf[i]); }
        } else if (cls == H5T_FLOAT) {
            std::vector<double> buf(n);
            if (n) st = H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
            for (size_t i = 0; i < n; ++i) {
                if (i) out += ", ";
                char tmp[32]; std::snprintf(tmp, sizeof tmp, "%.6g", buf[i]); out += tmp;
            }
        } else if (cls == H5T_STRING && H5Tis_variable_str(t)) {
            std::vector<char*> ptrs(n, nullptr);
            hid_t mt = H5Tcopy(H5T_C_S1);
            H5Tset_size(mt, H5T_VARIABLE); H5Tset_cset(mt, H5T_CSET_UTF8);
            if (n) st = H5Dread(dset, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, ptrs.data());
            for (size_t i = 0; i < n; ++i) { if (i) out += ", "; out += ptrs[i] ? ptrs[i] : ""; }
            if (n) { hid_t ms = H5Dget_space(dset);
                     H5Dvlen_reclaim(mt, ms, H5P_DEFAULT, ptrs.data()); H5Sclose(ms); }
            H5Tclose(mt);
        } else if (cls == H5T_STRING) {
            std::vector<char> buf(n * tsz, '\0');
            if (n) st = H5Dread(dset, t, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf.data());
            for (size_t i = 0; i < n; ++i) {
                if (i) out += ", ";
                size_t len = strnlen(buf.data() + i * tsz, tsz);
                out.append(buf.data() + i * tsz, len);
            }
        } else {
            out = descriptor();
        }
        if (st < 0) out = descriptor() + "  (unreadable: " + h5_read_why(dset) + ")";
        if (out.empty() && n == 0) out = "(empty)";
    }
    H5Tclose(t);
    return out;
}

// Walk an AnnData /uns group recursively, collecting (dotted-key, value) rows:
// scalars / short arrays show their values, plain nested dicts recurse, and an
// encoded sub-object (dataframe / categorical / sparse …) shows its
// encoding-type rather than being expanded. Bounded by depth + a row cap.
static void walk_uns(hid_t group, const std::string& prefix,
                     std::vector<std::pair<std::string, std::string>>* rows,
                     int depth) {
    if (depth > 16 || rows->size() >= 10000) return;
    for (const auto& name : list_children(group)) {
        std::string key = prefix.empty() ? name : prefix + "." + name;
        if (is_group(group, name.c_str())) {
            hid_t sub = H5Gopen2(group, name.c_str(), H5P_DEFAULT);
            if (sub < 0) continue;
            std::string enc = read_string_attr(sub, "encoding-type");
            if (enc.empty() || enc == "dict")
                walk_uns(sub, key, rows, depth + 1);    // recurse into plain dicts
            else
                rows->push_back({key, enc});            // dataframe / categorical / …
            H5Gclose(sub);
        } else {
            hid_t d = H5Dopen2(group, name.c_str(), H5P_DEFAULT);
            if (d < 0) continue;
            std::string v = h5_value_to_string(d);
            for (char& c : v) if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            rows->push_back({key, v});
            H5Dclose(d);
        }
    }
}

static std::shared_ptr<arrow::Table> build_uns_table(hid_t uns_group) {
    std::vector<std::pair<std::string, std::string>> rows;
    walk_uns(uns_group, "", &rows, 0);
    arrow::StringBuilder kb, vb;
    for (auto& kv : rows) { (void)kb.Append(kv.first); (void)vb.Append(kv.second); }
    std::shared_ptr<arrow::Array> ka, va;
    (void)kb.Finish(&ka); (void)vb.Finish(&va);
    auto sch = arrow::schema({arrow::field("key",   arrow::utf8()),
                              arrow::field("value", arrow::utf8())});
    return arrow::Table::Make(sch, {ka, va});
}

// ── Forward declarations ────────────────────────────────────────────────────
struct OpenSpec;
class Hdf5Source;
// Preview row caps. AnnData components can be enormous (a Tahoe-100M plate has
// ~4.7M-row obs and a 4.7M×62k X); reading one in full stalls for minutes on a
// slow mount and burns GBs of RAM. Cap every component to a head preview (the
// CSR X path already did); the real row count is still reported in the footer.
static constexpr int64_t kDense2DRowCap   = 1000;   // dense 2-D matrix rows
static constexpr int64_t kDense2DColCap   = 200;    // dense 2-D matrix cols
// A categorical column whose dictionary exceeds this is shown as integer codes
// rather than decoded to strings: a per-cell-unique categorical (e.g. a barcode
// column with millions of categories) would otherwise force reading the whole
// dictionary — minutes over a slow mount — just to render a preview. The default
// (1,000,000) comfortably covers real high-cardinality categoricals.
inline int64_t category_dict_cap() {
    static const int64_t cap = [] {
        if (const char* e = std::getenv("VV_CATEGORY_DICT_CAP")) {
            long long v = std::atoll(e);
            if (v > 0) return (int64_t)v;
        }
        return (int64_t)1000000;
    }();
    return cap;
}
// `row_cap` < 0 means "all rows"; otherwise only the first row_cap rows are
// read (HDF5 hyperslab). The full pre-cap length is reported via full_rows.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_anndata_dataframe(hid_t group, int64_t row_cap = -1,
                       int64_t* full_rows = nullptr);
static arrow::Result<std::shared_ptr<arrow::Table>>
read_2d_dataset_table(hid_t dataset, int64_t row_cap, int64_t col_cap = -1,
                      int64_t* full_rows = nullptr,
                      int64_t* full_cols = nullptr);
static arrow::Result<std::shared_ptr<arrow::Table>>
read_1d_dataset_table(hid_t dataset, int64_t row_cap = -1,
                      int64_t* full_rows = nullptr);
static arrow::Result<std::shared_ptr<arrow::Table>>
read_sparse_preview(hid_t group, int64_t row_cap);
static int64_t h5_len_1d(hid_t d);
static arrow::Result<std::shared_ptr<arrow::Table>>
read_sparse_preview_as(hid_t group, int64_t n_rows, int64_t n_cols, bool is_csr,
                       int64_t row_cap);
struct OpenSpec;
static std::string build_tenx_table(hid_t file_id, const OpenSpec& spec,
                                    int64_t row_cap,
                                    std::shared_ptr<arrow::Table>* out,
                                    std::string* footer);
static std::string build_loom_table(hid_t file_id, const OpenSpec& spec,
                                    int64_t row_cap,
                                    std::shared_ptr<arrow::Table>* out,
                                    std::string* footer);
// Label an AnnData X preview with its obs (row) / var (column) identifiers.
// Which AnnData axes a dense 2-D tab is indexed by. X and layers/* are
// (n_obs x n_var), but obsm/* is (n_obs x d) and varm/* is (n_var x d), where
// d is an embedding dimension — not a gene. Labelling all three the same way
// put gene names on UMAP coordinates.
enum class AnnMatrixAxes { ObsByVar, ObsByDim, VarByDim,
                           ObsByRawVar };   // raw/X: genes from raw/var

static void apply_anndata_matrix_labels(hid_t file_id, const std::string& root, AnnMatrixAxes axes,
                                        const std::string& key,
                                        std::shared_ptr<arrow::Table>* tbl);

// Tab specification: identifies which named object inside the HDF5 file
// this tab should view, and how to render it.
struct OpenSpec {
    enum class Kind { Hierarchy, DataFrame, Matrix2D, Sparse, Dataset1D,
                      TenxMatrix, TenxFeatures, TenxBarcodes, LoomMatrix, LoomAttrs,
                      Dataset2D, Summary, Uns,
                      EdgeList };   // obsp / varp graph: streamed (i, j, weight)
    Kind        kind;
    std::string h5_path;     // group / dataset path inside the file
    std::string display;     // tab label
    std::string footer_hint; // additional footer text
    // Matrix2D only: how to label the axes, and the obsm/varm key whose name
    // the dimension columns are derived from ("X_umap" -> X_umap1, X_umap2).
    AnnMatrixAxes axes = AnnMatrixAxes::ObsByVar;
    std::string   key;       // EdgeList: the labelled axis, "obs" or "var"
    bool          long_form = false;  // --matrix long: shown as entry rows
};

// The AnnData a dataset belongs to: "/mod/<name>" for a modality of a MuData
// (.h5mu) file, "" for a plain AnnData. Its obs / var label the dataset.
static std::string anndata_root(const std::string& h5_path) {
    if (h5_path.rfind("/mod/", 0) != 0) return "";
    const size_t e = h5_path.find('/', 5);
    return e == std::string::npos ? h5_path : h5_path.substr(0, e);
}

// An obsp / varp graph tab streams its edges from the sparse matrix rather than
// materialising a table, so it is a separate source class (H5EdgeListSource,
// defined below); Hdf5Source creates it for Kind::EdgeList siblings.
static std::unique_ptr<TabularSource> make_edge_list_source(const std::string& path,
                                                            H5FilePtr file,
                                                            const OpenSpec& spec);

// Read the AnnData layout and produce one OpenSpec per visible tab.
static std::vector<OpenSpec> scan_anndata(hid_t file_id, const std::string& root = "");
// Same but for a generic HDF5 file (one spec per 1D/2D dataset).
static std::vector<OpenSpec> scan_generic(hid_t file_id);

// ── Hdf5Source: one tab's view of the file ──────────────────────────────────

struct OpenSpec;
static std::unique_ptr<TabularSource> make_h5_matrix_stream(const H5FilePtr& file,
                                                           const std::string& path,
                                                           const OpenSpec& spec);
static std::unique_ptr<TabularSource> make_h5_long_matrix(const H5FilePtr& file,
                                                         const std::string& path,
                                                         const OpenSpec& spec);
class Hdf5Source : public WorkbookSource {
    H5FilePtr   file_;
    std::string h5_path_;
    OpenSpec    spec_;
    // Every sibling tab's OpenSpec, shared across the original + sibling
    // instances. The original holds the full list; siblings keep it
    // alive for symmetry / future drill-down.
    std::shared_ptr<std::vector<OpenSpec>> all_specs_;
    // Sibling specs *other* than this one — emitted via
    // open_sibling_sheets().
    std::vector<OpenSpec> siblings_;
    // Lazy materialization: a sibling tab's table is built only on first
    // access (ensure_built). The eager constructor sets built_ = true so its
    // overrides are no-ops; the lazy constructor leaves it false.
    mutable bool built_ = true;
    // Set when the lazy build fails: the tab then shows a one-cell error table,
    // and read_status() reports the failure so a scripted export exits non-zero.
    mutable arrow::Status build_status_;
    // Row cap for DataFrame (obs/var) tabs: the preview cap for the TUI/table
    // view, or -1 (all) / an explicit -n for a delimited dump — set from cfg in
    // open_source and inherited by sibling tabs. Matrix / sparse X ignore it.
    int64_t df_row_cap_ = kDataFrameRowCap;
    // The shown / full shape when this tab is a capped preview (preview_limit).
    mutable PreviewLimit limit_;

    Hdf5Source(std::shared_ptr<arrow::Table> tbl,
                std::string path,
                std::string footer,
                H5FilePtr file,
                OpenSpec spec,
                std::shared_ptr<std::vector<OpenSpec>> all_specs,
                std::vector<OpenSpec> siblings,
                int64_t df_row_cap = kDataFrameRowCap)
        : WorkbookSource(std::move(tbl), std::move(path), std::move(footer)),
          file_(std::move(file)),
          h5_path_(spec.h5_path),
          spec_(std::move(spec)),
          all_specs_(std::move(all_specs)),
          siblings_(std::move(siblings)),
          df_row_cap_(df_row_cap) {}

    // Lazy constructor: no table yet. ensure_built() reads it from `spec` on
    // first access, so opening a multi-component file (e.g. AnnData) doesn't
    // materialise every component up-front — only the tab(s) actually viewed.
    Hdf5Source(std::string path,
                H5FilePtr file,
                OpenSpec spec,
                std::shared_ptr<std::vector<OpenSpec>> all_specs,
                int64_t df_row_cap = kDataFrameRowCap)
        : WorkbookSource(nullptr, std::move(path), std::string{}),
          file_(std::move(file)),
          h5_path_(spec.h5_path),
          spec_(std::move(spec)),
          all_specs_(std::move(all_specs)),
          built_(false),
          df_row_cap_(df_row_cap) {}

    // Build this tab's table from its spec on first access (lazy ctor only).
    void ensure_built() const {
        if (built_) return;
        built_ = true;
        auto* self = const_cast<Hdf5Source*>(this);
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        std::string err = build_table(*file_, spec_, &tbl, &footer, df_row_cap_,
                                      &self->limit_);
        if (!err.empty() || !tbl) {
            // Surface the failure as a one-cell table instead of crashing a
            // null-table access (matches the eager path's graceful skip).
            std::string msg = err.empty()
                ? ("'" + spec_.h5_path + "': decoded to empty table") : err;
            arrow::StringBuilder b; (void)b.Append(msg);
            std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
            tbl = arrow::Table::Make(
                arrow::schema({arrow::field("error", arrow::utf8())}), {a});
            footer = "Format: HDF5 " + spec_.display + "  |  error: " + msg;
            self->build_status_ = arrow::Status::IOError(msg);
        }
        self->replace_table(std::move(tbl), std::move(footer));
    }

    // Resolve an OpenSpec to a populated arrow::Table. A dataset read that
    // fails anywhere during the build — including a column a reader would
    // otherwise skip, or the obs/var row labels — fails the whole tab with
    // that read's message rather than returning a partial or zeroed table.
    static std::string build_table(hid_t file_id,
                                    const OpenSpec& spec,
                                    std::shared_ptr<arrow::Table>* out,
                                    std::string* footer,
                                    int64_t df_row_cap = kDataFrameRowCap,
                                    PreviewLimit* limit = nullptr) {
        t_h5_read_error.clear();
        t_h5_preview = PreviewLimit{};
        std::string err = build_table_impl(file_id, spec, out, footer, df_row_cap);
        if (limit) *limit = t_h5_preview;
        std::string read_err;
        read_err.swap(t_h5_read_error);
        if (!read_err.empty()) {
            out->reset();
            return read_err;
        }
        return err;
    }

    static std::string build_table_impl(hid_t file_id,
                                         const OpenSpec& spec,
                                         std::shared_ptr<arrow::Table>* out,
                                         std::string* footer,
                                         int64_t df_row_cap) {
        switch (spec.kind) {
            case OpenSpec::Kind::Hierarchy: {
                *out = build_hierarchy_table(file_id);
                *footer = "Format: HDF5 (hierarchy)";
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                return "";
            }
            case OpenSpec::Kind::Summary: {
                // The summary table is built directly by scan_anndata
                // and stuffed into spec.footer_hint as JSON-ish text;
                // we just turn it back into rows here.
                arrow::StringBuilder kb, vb;
                std::stringstream ss(spec.footer_hint);
                std::string line;
                while (std::getline(ss, line)) {
                    auto eq = line.find('\t');
                    if (eq == std::string::npos) continue;
                    (void)kb.Append(line.substr(0, eq));
                    (void)vb.Append(line.substr(eq + 1));
                }
                std::shared_ptr<arrow::Array> ka, va;
                (void)kb.Finish(&ka); (void)vb.Finish(&va);
                auto sch = arrow::schema({
                    arrow::field("key",   arrow::utf8()),
                    arrow::field("value", arrow::utf8())});
                *out = arrow::Table::Make(sch, {ka, va});
                *footer = spec.footer_hint.rfind("format\tMuData\n", 0) == 0
                              ? "Format: MuData (summary)" : "Format: AnnData (summary)";
                return "";
            }
            case OpenSpec::Kind::Uns: {
                hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (g < 0) return "Cannot open group " + spec.h5_path;
                *out = build_uns_table(g);
                H5Gclose(g);
                *footer = "Format: AnnData (uns)  |  " +
                          std::to_string(*out ? (*out)->num_rows() : 0) +
                          " entries";
                return "";
            }
            case OpenSpec::Kind::DataFrame: {
                hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (g < 0) return "Cannot open group " + spec.h5_path;
                int64_t full = 0;
                // df_row_cap is the preview cap for the TUI/table view, or -1
                // (all rows) / an explicit -n in delimited export — see
                // open_source. Only obs/var (DataFrame) is uncapped on export;
                // matrix / sparse X stay bounded below.
                auto r = read_anndata_dataframe(g, df_row_cap, &full);
                H5Gclose(g);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                int64_t shown = *out ? (*out)->num_rows() : 0;
                int64_t ncol  = *out ? (*out)->num_columns() : 0;
                *footer = "Format: AnnData " + spec.display +
                          "  |  Cols: " + std::to_string(ncol);
                h5_note_preview(shown, full, ncol, ncol);
                if (shown < full)   // preview note so the cap isn't read as the real size
                    *footer += "  |  preview: first " + std::to_string(shown) +
                               " of " + std::to_string(full) + " rows";
                else
                    *footer += "  |  Rows: " + std::to_string(shown);
                return "";
            }
            case OpenSpec::Kind::Matrix2D:
            case OpenSpec::Kind::Dataset2D: {
                hid_t d = H5Dopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (d < 0) return "Cannot open dataset " + spec.h5_path;
                int64_t fr = 0, fc = 0;
                auto r = read_2d_dataset_table(d, kDense2DRowCap, kDense2DColCap,
                                               &fr, &fc);
                H5Dclose(d);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                *footer = "Format: HDF5 2D " + spec.display;
                // Note any preview truncation so the cap isn't mistaken for the
                // real shape.
                int64_t sr = *out ? (*out)->num_rows() : 0;
                int64_t sc = *out ? (*out)->num_columns() : 0;
                h5_note_preview(sr, fr, sc, fc);
                if (sr < fr || sc < fc) {
                    std::string note = "preview: ";
                    if (sr < fr)
                        note += "first " + std::to_string(sr) + " of " +
                                std::to_string(fr) + " rows";
                    if (sr < fr && sc < fc) note += ", ";
                    if (sc < fc)
                        note += "first " + std::to_string(sc) + " of " +
                                std::to_string(fc) + " cols";
                    *footer += "  |  " + note;
                }
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                // A dense AnnData matrix (Matrix2D, never generic Dataset2D)
                // gets obs/var identifiers, per its own axes.
                if (spec.kind == OpenSpec::Kind::Matrix2D)
                    apply_anndata_matrix_labels(file_id, anndata_root(spec.h5_path), spec.axes, spec.key, out);
                return "";
            }
            case OpenSpec::Kind::Dataset1D: {
                hid_t d = H5Dopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (d < 0) return "Cannot open dataset " + spec.h5_path;
                // Cap the read like an obs/var column (df_row_cap): a generic
                // HDF5 1-D dataset can be millions of elements (e.g. a per-read
                // array), so the TUI / table view previews the head, while a
                // mode that needs every row reads it all (see open_source).
                int64_t full = 0;
                auto r = read_1d_dataset_table(d, df_row_cap, &full);
                H5Dclose(d);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                *footer = "Format: HDF5 1D " + spec.display;
                int64_t shown = *out ? (*out)->num_rows() : 0;
                h5_note_preview(shown, full, 1, 1);
                if (shown < full)   // preview note so the cap isn't read as the real size
                    *footer += "  |  preview: first " + std::to_string(shown) +
                               " of " + std::to_string(full) + " rows";
                else
                    *footer += "  |  Rows: " + std::to_string(shown);
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                return "";
            }
            case OpenSpec::Kind::TenxMatrix:
            case OpenSpec::Kind::TenxFeatures:
            case OpenSpec::Kind::TenxBarcodes:
                return build_tenx_table(file_id, spec, df_row_cap, out, footer);
            case OpenSpec::Kind::LoomMatrix:
            case OpenSpec::Kind::LoomAttrs:
                return build_loom_table(file_id, spec, df_row_cap, out, footer);
            case OpenSpec::Kind::EdgeList:   // streamed by H5EdgeListSource
                return "graph '" + spec.h5_path + "' is read as an edge list";
            case OpenSpec::Kind::Sparse: {
                hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
                if (g < 0) return "Cannot open sparse group " + spec.h5_path;
                auto r = read_sparse_preview(g, 1000);
                // The full shape for preview_limit: the `shape` attribute,
                // with the compressed axis clamped to what indptr can describe
                // (the attribute is untrusted; the reader clamps the same way).
                int64_t shape[2] = {0, 0};
                read_shape2(g, "shape", shape);
                const bool csr = read_string_attr(g, "encoding-type") == "csr_matrix";
                if (hid_t ip = H5Dopen2(g, "indptr", H5P_DEFAULT); ip >= 0) {
                    int64_t& major = csr ? shape[0] : shape[1];
                    major = std::min<int64_t>(major, std::max<int64_t>(0, h5_len_1d(ip) - 1));
                    H5Dclose(ip);
                }
                H5Gclose(g);
                if (!r.ok()) return r.status().ToString();
                *out = *r;
                h5_note_preview((*out)->num_rows(), std::max<int64_t>(shape[0], (*out)->num_rows()),
                                (*out)->num_columns(), std::max<int64_t>(shape[1], (*out)->num_columns()));
                *footer = "Format: AnnData " + spec.display +
                          "  |  preview: first " +
                          std::to_string((*out)->num_rows()) + " rows";
                if (!spec.footer_hint.empty())
                    *footer += "  |  " + spec.footer_hint;
                // X and layers/* are (n_obs x n_var): columns named by var
                // (genes), obs (cells) row labels prepended. A sparse obsm /
                // varm entry is labelled per its own axes.
                apply_anndata_matrix_labels(file_id, anndata_root(spec.h5_path), spec.axes, spec.key, out);
                return "";
            }
        }
        return "Unknown OpenSpec kind";
    }

    static std::string build_one(const std::string& path,
                                  H5FilePtr file,
                                  OpenSpec spec,
                                  std::shared_ptr<std::vector<OpenSpec>> all,
                                  std::vector<OpenSpec> siblings,
                                  std::unique_ptr<Hdf5Source>* out,
                                  int64_t df_row_cap = kDataFrameRowCap) {
        std::shared_ptr<arrow::Table> tbl;
        std::string footer;
        PreviewLimit limit;
        std::string err = build_table(*file, spec, &tbl, &footer, df_row_cap, &limit);
        if (!err.empty()) return err;
        if (!tbl)
            return "'" + spec.h5_path + "': decoded to empty table";
        if (!siblings.empty()) {
            footer += "  |  +" + std::to_string(siblings.size()) +
                       " more tab(s)";
        }
        out->reset(new Hdf5Source(std::move(tbl), path, std::move(footer),
                                    std::move(file), std::move(spec),
                                    std::move(all), std::move(siblings),
                                    df_row_cap));
        (*out)->limit_ = limit;
        return "";
    }

public:
    // `matrix_long`: --matrix long — matrix tabs open as entry rows.
    static std::string open_first(const std::string& path,
                                    std::unique_ptr<Hdf5Source>* out,
                                    int64_t df_row_cap = kDataFrameRowCap,
                                    bool matrix_long = false);

    // tab_label() reads only the spec — no build, so the tab strip and the
    // --tab selector can list/match components without materialising them.
    std::string tab_label() const override { return spec_.display; }

    arrow::Status read_status() const override {
        ensure_built(); return build_status_;
    }

    // Data accessors force the lazy build first; for an eagerly-built source
    // (built_ == true) ensure_built() is a no-op.
    std::shared_ptr<arrow::Schema> schema() const override {
        ensure_built(); return MemoryTableSource::schema();
    }
    int64_t total_rows() const override {
        ensure_built(); return MemoryTableSource::total_rows();
    }
    int num_chunks() const override {
        ensure_built(); return MemoryTableSource::num_chunks();
    }
    ChunkMeta chunk_meta(int i) const override {
        ensure_built(); return MemoryTableSource::chunk_meta(i);
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols,
                             std::shared_ptr<arrow::Table>* out) override {
        ensure_built(); return MemoryTableSource::read_chunk(i, cols, out);
    }
    std::string footer() const override {
        ensure_built(); return MemoryTableSource::footer();
    }
    std::vector<std::string> hidden_for_display() const override {
        ensure_built(); return MemoryTableSource::hidden_for_display();
    }
    PreviewLimit preview_limit() const override {
        ensure_built(); return limit_;
    }
    // A capped dense matrix or CSR preview streams in full for an export.
    std::unique_ptr<TabularSource> full_matrix() const override {
        ensure_built();
        if (!limit_.capped()) return nullptr;
        return make_h5_matrix_stream(file_, path(), spec_);
    }

    std::vector<std::unique_ptr<TabularSource>>
    open_sibling_sheets() const override {
        std::vector<std::unique_ptr<TabularSource>> result;
        // Construct each sibling lazily: its table is read only when the tab is
        // first viewed (ensure_built), so opening a 14-component AnnData file
        // over a slow mount doesn't read every component up-front.
        for (const auto& sp : siblings_) {
            if (sp.kind == OpenSpec::Kind::EdgeList) {
                result.push_back(make_edge_list_source(path(), file_, sp));
                continue;
            }
            if (sp.long_form)
                if (auto lm = make_h5_long_matrix(file_, path(), sp)) {
                    result.push_back(std::move(lm));
                    continue;
                }
            result.push_back(std::unique_ptr<TabularSource>(
                new Hdf5Source(path(), file_, sp, all_specs_, df_row_cap_)));
        }
        return result;
    }
};

std::string open_hdf5_source(const std::string& path, std::unique_ptr<TabularSource>* out,
                             int64_t df_row_cap, bool matrix_long) {
    std::unique_ptr<Hdf5Source> s;
    std::string e = Hdf5Source::open_first(path, &s, df_row_cap, matrix_long);
    if (e.empty()) *out = std::move(s);
    return e;
}

// ── Read helpers — defined after Hdf5Source so they can be referenced
// from build_table. ─────────────────────────────────────────────────────────

// Read a 1-D dataset as a single-column Arrow table. Only the first `row_cap`
// elements are read (a hyperslab) when row_cap >= 0; the full length is
// reported via full_rows. Bounds the read for huge obs/var columns.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_1d_dataset_table(hid_t dset, int64_t row_cap, int64_t* full_rows) {
    hid_t space = H5Dget_space(dset);
    int nd = H5Sget_simple_extent_ndims(space);
    if (nd != 1) {
        H5Sclose(space);
        return arrow::Status::Invalid("expected 1-D dataset");
    }
    hsize_t dim;
    H5Sget_simple_extent_dims(space, &dim, nullptr);
    H5Sclose(space);
    if (full_rows) *full_rows = (int64_t)dim;
    hsize_t n = dim;
    if (row_cap >= 0 && (hsize_t)row_cap < dim) n = (hsize_t)row_cap;

    // Read the first `n` elements of `dset` into `buf` via a hyperslab; `ms`
    // (the matching memory dataspace) is returned so vlen strings can be
    // reclaimed against it.
    herr_t rd_st = 0;   // a failed read is reported after the type branches
    auto read_first_n = [&](hid_t memtype, void* buf) -> hid_t {
        hid_t fs = H5Dget_space(dset);
        hsize_t start = 0, count = n;
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
        hid_t ms = H5Screate_simple(1, &count, nullptr);
        if (n > 0 && h5_read(dset, memtype, ms, fs, buf) < 0) rd_st = -1;
        H5Sclose(fs);
        return ms;   // caller closes
    };

    hid_t t = H5Dget_type(dset);
    H5T_class_t cls = H5Tget_class(t);
    size_t tsz = H5Tget_size(t);
    arrow::FieldVector fields = { arrow::field("value", arrow::utf8()) };
    std::shared_ptr<arrow::Array> arr;
    if (cls == H5T_INTEGER) {
        std::vector<int64_t> buf((size_t)n);
        hid_t ms = read_first_n(H5T_NATIVE_INT64, buf.data()); H5Sclose(ms);
        arrow::Int64Builder b;
        for (auto v : buf) (void)b.Append(v);
        (void)b.Finish(&arr);
        fields[0] = arrow::field("value", arrow::int64());
    } else if (cls == H5T_FLOAT) {
        std::vector<double> buf((size_t)n);
        hid_t ms = read_first_n(H5T_NATIVE_DOUBLE, buf.data()); H5Sclose(ms);
        arrow::DoubleBuilder b;
        for (auto v : buf) (void)b.Append(v);
        (void)b.Finish(&arr);
        fields[0] = arrow::field("value", arrow::float64());
    } else if (cls == H5T_STRING) {
        arrow::StringBuilder b;
        if (H5Tis_variable_str(t)) {
            std::vector<char*> ptrs((size_t)n, nullptr);
            hid_t mt = H5Tcopy(H5T_C_S1);
            H5Tset_size(mt, H5T_VARIABLE);
            H5Tset_cset(mt, H5T_CSET_UTF8);
            hid_t ms = read_first_n(mt, ptrs.data());
            for (auto* p : ptrs) (void)b.Append(p ? std::string(p) : std::string{});
            // Free the vlens read into the first-n buffer.
            if (n > 0) H5Dvlen_reclaim(mt, ms, H5P_DEFAULT, ptrs.data());
            H5Sclose(ms);
            H5Tclose(mt);
        } else {
            std::vector<char> buf((size_t)n * tsz, '\0');
            hid_t ms = read_first_n(t, buf.data()); H5Sclose(ms);
            for (hsize_t i = 0; i < n; ++i) {
                size_t len = strnlen(buf.data() + i * tsz, tsz);
                (void)b.Append(std::string(buf.data() + i * tsz, len));
            }
        }
        (void)b.Finish(&arr);
    } else if (cls == H5T_ENUM) {
        // Map enum codes to their member names — h5py stores a bool column as
        // an int enum {FALSE=0, TRUE=1}, which otherwise hit the "?" fallback.
        //
        // H5Tget_member_value writes one value in the enum's BASE type, i.e.
        // H5Tget_size(t) bytes, and that width comes from the file: an enum
        // whose base is a 256-byte integer is well-formed HDF5 and makes the
        // library write 256 bytes. Reading into a bare int64 therefore let a
        // crafted file scribble over the stack with bytes it chose. Size the
        // buffer from the type, and convert to int64 through HDF5 instead of
        // assuming a little-endian layout. H5Tconvert works in place and needs
        // room for the wider of source and destination.
        std::map<int64_t, std::string> names;
        int nmem = H5Tget_nmembers(t);
        hid_t base = H5Tget_super(t);
        size_t esz = H5Tget_size(t);
        for (int m = 0; base >= 0 && esz > 0 && m < nmem; ++m) {
            std::vector<unsigned char> raw(std::max(esz, sizeof(int64_t)), 0);
            if (H5Tget_member_value(t, (unsigned)m, raw.data()) < 0) continue;
            if (H5Tconvert(base, H5T_NATIVE_INT64, 1, raw.data(), nullptr,
                           H5P_DEFAULT) < 0)
                continue;   // value does not fit an int64 — it cannot match a
                            // datum read below as int64 either, so drop the name
            int64_t val = 0;
            std::memcpy(&val, raw.data(), sizeof(val));
            char* mn = H5Tget_member_name(t, (unsigned)m);
            if (mn) { names[val] = mn; H5free_memory(mn); }
        }
        if (base >= 0) H5Tclose(base);
        std::vector<int64_t> buf((size_t)n);
        hid_t ms = read_first_n(H5T_NATIVE_INT64, buf.data()); H5Sclose(ms);
        // h5py's bool: exactly {FALSE = 0, TRUE = 1}. A boolean column, not
        // the text "TRUE" / "FALSE".
        const bool h5py_bool = names.size() == 2 && names.count(0) && names.count(1) &&
                               names[0] == "FALSE" && names[1] == "TRUE";
        if (h5py_bool) {
            arrow::BooleanBuilder b;
            for (auto v : buf) (void)b.Append(v != 0);
            (void)b.Finish(&arr);
            fields[0] = arrow::field("value", arrow::boolean());
        } else {
            arrow::StringBuilder b;
            for (auto v : buf) {
                auto it = names.find(v);
                (void)b.Append(it != names.end() ? it->second : std::to_string(v));
            }
            (void)b.Finish(&arr);
        }
    } else {
        // Fallback: unsupported type (compound, opaque, …).
        arrow::StringBuilder b;
        for (hsize_t i = 0; i < n; ++i) (void)b.Append("?");
        (void)b.Finish(&arr);
    }
    H5Tclose(t);
    if (rd_st < 0) return arrow::Status::IOError(h5_read_failure(dset));
    auto sch = arrow::schema(fields);
    return arrow::Table::Make(sch, {arr});
}

// Read a 2-D numeric dataset as an Arrow table. Columns are named col0,
// col1, … unless the dataset has a "column_names" attribute. row_cap / col_cap
// < 0 mean "all"; only the first row_cap rows and col_cap columns are read
// (the corner hyperslab), bounding memory. The full pre-cap dimensions are
// reported through full_rows / full_cols when those pointers are non-null.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_2d_dataset_table(hid_t dset, int64_t row_cap, int64_t col_cap,
                      int64_t* full_rows, int64_t* full_cols) {
    hid_t space = H5Dget_space(dset);
    int nd = H5Sget_simple_extent_ndims(space);
    if (nd != 2) {
        H5Sclose(space);
        return arrow::Status::Invalid("expected 2-D dataset");
    }
    hsize_t dims[2];
    H5Sget_simple_extent_dims(space, dims, nullptr);
    int64_t n_rows = (int64_t)dims[0];
    int64_t n_cols = (int64_t)dims[1];
    if (full_rows) *full_rows = n_rows;
    if (full_cols) *full_cols = n_cols;
    if (row_cap > 0 && row_cap < n_rows) n_rows = row_cap;
    if (col_cap > 0 && col_cap < n_cols) n_cols = col_cap;
    H5Sclose(space);

    hid_t t = H5Dget_type(dset);
    H5T_class_t cls = H5Tget_class(t);
    H5Tclose(t);

    // Build column names from the dataset's "column_names" attribute if
    // present (AnnData uses it for some embeddings).
    std::vector<std::string> names((size_t)n_cols);
    for (int64_t c = 0; c < n_cols; ++c) names[(size_t)c] = "col" + std::to_string(c);

    arrow::FieldVector fields((size_t)n_cols);
    std::vector<std::shared_ptr<arrow::Array>> cols((size_t)n_cols);
    if (cls == H5T_FLOAT) {
        std::vector<double> buf((size_t)(n_rows * n_cols));
        hid_t fs = H5Dget_space(dset);
        hsize_t start[2] = {0, 0};
        hsize_t count[2] = {(hsize_t)n_rows, (hsize_t)n_cols};
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr);
        hid_t ms = H5Screate_simple(2, count, nullptr);
        herr_t st = h5_read(dset, H5T_NATIVE_DOUBLE, ms, fs, buf.data());
        H5Sclose(ms); H5Sclose(fs);
        if (st < 0) return arrow::Status::IOError(h5_read_failure(dset));
        for (int64_t c = 0; c < n_cols; ++c) {
            arrow::DoubleBuilder b;
            for (int64_t r = 0; r < n_rows; ++r)
                (void)b.Append(buf[(size_t)(r * n_cols + c)]);
            (void)b.Finish(&cols[(size_t)c]);
            fields[(size_t)c] = arrow::field(names[(size_t)c], arrow::float64());
        }
    } else if (cls == H5T_INTEGER) {
        std::vector<int64_t> buf((size_t)(n_rows * n_cols));
        hid_t fs = H5Dget_space(dset);
        hsize_t start[2] = {0, 0};
        hsize_t count[2] = {(hsize_t)n_rows, (hsize_t)n_cols};
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr);
        hid_t ms = H5Screate_simple(2, count, nullptr);
        herr_t st = h5_read(dset, H5T_NATIVE_INT64, ms, fs, buf.data());
        H5Sclose(ms); H5Sclose(fs);
        if (st < 0) return arrow::Status::IOError(h5_read_failure(dset));
        for (int64_t c = 0; c < n_cols; ++c) {
            arrow::Int64Builder b;
            for (int64_t r = 0; r < n_rows; ++r)
                (void)b.Append(buf[(size_t)(r * n_cols + c)]);
            (void)b.Finish(&cols[(size_t)c]);
            fields[(size_t)c] = arrow::field(names[(size_t)c], arrow::int64());
        }
    } else {
        // String / compound 2-D: not supported in v1, emit empty.
        for (int64_t c = 0; c < n_cols; ++c) {
            arrow::StringBuilder b;
            (void)b.Finish(&cols[(size_t)c]);
            fields[(size_t)c] = arrow::field(names[(size_t)c], arrow::utf8());
        }
    }
    return arrow::Table::Make(arrow::schema(fields), cols);
}

// Is the mask true (= NA) at row i? The mask is an HDF5 bool, which vv decodes
// to its enum member name ("TRUE"/"FALSE") — so accept the string form as well
// as a genuine Arrow boolean.
static bool anndata_mask_bit(const arrow::Array& m, int64_t i) {
    if (i >= m.length() || m.IsNull(i)) return false;
    if (m.type_id() == arrow::Type::BOOL)
        return static_cast<const arrow::BooleanArray&>(m).Value(i);
    std::string s = cell_to_string(m, i);
    return s == "TRUE" || s == "True" || s == "true" || s == "1";
}

// Null out `values` where the `mask` is true (anndata's nullable encodings: mask
// bit set = NA). Returns `values` unchanged when there are no NAs (the common
// case) or the value array isn't a string array.
static std::shared_ptr<arrow::Array> anndata_apply_null_mask(
        const std::shared_ptr<arrow::Array>& values,
        const std::shared_ptr<arrow::Array>& mask) {
    if (!mask) return values;
    bool any = false;
    for (int64_t i = 0; i < mask->length(); ++i)
        if (anndata_mask_bit(*mask, i)) { any = true; break; }
    if (!any) return values;
    auto vs = std::dynamic_pointer_cast<arrow::StringArray>(values);
    if (!vs) return values;   // large_utf8 etc. — leave values, skip the mask
    arrow::StringBuilder b;
    for (int64_t i = 0; i < vs->length(); ++i) {
        if (anndata_mask_bit(*mask, i) || vs->IsNull(i)) (void)b.AppendNull();
        else (void)b.Append(vs->GetString(i));
    }
    std::shared_ptr<arrow::Array> out; (void)b.Finish(&out); return out;
}

// Decode an open `nullable-string-array` group (anndata >= 0.13's on-disk form
// for string columns / the DataFrame _index): a `values` string dataset plus a
// boolean `mask` for NA. Returns the resulting string array.
static arrow::Result<std::shared_ptr<arrow::Array>>
read_nullable_string_array(hid_t sub, int64_t cap, int64_t* full_rows) {
    if (!link_exists(sub, "values"))
        return arrow::Status::Invalid("nullable-string-array: no 'values'");
    hid_t vd = H5Dopen2(sub, "values", H5P_DEFAULT);
    if (vd < 0) return arrow::Status::Invalid("nullable-string-array: open 'values'");
    auto vt = read_1d_dataset_table(vd, cap, full_rows);
    H5Dclose(vd);
    if (!vt.ok() || (*vt)->num_columns() == 0)
        return arrow::Status::Invalid("nullable-string-array: empty 'values'");
    std::shared_ptr<arrow::Array> arr = (*vt)->column(0)->chunk(0);
    if (link_exists(sub, "mask")) {
        hid_t md = H5Dopen2(sub, "mask", H5P_DEFAULT);
        if (md >= 0) {
            auto mt = read_1d_dataset_table(md, cap, nullptr);
            H5Dclose(md);
            if (mt.ok() && (*mt)->num_columns() > 0)
                arr = anndata_apply_null_mask(arr, (*mt)->column(0)->chunk(0));
        }
    }
    return arr;
}

// Read AnnData's obs / var DataFrame layout — one column per non-special
// child link, categoricals expanded via the codes / categories sub-group.

// Number of elements in a 1-D HDF5 dataset (0 if not rank-1 / on error).
static int64_t h5_len_1d(hid_t d);

// Reserved children of an obs/var group that are metadata, not columns.
// anndata < 0.8 parks every categorical's lookup table in a `__categories`
// group beside the columns; the reader skipped `__categories__` (with the
// trailing underscores), which is a name anndata has never written, so the
// real one fell through and was counted as a column.
static bool anndata_reserved_child(const std::string& name) {
    return name == "__categories" || name == "__categories__";
}

// Number of real columns in an obs/var group — i.e. children minus the
// reserved ones. H5Gget_info's nlinks counts everything.
static int64_t anndata_column_count(hid_t group) {
    int64_t n = 0;
    for (const auto& c : list_children(group))
        if (!anndata_reserved_child(c)) ++n;
    return n;
}

// A scalar boolean attribute (anndata writes `ordered` as a numpy bool, i.e.
// an int8 / enum); false when absent or unreadable.
static bool read_bool_attr(hid_t obj, const char* name) {
    if (H5Aexists(obj, name) <= 0) return false;
    hid_t a = H5Aopen(obj, name, H5P_DEFAULT);
    if (a < 0) return false;
    int8_t v = 0;
    const bool ok = H5Aread(a, H5T_NATIVE_INT8, &v) >= 0;
    H5Aclose(a);
    return ok && v != 0;
}

// An AnnData categorical as an Arrow dictionary column: the codes are the
// indices and the categories keep their own type (strings, booleans,
// numbers), so a categorical of booleans is category[bool], distinct from
// category[string] and from a plain bool column. Shared by both encodings:
// the modern {codes, categories} group and the pre-0.8 "integer dataset +
// __categories/<name>" layout. Out-of-range and negative codes (anndata's
// -1 = missing) become nulls.
static std::shared_ptr<arrow::Array> anndata_decode_codes(
        const std::shared_ptr<arrow::Array>& codes_arr,
        const std::shared_ptr<arrow::Array>& cats_arr, bool ordered = false) {
    auto codes_i = std::dynamic_pointer_cast<arrow::Int64Array>(codes_arr);
    if (!codes_i || !cats_arr) return nullptr;
    const int64_t n = codes_i->length(), nc = cats_arr->length();
    arrow::Int32Builder b;
    for (int64_t i = 0; i < n; ++i) {
        const int64_t code = codes_i->IsNull(i) ? -1 : codes_i->Value(i);
        if (code < 0 || code >= nc) (void)b.AppendNull();
        else                        (void)b.Append((int32_t)code);
    }
    std::shared_ptr<arrow::Array> idx;
    if (!b.Finish(&idx).ok()) return nullptr;
    auto dict = arrow::DictionaryArray::FromArrays(
        arrow::dictionary(arrow::int32(), cats_arr->type(), ordered), idx, cats_arr);
    if (!dict.ok()) return nullptr;
    return *dict;
}

static arrow::Result<std::shared_ptr<arrow::Table>>
read_anndata_dataframe(hid_t group, int64_t row_cap, int64_t* full_rows) {
    auto names = list_children(group);
    // Identify the index column from the _index attribute if present.
    std::string idx_name = read_string_attr(group, "_index");
    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols;
    int64_t maxfull = 0;   // longest pre-cap child length (the real row count)

    // Helper to convert one child into a (name, array) pair.
    auto add_child = [&](const std::string& name) -> arrow::Status {
        // Skip private / reserved children.
        if (anndata_reserved_child(name)) return arrow::Status::OK();
        std::string display = name;
        VV_H5O_INFO_T info;
        if (VV_H5Oget_info_by_name(group, name.c_str(), &info, H5O_INFO_BASIC,
                                   H5P_DEFAULT) < 0)
            return arrow::Status::OK();
        if (info.type == H5O_TYPE_GROUP) {
            // AnnData modern categorical: group with "categories" + "codes" datasets.
            hid_t sub = H5Gopen2(group, name.c_str(), H5P_DEFAULT);
            if (sub < 0) return arrow::Status::OK();
            std::string enc = read_string_attr(sub, "encoding-type");
            if (enc == "categorical" &&
                link_exists(sub, "categories") &&
                link_exists(sub, "codes")) {
                hid_t cats_d = H5Dopen2(sub, "categories", H5P_DEFAULT);
                hid_t codes_d = H5Dopen2(sub, "codes", H5P_DEFAULT);
                // `codes` is row-length (cap it); `categories` is the lookup
                // table — read in full only if it's small enough.
                int64_t cats_len = 0;
                { hid_t sp = H5Dget_space(cats_d);
                  hsize_t dd = 0;
                  if (H5Sget_simple_extent_ndims(sp) == 1)
                      H5Sget_simple_extent_dims(sp, &dd, nullptr);
                  H5Sclose(sp); cats_len = (int64_t)dd; }
                int64_t cf = 0;
                auto codes_t = read_1d_dataset_table(codes_d, row_cap, &cf);
                maxfull = std::max(maxfull, cf);

                if (cats_len > category_dict_cap()) {
                    // High-cardinality (e.g. per-cell barcodes): decoding would
                    // require reading the whole multi-million-entry dictionary.
                    // Show the integer codes instead for the preview.
                    H5Dclose(cats_d); H5Dclose(codes_d); H5Gclose(sub);
                    if (codes_t.ok() && (*codes_t)->num_columns() > 0) {
                        cols.push_back((*codes_t)->column(0)->chunk(0));
                        fields.push_back(arrow::field(
                            display + " (codes)",
                            (*codes_t)->schema()->field(0)->type()));
                    }
                    return arrow::Status::OK();
                }

                auto cats_t = read_1d_dataset_table(cats_d);
                const bool ordered = read_bool_attr(sub, "ordered");
                H5Dclose(cats_d); H5Dclose(codes_d); H5Gclose(sub);
                if (cats_t.ok() && codes_t.ok()) {
                    auto a = anndata_decode_codes((*codes_t)->column(0)->chunk(0),
                                                  (*cats_t)->column(0)->chunk(0), ordered);
                    if (a) {
                        cols.push_back(a);
                        fields.push_back(arrow::field(display, a->type()));
                    }
                }
                return arrow::Status::OK();
            }
            if (enc == "nullable-string-array") {
                // anndata >= 0.13: a string column is a {values, mask} group.
                int64_t cf = 0;
                auto a = read_nullable_string_array(sub, row_cap, &cf);
                H5Gclose(sub);
                maxfull = std::max(maxfull, cf);
                if (a.ok()) {
                    cols.push_back(*a);
                    fields.push_back(arrow::field(display, (*a)->type()));
                }
                return arrow::Status::OK();
            }
            H5Gclose(sub);
            return arrow::Status::OK();
        }
        if (info.type != H5O_TYPE_DATASET) return arrow::Status::OK();
        hid_t d = H5Dopen2(group, name.c_str(), H5P_DEFAULT);
        if (d < 0) return arrow::Status::OK();

        // anndata < 0.8 categorical: the column IS the integer code array, and
        // its `categories` attribute is an HDF5 object reference to the lookup
        // table, which that writer always parked at `__categories/<column>` in
        // the same group. Without this the codes render as raw integers — and
        // the digit-grouping formatter makes them look like measurements
        // (`gene = 1_157`, `strand = 0`), which is worse than useless on a
        // real dataset.
        //
        // Resolved by name rather than by dereferencing the attribute: H5R's
        // API changed shape across the HDF5 versions vv builds against (1.14
        // in the static image, 2.x on a current distro), and the by-name
        // layout is what the writer that produced these files emitted. The
        // attribute is still required, so a plain integer column is never
        // mistaken for a categorical.
        if (H5Aexists(d, "categories") > 0 && link_exists(group, "__categories")) {
            hid_t catg = H5Gopen2(group, "__categories", H5P_DEFAULT);
            if (catg >= 0) {
                if (link_exists(catg, name.c_str())) {
                    hid_t cats_d = H5Dopen2(catg, name.c_str(), H5P_DEFAULT);
                    if (cats_d >= 0) {
                        int64_t cats_len = h5_len_1d(cats_d);
                        int64_t cf2 = 0;
                        auto codes_t = read_1d_dataset_table(d, row_cap, &cf2);
                        maxfull = std::max(maxfull, cf2);
                        // Same guard as the modern branch: a per-cell-barcode
                        // dictionary would mean reading millions of strings
                        // just to render a preview.
                        if (cats_len > category_dict_cap()) {
                            H5Dclose(cats_d); H5Gclose(catg); H5Dclose(d);
                            if (codes_t.ok() && (*codes_t)->num_columns() > 0) {
                                cols.push_back((*codes_t)->column(0)->chunk(0));
                                fields.push_back(arrow::field(
                                    display + " (codes)",
                                    (*codes_t)->schema()->field(0)->type()));
                            }
                            return arrow::Status::OK();
                        }
                        auto cats_t = read_1d_dataset_table(cats_d);
                        H5Dclose(cats_d); H5Gclose(catg); H5Dclose(d);
                        if (cats_t.ok() && codes_t.ok()) {
                            auto a = anndata_decode_codes(
                                (*codes_t)->column(0)->chunk(0),
                                (*cats_t)->column(0)->chunk(0));
                            if (a) {
                                cols.push_back(a);
                                fields.push_back(arrow::field(display, a->type()));
                                return arrow::Status::OK();
                            }
                        }
                        // Decode failed — fall through to the raw codes below.
                        hid_t d2 = H5Dopen2(group, name.c_str(), H5P_DEFAULT);
                        if (d2 < 0) return arrow::Status::OK();
                        if (codes_t.ok() && (*codes_t)->num_columns() > 0) {
                            cols.push_back((*codes_t)->column(0)->chunk(0));
                            fields.push_back(arrow::field(
                                display,
                                (*codes_t)->schema()->field(0)->type()));
                        }
                        H5Dclose(d2);
                        return arrow::Status::OK();
                    }
                }
                H5Gclose(catg);
            }
        }

        int64_t cf = 0;
        auto t = read_1d_dataset_table(d, row_cap, &cf);
        maxfull = std::max(maxfull, cf);
        H5Dclose(d);
        if (t.ok() && (*t)->num_columns() > 0) {
            cols.push_back((*t)->column(0)->chunk(0));
            fields.push_back(arrow::field(display, (*t)->schema()->field(0)->type()));
        }
        return arrow::Status::OK();
    };

    // Emit the _index column first if it exists, then the rest in
    // discovered order.
    if (!idx_name.empty()) {
        auto it = std::find(names.begin(), names.end(), idx_name);
        if (it != names.end()) {
            ARROW_RETURN_NOT_OK(add_child(*it));
            names.erase(it);
        }
    }
    for (const auto& n : names)
        ARROW_RETURN_NOT_OK(add_child(n));

    if (full_rows) *full_rows = maxfull;

    // Every column of a DataFrame must share the same length. A malformed
    // AnnData (children of differing sizes, or a categorical whose codes are a
    // different length than its siblings) would otherwise produce an invalid
    // Arrow table and out-of-bounds reads when the TUI pages a row that exists
    // in one column but not another. Normalise to the longest column: pad
    // short ones with trailing nulls, slice over-long ones.
    int64_t target = 0;
    for (const auto& a : cols) target = std::max(target, a->length());
    for (auto& a : cols) {
        if (a->length() == target) continue;
        if (a->length() > target) { a = a->Slice(0, target); continue; }
        auto nulls = arrow::MakeArrayOfNull(a->type(), target - a->length());
        if (!nulls.ok()) continue;
        auto cat = arrow::Concatenate({a, *nulls});
        if (cat.ok()) a = *cat;
    }
    return arrow::Table::Make(arrow::schema(fields), cols, target);
}

// Number of elements in a 1-D HDF5 dataset (0 if not rank-1 / on error).
static int64_t h5_len_1d(hid_t d) {
    hid_t sp = H5Dget_space(d);
    if (sp < 0) return 0;
    hsize_t dd = 0;
    if (H5Sget_simple_extent_ndims(sp) == 1)
        H5Sget_simple_extent_dims(sp, &dd, nullptr);
    H5Sclose(sp);
    return (int64_t)dd;
}

// Densify the first `row_cap` rows (and first 200 columns) of an AnnData sparse
// matrix into an Arrow table (one float64 column per matrix column). Handles
// both CSR (indptr per row, indices are columns) and CSC (indptr per column,
// indices are rows) — the output is rows × columns either way.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_sparse_preview(hid_t group, int64_t row_cap) {
    // shape attribute = [n_rows, n_cols]
    int64_t shape[2] = {0, 0};
    read_shape2(group, "shape", shape);
    std::string enc = read_string_attr(group, "encoding-type");
    bool is_csr = (enc == "csr_matrix");
    bool is_csc = (enc == "csc_matrix");
    if (!is_csr && !is_csc)
        return arrow::Status::Invalid("not a CSR/CSC sparse group");
    return read_sparse_preview_as(group, shape[0], shape[1], is_csr, row_cap);
}

// The same densifying reader for a group holding indptr / indices / data whose
// shape and orientation come from elsewhere (10x Genomics HDF5 keeps the shape
// in a dataset and has no encoding-type attribute). `n_rows` × `n_cols` is the
// logical shape; `is_csr` says indptr runs over rows.
static arrow::Result<std::shared_ptr<arrow::Table>>
read_sparse_preview_as(hid_t group, int64_t n_rows, int64_t n_cols, bool is_csr,
                       int64_t row_cap) {
    if (n_rows < 0) n_rows = 0;          // shape attribute is untrusted
    if (n_cols < 0) n_cols = 0;
    if (row_cap > 0 && row_cap < n_rows) n_rows = row_cap;
    if (n_cols > 200) n_cols = 200;     // wide-table sanity cap

    hid_t indptr_d = H5Dopen2(group, "indptr", H5P_DEFAULT);
    hid_t indices_d = H5Dopen2(group, "indices", H5P_DEFAULT);
    hid_t data_d    = H5Dopen2(group, "data",    H5P_DEFAULT);
    if (indptr_d < 0 || indices_d < 0 || data_d < 0) {
        if (indptr_d >= 0) H5Dclose(indptr_d);
        if (indices_d >= 0) H5Dclose(indices_d);
        if (data_d >= 0) H5Dclose(data_d);
        return arrow::Status::IOError("sparse: missing indptr/indices/data");
    }

    // The compressed (indptr) axis is rows for CSR and columns for CSC. The
    // 'shape' attribute is untrusted: indptr has exactly (compressed_len + 1)
    // entries, so clamp that axis to what indptr actually holds — otherwise the
    // hyperslab below reads past the dataset extent.
    int64_t indptr_len = h5_len_1d(indptr_d);
    if (indptr_len < 1) {
        H5Dclose(indptr_d); H5Dclose(indices_d); H5Dclose(data_d);
        return arrow::Status::Invalid("sparse: empty/!1-D indptr");
    }
    int64_t& n_major = is_csr ? n_rows : n_cols;   // axis the indptr indexes
    if (n_major + 1 > indptr_len) n_major = indptr_len - 1;
    const int64_t nnz_avail = std::min(h5_len_1d(indices_d), h5_len_1d(data_d));

    // Read indptr[0 .. n_major].
    std::vector<int64_t> indptr((size_t)(n_major + 1));
    {
        hid_t fs = H5Dget_space(indptr_d);
        hsize_t start = 0, count = (hsize_t)(n_major + 1);
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
        hid_t ms = H5Screate_simple(1, &count, nullptr);
        herr_t st = h5_read(indptr_d, H5T_NATIVE_INT64, ms, fs, indptr.data());
        H5Sclose(ms); H5Sclose(fs);
        if (st < 0) {
            std::string e = h5_read_failure(indptr_d);
            H5Dclose(indptr_d); H5Dclose(indices_d); H5Dclose(data_d);
            return arrow::Status::IOError(e);
        }
    }
    // indptr values are untrusted too: the read window [front, back) into
    // indices/data must stay inside their actual extent, or the hyperslab
    // reads out of bounds.
    int64_t front = indptr.front();
    if (front < 0) front = 0;
    if (front > nnz_avail) front = nnz_avail;
    int64_t nnz_span = indptr.back() - front;
    if (nnz_span < 0) nnz_span = 0;
    if (nnz_span > nnz_avail - front) nnz_span = nnz_avail - front;

    std::vector<int64_t> indices((size_t)nnz_span);
    std::vector<double>  data((size_t)nnz_span);
    std::string nz_err;   // first indices/data read failure
    if (nnz_span > 0) {
        hsize_t start = (hsize_t)front;
        hsize_t count = (hsize_t)nnz_span;
        {
            hid_t fs = H5Dget_space(indices_d);
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
            hid_t ms = H5Screate_simple(1, &count, nullptr);
            if (h5_read(indices_d, H5T_NATIVE_INT64, ms, fs, indices.data()) < 0 && nz_err.empty())
                nz_err = h5_read_failure(indices_d);
            H5Sclose(ms); H5Sclose(fs);
        }
        {
            hid_t fs = H5Dget_space(data_d);
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
            hid_t ms = H5Screate_simple(1, &count, nullptr);
            if (h5_read(data_d, H5T_NATIVE_DOUBLE, ms, fs, data.data()) < 0 && nz_err.empty())
                nz_err = h5_read_failure(data_d);
            H5Sclose(ms); H5Sclose(fs);
        }
    }
    H5Dclose(indptr_d); H5Dclose(indices_d); H5Dclose(data_d);
    if (!nz_err.empty()) return arrow::Status::IOError(nz_err);

    // Densify into a column-major buffer (always rows × cols). Walk each
    // compressed-axis slice and scatter its values: for CSR `m` is a row and
    // the index is a column; for CSC `m` is a column and the index is a row.
    std::vector<std::vector<double>> colbuf((size_t)n_cols,
                                            std::vector<double>((size_t)n_rows, 0.0));
    for (int64_t m = 0; m < n_major; ++m) {
        // Offsets are relative to the clamped read window `front`, and bounded
        // to [0, nnz_span] so a non-monotonic / corrupt indptr can't index the
        // indices/data vectors out of range.
        int64_t s = indptr[(size_t)m] - front;
        int64_t e = indptr[(size_t)(m + 1)] - front;
        if (s < 0) s = 0;             if (s > nnz_span) s = nnz_span;
        if (e < s) e = s;             if (e > nnz_span) e = nnz_span;
        for (int64_t k = s; k < e; ++k) {
            int64_t idx = indices[(size_t)k];        // minor-axis index
            if (is_csr) {                            // m = row, idx = column
                if (idx >= 0 && idx < n_cols)
                    colbuf[(size_t)idx][(size_t)m] = data[(size_t)k];
            } else {                                 // m = column, idx = row
                if (idx >= 0 && idx < n_rows)
                    colbuf[(size_t)m][(size_t)idx] = data[(size_t)k];
            }
        }
    }
    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols;
    for (int64_t c = 0; c < n_cols; ++c) {
        arrow::DoubleBuilder b;
        (void)b.AppendValues(colbuf[(size_t)c]);
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        cols.push_back(std::move(a));
        fields.push_back(arrow::field("col" + std::to_string(c), arrow::float64()));
    }
    return arrow::Table::Make(arrow::schema(fields), cols, n_rows);
}

// Read up to `cap` values of an AnnData component group's index dataset (the
// dataset named by the group's `_index` attribute, e.g. obs/var cell & gene
// identifiers) as display strings. `*index_name` receives that dataset's name
// for use as a column header. Leaves the outputs empty on any problem.
static void read_anndata_index_labels(hid_t file_id, const std::string& group_path_s,
                                      int64_t cap,
                                      std::vector<std::string>* out,
                                      std::string* index_name) {
    const char* group_path = group_path_s.c_str();
    out->clear();
    if (index_name) index_name->clear();
    hid_t g = H5Gopen2(file_id, group_path, H5P_DEFAULT);
    if (g < 0) return;
    std::string idx = read_string_attr(g, "_index");
    if (idx.empty()) idx = "_index";           // anndata's conventional default
    if (!link_exists(g, idx.c_str())) { H5Gclose(g); return; }
    // _index is a string dataset (legacy) or a nullable-string-array group
    // (anndata >= 0.13).
    std::shared_ptr<arrow::Array> col;
    VV_H5O_INFO_T info;
    if (VV_H5Oget_info_by_name(g, idx.c_str(), &info, H5O_INFO_BASIC, H5P_DEFAULT) >= 0
        && info.type == H5O_TYPE_GROUP) {
        hid_t sub = H5Gopen2(g, idx.c_str(), H5P_DEFAULT);
        if (sub >= 0) {
            auto a = read_nullable_string_array(sub, cap, nullptr);
            H5Gclose(sub);
            if (a.ok()) col = *a;
        }
    } else {
        hid_t d = H5Dopen2(g, idx.c_str(), H5P_DEFAULT);
        if (d >= 0) {
            auto t = read_1d_dataset_table(d, cap);
            H5Dclose(d);
            if (t.ok() && (*t)->num_columns() > 0) col = (*t)->column(0)->chunk(0);
        }
    }
    H5Gclose(g);
    if (!col) return;
    if (index_name) *index_name = idx;
    for (int64_t i = 0; i < col->length(); ++i)
        out->push_back(cell_to_string(*col, i));
}

// Label a dense AnnData 2-D preview with its obs / var identifiers.
//
// The three shapes are NOT interchangeable, and treating them as one put gene
// names on UMAP coordinates:
//
//   X, layers/*  (n_obs x n_var)  rows <- obs (cells),  cols <- var (genes)
//   obsm/*       (n_obs x d)      rows <- obs (cells),  cols <- dimensions
//   varm/*       (n_var x d)      rows <- var (genes),  cols <- dimensions
//
// `d` is an embedding width (2 for a UMAP, 50 for a PCA), unrelated to the
// number of genes — so the columns are named after the key that holds them,
// 1-based: X_umap -> X_umap1, X_umap2.
//
// No-op for a non-AnnData file (no obs/var groups -> empty labels).
static void apply_anndata_matrix_labels(hid_t file_id, const std::string& root, AnnMatrixAxes axes,
                                        const std::string& key,
                                        std::shared_ptr<arrow::Table>* tbl) {
    if (!tbl || !*tbl) return;
    auto t = *tbl;
    const int64_t ncols = t->num_columns();
    const int64_t nrows = t->num_rows();

    // ── Columns ─────────────────────────────────────────────────────────────
    std::vector<std::string> col_names;
    if (axes == AnnMatrixAxes::ObsByVar || axes == AnnMatrixAxes::ObsByRawVar) {
        // Gene identifiers, from the var index — raw/var's for raw/X, which
        // usually keeps more genes than X (X is often subset to the highly
        // variable ones).
        std::string var_idx_name;
        read_anndata_index_labels(file_id,
                                  root + (axes == AnnMatrixAxes::ObsByRawVar ? "/raw/var" : "/var"),
                                  ncols, &col_names, &var_idx_name);
    } else {
        // Embedding dimensions. Derived from the key so the header says where
        // the numbers came from; falls back to the existing generated names if
        // the key is somehow empty.
        if (!key.empty())
            for (int64_t i = 0; i < ncols; ++i)
                col_names.push_back(key + std::to_string(i + 1));
    }
    if (!col_names.empty()) {
        std::vector<std::string> names;
        names.reserve((size_t)ncols);
        for (int64_t i = 0; i < ncols; ++i)
            names.push_back(i < (int64_t)col_names.size()
                                ? col_names[(size_t)i]
                                : t->field((int)i)->name());
        auto r = t->RenameColumns(names);
        if (r.ok()) t = *r;
    }

    // ── Rows: prepended as a leading label column ───────────────────────────
    // varm is indexed by gene, everything else by cell.
    const std::string row_group = root + ((axes == AnnMatrixAxes::VarByDim) ? "/var" : "/obs");
    const char* row_default = (axes == AnnMatrixAxes::VarByDim) ? "var" : "obs";
    std::vector<std::string> row_labels;
    std::string row_idx_name;
    read_anndata_index_labels(file_id, row_group, nrows, &row_labels,
                              &row_idx_name);
    if (!row_labels.empty()) {
        arrow::StringBuilder b;
        for (int64_t i = 0; i < nrows; ++i) {
            if (i < (int64_t)row_labels.size()) (void)b.Append(row_labels[(size_t)i]);
            else                                (void)b.AppendNull();
        }
        std::shared_ptr<arrow::Array> a;
        if (b.Finish(&a).ok()) {
            std::string header = row_idx_name.empty() || row_idx_name == "_index"
                                     ? row_default : row_idx_name;
            auto col = std::make_shared<arrow::ChunkedArray>(a);
            auto r = t->AddColumn(0, arrow::field(header, arrow::utf8()), col);
            if (r.ok()) t = *r;
        }
    }
    *tbl = t;
}

// ── A whole AnnData / HDF5 matrix, streamed in row blocks ───────────────────
//
// The export counterpart of the capped matrix preview: every row and column
// of a dense 2-D dataset (hyperslabs; Loom's genes × cells read as column
// slabs), or a CSR-style group (AnnData CSR, Cell Ranger's barcode-major
// matrix: indptr ranges, densified per block), with the preview's columns —
// the row label, then one column per gene / dimension. Blocks hold about 4
// million cells, so memory is bounded however large the matrix. Values keep
// the preview's types. An AnnData CSC matrix is transposed to CSR in memory
// on the first read (about 12 bytes per stored value).
class H5MatrixStreamSource : public TabularSource {
public:
    // How rows map onto the stored data: a dense dataset read row by row
    // (Dense), a dense dataset whose columns are the rows (DenseT: Loom's
    // genes × cells, shown cells × genes), or a group with CSR-style
    // data / indices / indptr over the rows (AnnData CSR, Cell Ranger), or
    // over the columns (Csc: AnnData CSC, transposed in memory on first read).
    enum class Layout { Dense, DenseT, Csr, Csc };
    struct Plan {
        std::string h5_path, label, format;
        Layout      layout = Layout::Dense;
        int64_t     rows = 0, cols = 0;
        bool        ints = false;            // int64 values, else double
        std::string row_header;              // row label column ("" = none)
        std::vector<std::string> row_labels, col_names;
        // --matrix long: the column-label column's name ("var", the var
        // index name, "dim" for an embedding, ...); row_header names the row
        // labels ("obs" when the wide view has none). Empty labels → indexes.
        std::string long_row = "row", long_col = "col";
    };

private:
    H5FilePtr   file_;
    std::string path_;
    Plan        plan_;
    int64_t     block_ = 1;
    std::shared_ptr<arrow::Schema> schema_;
    mutable arrow::Status status_;
    // Layout::Csc: the matrix as CSR (row pointers, column, value), built by
    // one counting-sort pass over indices / data the first time a chunk is
    // read — about 12 bytes per stored value plus 8 per row.
    bool                 csr_built_ = false;
    std::vector<int64_t> csr_ptr_;
    std::vector<int32_t> csr_col_;
    std::vector<double>  csr_val_;

    arrow::Status build_csr_from_csc() {
        const hid_t fid = *file_;
        const std::string& gp = plan_.h5_path;
        const int64_t R = plan_.rows, C = plan_.cols;
        if (C > INT32_MAX) return arrow::Status::Invalid(gp, ": too many columns");
        hid_t g = H5Gopen2(fid, gp.c_str(), H5P_DEFAULT);
        if (g < 0) return arrow::Status::IOError("cannot open ", gp);
        std::string err;
        auto read_slice = [&](const char* name, int64_t off, int64_t len, hid_t mtype, void* buf) {
            if (len == 0) return true;
            hid_t d = H5Dopen2(g, name, H5P_DEFAULT);
            if (d < 0) { err = gp + "/" + name + ": cannot open"; return false; }
            hid_t fs = H5Dget_space(d);
            hsize_t start = (hsize_t)off, count = (hsize_t)len;
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
            hid_t ms = H5Screate_simple(1, &count, nullptr);
            const bool ok = H5Dread(d, mtype, ms, fs, H5P_DEFAULT, buf) >= 0;
            if (!ok) err = h5_read_failure(d);
            H5Sclose(ms); H5Sclose(fs); H5Dclose(d);
            return ok;
        };
        std::vector<int64_t> cptr((size_t)C + 1);
        bool ok = read_slice("indptr", 0, C + 1, H5T_NATIVE_INT64, cptr.data());
        const int64_t nnz = ok ? cptr[(size_t)C] - cptr[0] : 0;
        for (int64_t c = 0; ok && c < C; ++c)
            if (cptr[(size_t)c + 1] < cptr[(size_t)c]) { err = gp + "/indptr decreases"; ok = false; }
        // Pass 1: entries per row. Pass 2: scatter (column, value) into each
        // row's slots in column order, so every row comes out sorted.
        constexpr int64_t kStep = (int64_t)1 << 22;
        std::vector<int64_t> idx;
        std::vector<double>  val;
        if (ok) csr_ptr_.assign((size_t)R + 1, 0);
        for (int64_t k0 = 0; ok && k0 < nnz; k0 += kStep) {
            const int64_t len = std::min(kStep, nnz - k0);
            idx.resize((size_t)len);
            ok = read_slice("indices", cptr[0] + k0, len, H5T_NATIVE_INT64, idx.data());
            for (int64_t k = 0; ok && k < len; ++k) {
                if (idx[(size_t)k] < 0 || idx[(size_t)k] >= R) { err = gp + ": row index out of range"; ok = false; break; }
                ++csr_ptr_[(size_t)idx[(size_t)k] + 1];
            }
        }
        if (ok) {
            for (int64_t r = 0; r < R; ++r) csr_ptr_[(size_t)r + 1] += csr_ptr_[(size_t)r];
            csr_col_.resize((size_t)nnz);
            csr_val_.resize((size_t)nnz);
        }
        std::vector<int64_t> fill;                     // next free slot per row
        if (ok) fill.assign(csr_ptr_.begin(), csr_ptr_.end() - 1);
        int64_t c = 0;
        for (int64_t k0 = 0; ok && k0 < nnz; k0 += kStep) {
            const int64_t len = std::min(kStep, nnz - k0);
            idx.resize((size_t)len); val.resize((size_t)len);
            ok = read_slice("indices", cptr[0] + k0, len, H5T_NATIVE_INT64, idx.data()) &&
                 read_slice("data", cptr[0] + k0, len, H5T_NATIVE_DOUBLE, val.data());
            for (int64_t k = 0; ok && k < len; ++k) {
                const int64_t at = cptr[0] + k0 + k;
                while (c < C && at >= cptr[(size_t)c + 1]) ++c;
                const int64_t pos = fill[(size_t)idx[(size_t)k]]++;
                csr_col_[(size_t)pos] = (int32_t)c;
                csr_val_[(size_t)pos] = val[(size_t)k];
            }
        }
        H5Gclose(g);
        if (!ok) {
            csr_ptr_.clear(); csr_col_.clear(); csr_val_.clear();
            return arrow::Status::IOError(err.empty() ? gp + ": cannot read" : err);
        }
        csr_built_ = true;
        return arrow::Status::OK();
    }

public:
    static std::unique_ptr<TabularSource> from_plan(const H5FilePtr& file, const std::string& path,
                                                    Plan plan) {
        if (plan.cols <= 0 || plan.rows < 0) return nullptr;
        auto self = std::make_unique<H5MatrixStreamSource>();
        self->file_ = file; self->path_ = path;
        self->block_ = std::clamp<int64_t>((int64_t)4000000 / plan.cols, 1, 65536);
        arrow::FieldVector fields;
        if (!plan.row_header.empty()) fields.push_back(arrow::field(plan.row_header, arrow::utf8()));
        for (int64_t c = 0; c < plan.cols; ++c)
            fields.push_back(arrow::field(c < (int64_t)plan.col_names.size() ? plan.col_names[(size_t)c]
                                                                             : "col" + std::to_string(c),
                                          plan.ints ? arrow::int64() : arrow::float64()));
        self->schema_ = arrow::schema(fields);
        self->plan_ = std::move(plan);
        return self;
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return plan_.rows; }
    int     num_chunks() const override { return (int)((plan_.rows + block_ - 1) / block_); }
    ChunkMeta chunk_meta(int i) const override {
        const int64_t r0 = (int64_t)i * block_;
        return {r0, std::min(block_, plan_.rows - r0)};
    }
    arrow::Status read_status() const override { return status_; }
    const std::string& path() const override { return path_; }
    std::string tab_label() const override { return plan_.label; }
    std::string footer() const override {
        return "Format: " + plan_.format + " " + plan_.label + " (full matrix)  |  " +
               std::to_string(plan_.rows) + " \xc3\x97 " + std::to_string(plan_.cols);
    }

    const Plan& plan() const { return plan_; }
    int64_t block_rows() const { return block_; }

    // Block i's rows as a dense row-major n × cols buffer (iv for integer
    // plans, dv otherwise).
    arrow::Status fill_block(int i, std::vector<int64_t>& iv, std::vector<double>& dv) {
        const int64_t r0 = (int64_t)i * block_;
        const int64_t n  = std::min(block_, plan_.rows - r0);
        const int64_t cols_ = plan_.cols;
        const bool ints_ = plan_.ints;
        const std::string& h5_path_ = plan_.h5_path;
        if (n <= 0) return arrow::Status::IndexError("chunk ", i, " out of range");
        (ints_ ? (void)iv.assign((size_t)(n * cols_), 0) : (void)dv.assign((size_t)(n * cols_), 0.0));
        const hid_t fid = *file_;
        auto fail = [&](const std::string& why) {
            status_ = arrow::Status::IOError(why);
            return status_;
        };
        if (plan_.layout == Layout::Csc) {
            if (!csr_built_) {
                auto st = build_csr_from_csc();
                if (!st.ok()) return fail(st.message());
            }
            for (int64_t r = 0; r < n; ++r)
                for (int64_t k = csr_ptr_[(size_t)(r0 + r)]; k < csr_ptr_[(size_t)(r0 + r) + 1]; ++k)
                    dv[(size_t)(r * cols_ + csr_col_[(size_t)k])] = csr_val_[(size_t)k];
        } else if (plan_.layout != Layout::Csr) {
            // DenseT reads stored columns r0..r0+n of every stored row into a
            // cols × n buffer; the cell (r, c) is then buf[c * n + r].
            const bool tr = plan_.layout == Layout::DenseT;
            hid_t d = H5Dopen2(fid, h5_path_.c_str(), H5P_DEFAULT);
            if (d < 0) return fail("cannot open " + h5_path_);
            hid_t fs = H5Dget_space(d);
            hsize_t start[2] = {(hsize_t)r0, 0}, count[2] = {(hsize_t)n, (hsize_t)cols_};
            if (tr) { start[0] = 0; start[1] = (hsize_t)r0; count[0] = (hsize_t)cols_; count[1] = (hsize_t)n; }
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr);
            hid_t ms = H5Screate_simple(2, count, nullptr);
            herr_t st = ints_ ? H5Dread(d, H5T_NATIVE_INT64, ms, fs, H5P_DEFAULT, iv.data())
                              : H5Dread(d, H5T_NATIVE_DOUBLE, ms, fs, H5P_DEFAULT, dv.data());
            const std::string why = st < 0 ? h5_read_failure(d) : std::string();
            H5Sclose(ms); H5Sclose(fs); H5Dclose(d);
            if (st < 0) return fail(why);
            if (tr) {                                    // to row-major n × cols
                if (ints_) {
                    std::vector<int64_t> t((size_t)(n * cols_));
                    for (int64_t c = 0; c < cols_; ++c)
                        for (int64_t r = 0; r < n; ++r) t[(size_t)(r * cols_ + c)] = iv[(size_t)(c * n + r)];
                    iv.swap(t);
                } else {
                    std::vector<double> t((size_t)(n * cols_));
                    for (int64_t c = 0; c < cols_; ++c)
                        for (int64_t r = 0; r < n; ++r) t[(size_t)(r * cols_ + c)] = dv[(size_t)(c * n + r)];
                    dv.swap(t);
                }
            }
        } else {
            hid_t g = H5Gopen2(fid, h5_path_.c_str(), H5P_DEFAULT);
            if (g < 0) return fail("cannot open " + h5_path_);
            auto read_slice = [&](const char* name, int64_t off, int64_t len, hid_t mtype, void* buf) {
                hid_t d = H5Dopen2(g, name, H5P_DEFAULT);
                if (d < 0) return false;
                hid_t fs = H5Dget_space(d);
                hsize_t start = (hsize_t)off, count = (hsize_t)len;
                H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
                hid_t ms = H5Screate_simple(1, &count, nullptr);
                const bool ok = len == 0 || H5Dread(d, mtype, ms, fs, H5P_DEFAULT, buf) >= 0;
                H5Sclose(ms); H5Sclose(fs); H5Dclose(d);
                return ok;
            };
            std::vector<int64_t> ip((size_t)n + 1);
            if (!read_slice("indptr", r0, n + 1, H5T_NATIVE_INT64, ip.data())) { H5Gclose(g); return fail("cannot read indptr of " + h5_path_); }
            const int64_t a = ip[0], len = ip[(size_t)n] - a;
            if (len < 0) { H5Gclose(g); return fail("indptr of " + h5_path_ + " decreases"); }
            std::vector<int64_t> idx((size_t)len);
            std::vector<double>  dval;
            std::vector<int64_t> ival;
            (ints_ ? ival.resize((size_t)len) : dval.resize((size_t)len));
            const bool ok = read_slice("indices", a, len, H5T_NATIVE_INT64, idx.data()) &&
                            (ints_ ? read_slice("data", a, len, H5T_NATIVE_INT64, ival.data())
                                   : read_slice("data", a, len, H5T_NATIVE_DOUBLE, dval.data()));
            H5Gclose(g);
            if (!ok) return fail("cannot read data / indices of " + h5_path_);
            for (int64_t r = 0; r < n; ++r)
                for (int64_t k = ip[(size_t)r] - a; k < ip[(size_t)r + 1] - a; ++k) {
                    const int64_t c = idx[(size_t)k];
                    if (c < 0 || c >= cols_) return fail("column index out of range in " + h5_path_);
                    if (ints_) iv[(size_t)(r * cols_ + c)] = ival[(size_t)k];
                    else       dv[(size_t)(r * cols_ + c)] = dval[(size_t)k];
                }
        }
        return arrow::Status::OK();
    }

    // --matrix long: the entries of block i as (row, column, value) in row
    // order — every stored entry of a sparse matrix, every non-zero cell of a
    // dense one.
    struct Entries {
        std::vector<int64_t> row;
        std::vector<int64_t> col;
        std::vector<int64_t> iv;
        std::vector<double>  dv;
    };
    arrow::Status entries(int i, Entries* e) {
        e->row.clear(); e->col.clear(); e->iv.clear(); e->dv.clear();
        const int64_t r0 = (int64_t)i * block_;
        const int64_t n  = std::min(block_, plan_.rows - r0);
        if (n <= 0) return arrow::Status::IndexError("chunk ", i, " out of range");
        if (plan_.layout == Layout::Csc) {
            if (!csr_built_) ARROW_RETURN_NOT_OK(build_csr_from_csc());
            for (int64_t r = r0; r < r0 + n; ++r)
                for (int64_t k = csr_ptr_[(size_t)r]; k < csr_ptr_[(size_t)r + 1]; ++k) {
                    e->row.push_back(r); e->col.push_back(csr_col_[(size_t)k]);
                    e->dv.push_back(csr_val_[(size_t)k]);
                }
            return arrow::Status::OK();
        }
        if (plan_.layout == Layout::Csr) {
            hid_t g = H5Gopen2(*file_, plan_.h5_path.c_str(), H5P_DEFAULT);
            if (g < 0) return arrow::Status::IOError("cannot open ", plan_.h5_path);
            std::string err;
            auto read_slice = [&](const char* name, int64_t off, int64_t len, hid_t mtype, void* buf) {
                if (len == 0) return true;
                hid_t d = H5Dopen2(g, name, H5P_DEFAULT);
                if (d < 0) { err = plan_.h5_path + "/" + name + ": cannot open"; return false; }
                hid_t fs = H5Dget_space(d);
                hsize_t start = (hsize_t)off, count = (hsize_t)len;
                H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
                hid_t ms = H5Screate_simple(1, &count, nullptr);
                const bool ok = H5Dread(d, mtype, ms, fs, H5P_DEFAULT, buf) >= 0;
                if (!ok) err = h5_read_failure(d);
                H5Sclose(ms); H5Sclose(fs); H5Dclose(d);
                return ok;
            };
            std::vector<int64_t> ip((size_t)n + 1);
            bool ok = read_slice("indptr", r0, n + 1, H5T_NATIVE_INT64, ip.data());
            const int64_t a = ok ? ip[0] : 0, len = ok ? ip[(size_t)n] - a : 0;
            if (ok && len < 0) { err = plan_.h5_path + "/indptr decreases"; ok = false; }
            if (ok) {
                e->col.resize((size_t)len);
                ok = read_slice("indices", a, len, H5T_NATIVE_INT64, e->col.data());
                if (ok && plan_.ints) { e->iv.resize((size_t)len); ok = read_slice("data", a, len, H5T_NATIVE_INT64, e->iv.data()); }
                else if (ok)          { e->dv.resize((size_t)len); ok = read_slice("data", a, len, H5T_NATIVE_DOUBLE, e->dv.data()); }
            }
            H5Gclose(g);
            if (!ok) return arrow::Status::IOError(err.empty() ? plan_.h5_path + ": cannot read" : err);
            e->row.resize((size_t)len);
            for (int64_t r = 0; r < n; ++r) {
                if (ip[(size_t)r + 1] < ip[(size_t)r]) return arrow::Status::IOError(plan_.h5_path, "/indptr decreases");
                for (int64_t k = ip[(size_t)r] - a; k < ip[(size_t)r + 1] - a; ++k) e->row[(size_t)k] = r0 + r;
            }
            for (int64_t c : e->col)
                if (c < 0 || c >= plan_.cols) return arrow::Status::IOError("column index out of range in ", plan_.h5_path);
            return arrow::Status::OK();
        }
        std::vector<int64_t> iv;
        std::vector<double>  dv;
        ARROW_RETURN_NOT_OK(fill_block(i, iv, dv));
        const int64_t C = plan_.cols;
        for (int64_t r = 0; r < n; ++r)
            for (int64_t c = 0; c < C; ++c) {
                const size_t k = (size_t)(r * C + c);
                if (plan_.ints ? iv[k] == 0 : dv[k] == 0.0) continue;
                e->row.push_back(r0 + r); e->col.push_back(c);
                if (plan_.ints) e->iv.push_back(iv[k]); else e->dv.push_back(dv[k]);
            }
        return arrow::Status::OK();
    }

    // Stored entries per block, when they are known without reading values
    // (sparse layouts); false for a dense matrix.
    bool block_counts(std::vector<int64_t>* counts) {
        counts->clear();
        const int nb = num_chunks();
        if (plan_.layout == Layout::Csc) {
            if (!csr_built_ && !build_csr_from_csc().ok()) return false;
            for (int b = 0; b < nb; ++b) {
                const int64_t r0 = (int64_t)b * block_, r1 = std::min(plan_.rows, r0 + block_);
                counts->push_back(csr_ptr_[(size_t)r1] - csr_ptr_[(size_t)r0]);
            }
            return true;
        }
        if (plan_.layout != Layout::Csr) return false;
        hid_t g = H5Gopen2(*file_, plan_.h5_path.c_str(), H5P_DEFAULT);
        if (g < 0) return false;
        hid_t d = H5Dopen2(g, "indptr", H5P_DEFAULT);
        std::vector<int64_t> ip((size_t)plan_.rows + 1);
        bool ok = d >= 0;
        if (ok) {
            hid_t fs = H5Dget_space(d);
            hsize_t start = 0, count = (hsize_t)plan_.rows + 1;
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
            hid_t ms = H5Screate_simple(1, &count, nullptr);
            ok = H5Dread(d, H5T_NATIVE_INT64, ms, fs, H5P_DEFAULT, ip.data()) >= 0;
            H5Sclose(ms); H5Sclose(fs); H5Dclose(d);
        }
        H5Gclose(g);
        if (!ok) return false;
        for (int b = 0; b < nb; ++b) {
            const int64_t r0 = (int64_t)b * block_, r1 = std::min(plan_.rows, r0 + block_);
            if (ip[(size_t)r1] < ip[(size_t)r0]) return false;
            counts->push_back(ip[(size_t)r1] - ip[(size_t)r0]);
        }
        return true;
    }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        const int64_t r0 = (int64_t)i * block_;
        const int64_t n  = std::min(block_, plan_.rows - r0);
        const int64_t cols_ = plan_.cols;
        const bool ints_ = plan_.ints;
        if (n <= 0) return arrow::Status::IndexError("chunk ", i, " out of range");
        std::vector<double>  dv;
        std::vector<int64_t> iv;
        if (auto st = fill_block(i, iv, dv); !st.ok()) {
            if (status_.ok()) status_ = st;
            return st;
        }
        const auto& row_labels_ = plan_.row_labels;
        const int label = plan_.row_header.empty() ? 0 : 1;
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::Array>> cols;
        for (int ci : col_indices) {
            fields.push_back(schema_->field(ci));
            std::shared_ptr<arrow::Array> arr;
            if (label && ci == 0) {
                arrow::StringBuilder b;
                for (int64_t r = 0; r < n; ++r)
                    (void)(r0 + r < (int64_t)row_labels_.size() ? b.Append(row_labels_[(size_t)(r0 + r)]) : b.AppendNull());
                ARROW_RETURN_NOT_OK(b.Finish(&arr));
            } else if (ints_) {
                arrow::Int64Builder b;
                ARROW_RETURN_NOT_OK(b.Reserve(n));
                for (int64_t r = 0; r < n; ++r) b.UnsafeAppend(iv[(size_t)(r * cols_ + ci - label)]);
                ARROW_RETURN_NOT_OK(b.Finish(&arr));
            } else {
                arrow::DoubleBuilder b;
                ARROW_RETURN_NOT_OK(b.Reserve(n));
                for (int64_t r = 0; r < n; ++r) b.UnsafeAppend(dv[(size_t)(r * cols_ + ci - label)]);
                ARROW_RETURN_NOT_OK(b.Finish(&arr));
            }
            cols.push_back(std::move(arr));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, n);
        return arrow::Status::OK();
    }
};


// ── --matrix long: one row per matrix entry ─────────────────────────────────
//
// The whole matrix (not a preview) as (row label, column label, value) rows,
// built block by block from an H5MatrixStreamSource: every stored entry of a
// sparse matrix, every non-zero cell of a dense one. Labels are dictionary
// columns over the full label lists (one copy, any number of rows); a matrix
// without labels gets integer indexes. A sparse matrix knows its entries per
// block from indptr, so all chunks are addressable at once; a dense one is
// read forward, a block at a time, until its zeros have been skipped.
class H5LongMatrixSource : public TabularSource {
    using Stream = H5MatrixStreamSource;
    std::unique_ptr<Stream>          wide_;
    std::string                      label_;
    std::shared_ptr<arrow::Schema>   schema_;
    std::shared_ptr<arrow::Array>    row_dict_, col_dict_;   // null: indexes
    std::vector<int64_t>             first_;                  // per known block
    std::vector<int64_t>             count_;
    bool                             all_known_ = false;
    int                              cached_block_ = -1;
    Stream::Entries                  cached_;
    mutable arrow::Status            status_;

    static std::shared_ptr<arrow::Array> strings(const std::vector<std::string>& v) {
        arrow::StringBuilder b;
        for (const auto& x : v) (void)b.Append(x);
        std::shared_ptr<arrow::Array> a;
        (void)b.Finish(&a);
        return a;
    }
    arrow::Status load(int b) {
        if (b == cached_block_) return arrow::Status::OK();
        auto st = wide_->entries(b, &cached_);
        if (!st.ok()) { if (status_.ok()) status_ = st; cached_block_ = -1; return st; }
        cached_block_ = b;
        return arrow::Status::OK();
    }

public:
    static std::unique_ptr<TabularSource> make(std::unique_ptr<TabularSource> wide_src) {
        auto* w = dynamic_cast<Stream*>(wide_src.get());
        if (!w) return nullptr;
        auto self = std::make_unique<H5LongMatrixSource>();
        self->wide_.reset(static_cast<Stream*>(wide_src.release()));
        const auto& p = self->wide_->plan();
        std::string lbl = p.label;
        if (auto at = lbl.find(" (preview)"); at != std::string::npos) lbl.erase(at);
        const std::string tag = " (long)";
        if (lbl.size() < tag.size() || lbl.compare(lbl.size() - tag.size(), tag.size(), tag) != 0)
            lbl += tag;
        self->label_ = lbl;
        const bool rl = !p.row_labels.empty() && (int64_t)p.row_labels.size() == p.rows &&
                        p.rows < INT32_MAX;
        const bool cl = !p.col_names.empty() && (int64_t)p.col_names.size() == p.cols &&
                        p.cols < INT32_MAX;
        if (rl) self->row_dict_ = strings(p.row_labels);
        if (cl) self->col_dict_ = strings(p.col_names);
        auto label_type = arrow::dictionary(arrow::int32(), arrow::utf8());
        std::string rname = p.long_row, cname = p.long_col, vname = "value";
        if (cname == rname) cname += "_col";
        self->schema_ = arrow::schema({
            arrow::field(rname, rl ? label_type : arrow::int64()),
            arrow::field(cname, cl ? label_type : arrow::int64()),
            arrow::field(vname, p.ints ? arrow::int64() : arrow::float64())});
        std::vector<int64_t> counts;
        if (self->wide_->block_counts(&counts)) {
            int64_t at = 0;
            for (int64_t c : counts) { self->first_.push_back(at); self->count_.push_back(c); at += c; }
            self->all_known_ = true;
        }
        return self;
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override {
        if (!all_known_) return -1;
        return first_.empty() ? 0 : first_.back() + count_.back();
    }
    int  num_chunks() const override { return (int)count_.size(); }
    ChunkMeta chunk_meta(int i) const override {
        if (i < 0 || i >= (int)count_.size()) return {total_rows() < 0 ? 0 : total_rows(), 0};
        return {first_[(size_t)i], count_[(size_t)i]};
    }
    // A dense matrix: read blocks forward until block i's size is known.
    void ensure(int i) override {
        while (!all_known_ && (int)count_.size() <= i) {
            const int b = (int)count_.size();
            if (b >= wide_->num_chunks()) { all_known_ = true; break; }
            if (!load(b).ok()) { all_known_ = true; break; }
            const int64_t at = first_.empty() ? 0 : first_.back() + count_.back();
            first_.push_back(at);
            count_.push_back((int64_t)cached_.row.size());
        }
    }
    arrow::Status read_status() const override {
        return status_.ok() ? wide_->read_status() : status_;
    }
    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i < 0 || i >= (int)count_.size())
            return arrow::Status::IndexError("chunk ", i, " out of range");
        ARROW_RETURN_NOT_OK(load(i));
        const auto& e = cached_;
        const int64_t n = (int64_t)e.row.size();
        auto labels = [&](const std::vector<int64_t>& idx, const std::shared_ptr<arrow::Array>& dict,
                          std::shared_ptr<arrow::Array>* arr) -> arrow::Status {
            if (!dict) {
                arrow::Int64Builder b;
                ARROW_RETURN_NOT_OK(b.AppendValues(idx));
                return b.Finish(arr);
            }
            arrow::Int32Builder b;
            ARROW_RETURN_NOT_OK(b.Reserve(n));
            for (int64_t v : idx) b.UnsafeAppend((int32_t)v);
            std::shared_ptr<arrow::Array> ix;
            ARROW_RETURN_NOT_OK(b.Finish(&ix));
            ARROW_ASSIGN_OR_RAISE(*arr, arrow::DictionaryArray::FromArrays(
                arrow::dictionary(arrow::int32(), arrow::utf8()), ix, dict));
            return arrow::Status::OK();
        };
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::Array>> cols;
        for (int c : col_indices) {
            std::shared_ptr<arrow::Array> arr;
            if (c == 0) ARROW_RETURN_NOT_OK(labels(e.row, row_dict_, &arr));
            else if (c == 1) ARROW_RETURN_NOT_OK(labels(e.col, col_dict_, &arr));
            else if (wide_->plan().ints) {
                arrow::Int64Builder b;
                ARROW_RETURN_NOT_OK(b.AppendValues(e.iv));
                ARROW_RETURN_NOT_OK(b.Finish(&arr));
            } else {
                arrow::DoubleBuilder b;
                ARROW_RETURN_NOT_OK(b.AppendValues(e.dv));
                ARROW_RETURN_NOT_OK(b.Finish(&arr));
            }
            fields.push_back(schema_->field(c));
            cols.push_back(std::move(arr));
        }
        *out = arrow::Table::Make(arrow::schema(fields), cols, n);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return wide_->path(); }
    std::string tab_label() const override { return label_; }
    std::string footer() const override {
        const auto& p = wide_->plan();
        std::string f = "Format: " + p.format + " " + label_ + "  |  " + std::to_string(p.rows) +
                        " \xc3\x97 " + std::to_string(p.cols) + " matrix, one row per " +
                        (p.layout == Stream::Layout::Csr || p.layout == Stream::Layout::Csc
                             ? "stored entry" : "non-zero cell");
        if (all_known_) f += "  |  Rows: " + std::to_string(total_rows());
        return f;
    }
};

// ── Scanners — decide which tabs to emit ────────────────────────────────────

static std::vector<OpenSpec> scan_generic(hid_t file_id) {
    std::vector<OpenSpec> specs;
    // Tab 0 = hierarchy.
    specs.push_back({OpenSpec::Kind::Hierarchy, "/", "hierarchy", ""});
    // For each 1-D or 2-D dataset, add a tab.
    struct Scan { std::vector<OpenSpec>* out; int n_dsets = 0; };
    Scan ctx{&specs, 0};
    auto cb = [](hid_t loc_id, const char* name, const VV_H5L_INFO_T*, void* data) -> herr_t {
        auto* sc = static_cast<Scan*>(data);
        if (sc->n_dsets > 32) return 0;          // cap to keep tab count sane
        VV_H5O_INFO_T info;
        if (VV_H5Oget_info_by_name(loc_id, name, &info, H5O_INFO_BASIC, H5P_DEFAULT) < 0)
            return 0;
        if (info.type != H5O_TYPE_DATASET) return 0;
        hid_t d = H5Dopen2(loc_id, name, H5P_DEFAULT);
        if (d < 0) return 0;
        hid_t s = H5Dget_space(d);
        int nd = H5Sget_simple_extent_ndims(s);
        std::vector<hsize_t> dims((size_t)nd);
        if (nd > 0) H5Sget_simple_extent_dims(s, dims.data(), nullptr);
        H5Sclose(s); H5Dclose(d);
        if (nd == 1) {
            sc->out->push_back({OpenSpec::Kind::Dataset1D,
                                  std::string("/") + name,
                                  std::string("/") + name,
                                  shape_to_string(dims)});
            ++sc->n_dsets;
        } else if (nd == 2) {
            // Any width: the 2-D reader previews the first 1000 rows and 200
            // columns and says so in the footer. Wider datasets used to get no
            // tab at all, which hid e.g. a Loom file's expression matrix.
            sc->out->push_back({OpenSpec::Kind::Dataset2D,
                                  std::string("/") + name,
                                  std::string("/") + name,
                                  shape_to_string(dims)});
            ++sc->n_dsets;
        }
        return 0;
    };
    VV_H5Lvisit(file_id, H5_INDEX_NAME, H5_ITER_NATIVE,
               (VV_H5L_ITERATE_T)cb, &ctx);
    return specs;
}

// One summary line profiling an AnnData matrix (X, a layer, raw/X): its dtype,
// how much of it is stored (sparse) and a sample of its values — the max and
// whether they are whole numbers — so raw counts can be told from normalised or
// log values without opening the matrix. The sample is the first 100,000
// stored values (sparse data) or the leading rows (dense); the line says so.
// "" when `path` is not a 2-D dataset or a CSR / CSC group.
static std::string matrix_profile(hid_t file_id, const std::string& path) {
    constexpr int64_t kSample = 100000;
    int64_t rows = 0, cols = 0, stored = -1;
    bool sparse = false;
    hid_t d = -1;
    if (is_group(file_id, path.c_str())) {
        hid_t g = H5Gopen2(file_id, path.c_str(), H5P_DEFAULT);
        if (g < 0) return "";
        const std::string enc = read_string_attr(g, "encoding-type");
        if (enc == "csr_matrix" || enc == "csc_matrix") {
            int64_t shape[2] = {0, 0};
            read_shape2(g, "shape", shape);
            rows = shape[0]; cols = shape[1];
            if (link_exists(g, "data")) d = H5Dopen2(g, "data", H5P_DEFAULT);
            sparse = true;
        }
        H5Gclose(g);
        if (d < 0) return "";
        stored = h5_len_1d(d);
    } else {
        d = H5Dopen2(file_id, path.c_str(), H5P_DEFAULT);
        if (d < 0) return "";
        hid_t sp = H5Dget_space(d);
        hsize_t dims[2] = {0, 0};
        const bool two_d = H5Sget_simple_extent_ndims(sp) == 2;
        if (two_d) H5Sget_simple_extent_dims(sp, dims, nullptr);
        H5Sclose(sp);
        if (!two_d) { H5Dclose(d); return ""; }
        rows = (int64_t)dims[0]; cols = (int64_t)dims[1];
    }
    hid_t t = H5Dget_type(d);
    const std::string dtype = dtype_to_string(t);
    const H5T_class_t cls = H5Tget_class(t);
    H5Tclose(t);
    if (cls != H5T_INTEGER && cls != H5T_FLOAT) { H5Dclose(d); return dtype; }

    // The sample: a leading run of stored values (sparse) or leading rows.
    std::vector<double> v;
    std::string err;
    {
        hid_t fs = H5Dget_space(d);
        hid_t ms = -1;
        if (sparse) {
            hsize_t start = 0, count = (hsize_t)std::max<int64_t>(0, std::min(stored, kSample));
            v.resize((size_t)count);
            if (count) {
                H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
                ms = H5Screate_simple(1, &count, nullptr);
            }
        } else if (rows > 0 && cols > 0) {
            const int64_t sc = std::min(cols, kSample);
            const int64_t sr = std::max<int64_t>(1, std::min(rows, kSample / sc));
            hsize_t start[2] = {0, 0}, count[2] = {(hsize_t)sr, (hsize_t)sc};
            v.resize((size_t)(sr * sc));
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr);
            ms = H5Screate_simple(2, count, nullptr);
        }
        if (ms >= 0) {
            if (h5_read(d, H5T_NATIVE_DOUBLE, ms, fs, v.data()) < 0) err = h5_read_failure(d);
            H5Sclose(ms);
        }
        H5Sclose(fs);
    }
    H5Dclose(d);

    auto num = [](double x) { char b[32]; std::snprintf(b, sizeof b, "%g", x); return std::string(b); };
    auto pct = [](double x) { char b[32]; std::snprintf(b, sizeof b, "%.3g%%", x); return std::string(b); };
    std::string out = dtype;
    const double cells = (double)rows * (double)cols;
    if (sparse) {
        out += "  |  " + std::to_string(stored) + " stored of " + std::to_string(rows) +
               " \xc3\x97 " + std::to_string(cols);
        if (cells > 0) out += " (" + pct(100.0 * (double)stored / cells) + ")";
    } else {
        out += "  |  dense " + std::to_string(rows) + " \xc3\x97 " + std::to_string(cols);
    }
    if (!err.empty()) return out + "  |  sample unreadable: " + err;
    if (v.empty()) return out;

    double mx = v[0], mn = v[0], example = 0;
    bool whole = true;
    int64_t zeros = 0;
    for (double x : v) {
        mx = std::max(mx, x);
        mn = std::min(mn, x);
        if (x == 0) ++zeros;
        if (whole && (!std::isfinite(x) || x != std::floor(x))) { whole = false; example = x; }
    }
    const int64_t total = sparse ? stored : rows * cols;
    out += "  |  " + std::string((int64_t)v.size() == total ? "all " : "first ") +
           std::to_string(v.size()) + " values: max " + num(mx);
    if (!sparse) out += ", " + pct(100.0 * (double)zeros / (double)v.size()) + " zero";
    if (mx == 0 && mn == 0)
        out += ", all zero";
    else if (cls == H5T_INTEGER)
        out += ", integer dtype";
    else if (whole && mn >= 0)
        out += ", all non-negative whole numbers (looks like raw counts)";
    else if (whole)
        out += ", all whole numbers";
    else
        out += ", not whole numbers (e.g. " + num(example) + ")";
    return out;
}

// ── Storage layout of an AnnData matrix (summary tab) ───────────────────────
//
// How a matrix is laid out in the file — encoding, chunking, compression,
// bytes on disk — read from HDF5 metadata alone (no array data), so it costs
// milliseconds even on a network mount. Chunking decides what a row slice
// costs: HDF5 decompresses whole chunks, so a slice of a sparse matrix reads
// at least one full chunk of `data` and of `indices`.

static std::string h5_size_label(double bytes) {
    char buf[32];
    if      (bytes < 1024)               std::snprintf(buf, sizeof buf, "%.0f B", bytes);
    else if (bytes < 1024.0 * 1024)      std::snprintf(buf, sizeof buf, "%.1f KiB", bytes / 1024);
    else if (bytes < 1024.0 * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MiB", bytes / (1024.0 * 1024));
    else                                 std::snprintf(buf, sizeof buf, "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

static std::string h5_count_label(int64_t n) { return digits_with_sep(std::to_string(n)); }

// The filter pipeline of a dataset: "gzip 4 + shuffle", "lzf", "none", …
static std::string h5_filters_label(hid_t dcpl) {
    std::string out;
    const int nf = H5Pget_nfilters(dcpl);
    for (int i = 0; i < nf; ++i) {
        unsigned flags = 0, cfg = 0;
        size_t nelm = 8;
        unsigned cd[8] = {0};
        char name[64] = {0};
        const H5Z_filter_t id = H5Pget_filter2(dcpl, (unsigned)i, &flags, &nelm, cd,
                                               sizeof name, name, &cfg);
        std::string f;
        switch (id) {
            case H5Z_FILTER_DEFLATE:     f = "gzip " + std::to_string(nelm ? cd[0] : 0); break;
            case H5Z_FILTER_SHUFFLE:     f = "shuffle"; break;
            case H5Z_FILTER_FLETCHER32:  f = "fletcher32"; break;
            case H5Z_FILTER_SZIP:        f = "szip"; break;
            case H5Z_FILTER_NBIT:        f = "nbit"; break;
            case H5Z_FILTER_SCALEOFFSET: f = "scaleoffset"; break;
            case 32000: f = "lzf"; break;
            case 32001: f = "blosc"; break;
            case 32004: f = "lz4"; break;
            case 32008: f = "bitshuffle"; break;
            case 32015: f = "zstd"; break;
            default:    f = name[0] ? std::string(name) : "filter " + std::to_string(id); break;
        }
        out += (out.empty() ? "" : " + ") + f;
    }
    return out.empty() ? "no compression" : out;
}

struct H5Layout {
    std::string dtype;
    int64_t     n = 0;             // elements
    size_t      itemsize = 0;
    bool        chunked = false;
    std::vector<hsize_t> chunk;    // chunk dims when chunked
    int64_t     chunk_elems = 0;
    std::string filters;
    hsize_t     stored = 0;        // bytes allocated in the file
};

static bool h5_layout(hid_t loc, const char* name, H5Layout* out) {
    if (!link_exists(loc, name)) return false;
    hid_t d = H5Dopen2(loc, name, H5P_DEFAULT);
    if (d < 0) return false;
    hid_t t = H5Dget_type(d), sp = H5Dget_space(d), dcpl = H5Dget_create_plist(d);
    out->dtype = dtype_to_string(t);
    out->itemsize = H5Tget_size(t);
    out->n = (int64_t)H5Sget_simple_extent_npoints(sp);
    const int rank = H5Sget_simple_extent_ndims(sp);
    if (H5Pget_layout(dcpl) == H5D_CHUNKED && rank > 0) {
        out->chunked = true;
        out->chunk.assign((size_t)rank, 0);
        H5Pget_chunk(dcpl, rank, out->chunk.data());
        out->chunk_elems = 1;
        for (auto c : out->chunk) out->chunk_elems *= (int64_t)c;
    }
    out->filters = h5_filters_label(dcpl);
    out->stored = H5Dget_storage_size(d);
    H5Pclose(dcpl); H5Sclose(sp); H5Tclose(t); H5Dclose(d);
    return true;
}

static std::string h5_layout_label(const H5Layout& l) {
    std::string s = l.dtype + "  |  ";
    if (l.chunked) {
        std::string dims;
        for (size_t i = 0; i < l.chunk.size(); ++i)
            dims += (i ? " \xc3\x97 " : "") + h5_count_label((int64_t)l.chunk[i]);
        s += "chunks of " + dims + " (" +
             h5_size_label((double)l.chunk_elems * (double)l.itemsize) + ")";
    } else {
        s += "contiguous";
    }
    s += "  |  " + l.filters + "  |  " + h5_size_label((double)l.stored) + " on disk";
    return s;
}

// The last element of a 1-D integer dataset (a sparse matrix's indptr[-1],
// its stored-value count); -1 when unreadable.
static int64_t h5_last_int(hid_t loc, const char* name) {
    hid_t d = H5Dopen2(loc, name, H5P_DEFAULT);
    if (d < 0) return -1;
    hid_t sp = H5Dget_space(d);
    int64_t v = -1;
    hsize_t n = 0;
    if (H5Sget_simple_extent_ndims(sp) == 1 && H5Sget_simple_extent_dims(sp, &n, nullptr) == 1 && n > 0) {
        hsize_t start = n - 1, count = 1;
        H5Sselect_hyperslab(sp, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
        hid_t ms = H5Screate_simple(1, &count, nullptr);
        if (H5Dread(d, H5T_NATIVE_INT64, ms, sp, H5P_DEFAULT, &v) < 0) v = -1;
        H5Sclose(ms);
    }
    H5Sclose(sp); H5Dclose(d);
    return v;
}

// Summary rows for the matrix at `path` (X, raw/X, layers/<name>), labelled
// `label`: encoding, stored values, bytes on disk, and each component's
// chunking and compression. For a sparse matrix, also how many rows one
// chunk of `data` spans — flagged when a chunk is over 64 MiB, since every
// row slice then decompresses at least that much of `data` and of `indices`.
template <typename Add>
static void matrix_storage_rows(hid_t file_id, const std::string& path, const std::string& label,
                                Add&& add) {
    if (!link_exists(file_id, path.c_str() + 1)) return;
    if (is_group(file_id, path.c_str() + 1)) {
        hid_t g = H5Gopen2(file_id, path.c_str() + 1, H5P_DEFAULT);
        const std::string enc = read_string_attr(g, "encoding-type");
        if (enc != "csr_matrix" && enc != "csc_matrix") { H5Gclose(g); return; }
        const std::string ver = read_string_attr(g, "encoding-version");
        int64_t shape[2] = {0, 0};
        read_shape2(g, "shape", shape);
        const bool csr = enc == "csr_matrix";
        const int64_t major = csr ? shape[0] : shape[1];
        const int64_t nnz = h5_last_int(g, "indptr");
        H5Layout parts[3];
        const char* names[3] = {"data", "indices", "indptr"};
        hsize_t stored = 0;
        double raw = 0;
        for (int i = 0; i < 3; ++i)
            if (h5_layout(g, names[i], &parts[i])) {
                stored += parts[i].stored;
                raw += (double)parts[i].n * (double)parts[i].itemsize;
            }
        H5Gclose(g);
        std::string s = enc + (ver.empty() ? "" : " " + ver);
        if (nnz >= 0) {
            s += "  |  " + h5_count_label(nnz) + " stored values";
            if (major > 0) {
                char per[64];
                std::snprintf(per, sizeof per, " (%s per %s)",
                              h5_count_label((int64_t)std::llround((double)nnz / (double)major)).c_str(),
                              csr ? "row" : "column");
                s += per;
            }
        }
        s += "  |  " + h5_size_label((double)stored) + " on disk";
        if (stored > 0) {
            char r[48];
            std::snprintf(r, sizeof r, " for %s raw (%.1f\xc3\x97)", h5_size_label(raw).c_str(),
                          raw / (double)stored);
            s += r;
        }
        add(label + " storage", s);
        for (int i = 0; i < 3; ++i)
            if (!parts[i].dtype.empty()) add(label + "/" + names[i], h5_layout_label(parts[i]));
        const H5Layout& data = parts[0];
        const H5Layout& idx = parts[1];
        if (nnz > 0 && major > 0 && data.chunked && data.chunk_elems > 0) {
            const double per_major = (double)nnz / (double)major;
            const double chunk_bytes = (double)data.chunk_elems * (double)data.itemsize;
            const double idx_bytes = idx.chunked ? (double)idx.chunk_elems * (double)idx.itemsize
                                                 : (double)idx.n * (double)idx.itemsize;
            std::string note = "one data chunk \xe2\x89\x88 " +
                h5_count_label((int64_t)std::llround((double)data.chunk_elems / per_major)) +
                (csr ? " rows" : " columns") + "; a slice decompresses at least " +
                h5_size_label(chunk_bytes) + " of data and " + h5_size_label(idx_bytes) +
                " of indices";
            if (chunk_bytes > 64.0 * 1024 * 1024) note += "  (large chunks)";
            add(label + (csr ? " row slice" : " column slice"), note);
        }
        return;
    }
    // Dense: one dataset.
    H5Layout d;
    const std::string parent = path.substr(0, path.rfind('/'));
    const std::string leaf = path.substr(path.rfind('/') + 1);
    hid_t loc = parent.empty() ? file_id : H5Gopen2(file_id, parent.c_str(), H5P_DEFAULT);
    if (loc < 0) return;
    const bool ok = h5_layout(loc, leaf.c_str(), &d);
    if (loc != file_id) H5Gclose(loc);
    if (ok) add(label + " storage", "dense " + h5_layout_label(d));
}

static std::vector<OpenSpec> scan_anndata(hid_t fid, const std::string& root) {
    // `root`: the AnnData's group ("/mod/rna" inside a MuData file; "" for the
    // file itself). Lookups are relative to it; stored paths are absolute.
    const hid_t file_id = root.empty() ? fid : H5Gopen2(fid, root.c_str(), H5P_DEFAULT);
    struct CloseRoot { hid_t g; bool own; ~CloseRoot() { if (own && g >= 0) H5Gclose(g); } }
        close_root{file_id, !root.empty()};
    std::vector<OpenSpec> specs;

    // Summary tab (key/value rows). The storage-layout rows are collected
    // apart and appended last, so a preview of the summary's first rows still
    // shows the matrix, obs / var and the other entries.
    std::string summary, storage;
    auto add = [&](const std::string& k, const std::string& v) {
        summary += k; summary += '\t'; summary += v; summary += '\n';
    };
    auto add_storage = [&](const std::string& k, const std::string& v) {
        storage += k; storage += '\t'; storage += v; storage += '\n';
    };
    add("format", "AnnData");
    std::string enc = read_string_attr(file_id, "encoding-type");
    if (!enc.empty()) add("root-encoding", enc);
    if (hsize_t fsz = 0; root.empty() && H5Fget_filesize(fid, &fsz) >= 0)
        add_storage("file size", h5_size_label((double)fsz));

    // X (matrix). Either a dataset (dense) or a group with
    // encoding-type ∈ {csr_matrix, csc_matrix}.
    bool x_is_sparse = false;
    int64_t x_rows = 0, x_cols = 0;
    if (link_exists(file_id, "X")) {
        if (is_group(file_id, "X")) {
            hid_t g = H5Gopen2(file_id, "X", H5P_DEFAULT);
            std::string xenc = read_string_attr(g, "encoding-type");
            if (xenc == "csr_matrix" || xenc == "csc_matrix") {
                x_is_sparse = true;
                int64_t shape[2] = {0, 0};
                read_shape2(g, "shape", shape);
                x_rows = shape[0]; x_cols = shape[1];
                add("X", xenc + "  (" + std::to_string(x_rows) +
                          " \xc3\x97 " + std::to_string(x_cols) + ")");
                add("X profile", matrix_profile(fid, root + "/X"));
                matrix_storage_rows(fid, root + "/X", "X", add_storage);
                // Both CSR and CSC densify to the same rows × columns preview.
                specs.push_back({OpenSpec::Kind::Sparse, root + "/X",
                                  "X (preview)",
                                  xenc + "  shape: " +
                                  std::to_string(x_rows) + " \xc3\x97 " +
                                  std::to_string(x_cols)});
            }
            H5Gclose(g);
        } else {
            hid_t d = H5Dopen2(file_id, "X", H5P_DEFAULT);
            hid_t s = H5Dget_space(d);
            int nd = H5Sget_simple_extent_ndims(s);
            std::vector<hsize_t> dims((size_t)nd);
            if (nd > 0) H5Sget_simple_extent_dims(s, dims.data(), nullptr);
            H5Sclose(s); H5Dclose(d);
            if (nd == 2) {
                x_rows = (int64_t)dims[0]; x_cols = (int64_t)dims[1];
                add("X", "dense  (" + std::to_string(x_rows) +
                          " \xc3\x97 " + std::to_string(x_cols) + ")");
                add("X profile", matrix_profile(fid, root + "/X"));
                matrix_storage_rows(fid, root + "/X", "X", add_storage);
                specs.push_back({OpenSpec::Kind::Matrix2D, root + "/X",
                                  "X", "dense"});
            }
        }
    }

    if (link_exists(file_id, "obs") && is_group(file_id, "obs")) {
        hid_t g = H5Gopen2(file_id, "obs", H5P_DEFAULT);
        add("obs", std::to_string(x_rows) + " rows, " +
                    std::to_string(anndata_column_count(g)) + " columns");
        H5Gclose(g);
        specs.push_back({OpenSpec::Kind::DataFrame, root + "/obs", "obs", ""});
    }
    if (link_exists(file_id, "var") && is_group(file_id, "var")) {
        hid_t g = H5Gopen2(file_id, "var", H5P_DEFAULT);
        add("var", std::to_string(x_cols) + " rows, " +
                    std::to_string(anndata_column_count(g)) + " columns");
        H5Gclose(g);
        specs.push_back({OpenSpec::Kind::DataFrame, root + "/var", "var", ""});
    }

    // obsm / varm / layers — each child becomes its own tab.
    auto add_subgroup_tabs = [&](const char* parent_name,
                                   OpenSpec::Kind k,
                                   const char* footer_kind,
                                   AnnMatrixAxes axes) {
        if (!link_exists(file_id, parent_name) ||
            !is_group(file_id, parent_name)) return;
        hid_t g = H5Gopen2(file_id, parent_name, H5P_DEFAULT);
        auto names = list_children(g);
        for (const auto& nm : names) {
            OpenSpec s{k, root + "/" + parent_name + "/" + nm,
                       std::string(parent_name) + "[" + nm + "]",
                       footer_kind, axes, nm};
            // A CSR/CSC entry is a group, not a dataset — scanpy writes layers
            // of a sparse X this way — so read it like a sparse X.
            if (is_group(g, nm.c_str())) {
                hid_t eg = H5Gopen2(g, nm.c_str(), H5P_DEFAULT);
                std::string enc = read_string_attr(eg, "encoding-type");
                if (enc == "csr_matrix" || enc == "csc_matrix") {
                    int64_t shape[2] = {0, 0};
                    read_shape2(eg, "shape", shape);
                    s.kind = OpenSpec::Kind::Sparse;
                    s.footer_hint = enc + "  shape: " + std::to_string(shape[0]) +
                                    " \xc3\x97 " + std::to_string(shape[1]);
                }
                H5Gclose(eg);
            }
            specs.push_back(std::move(s));
        }
        H5Gclose(g);
        if (!names.empty())
            add(parent_name, std::to_string(names.size()) + " entries");
        // layers mirror X: profile each, so raw counts stored as a layer show.
        if (axes == AnnMatrixAxes::ObsByVar)
            for (const auto& nm : names) {
                if (auto pr = matrix_profile(fid, root + "/" + parent_name + "/" + nm);
                    !pr.empty())
                    add(std::string(parent_name) + "[" + nm + "] profile", pr);
                matrix_storage_rows(fid, root + "/" + parent_name + "/" + nm,
                                    std::string(parent_name) + "[" + nm + "]", add_storage);
            }
    };
    // layers/* mirror X's shape, so they keep gene columns; obsm/varm do not.
    add_subgroup_tabs("obsm",   OpenSpec::Kind::Matrix2D, "obsm",
                      AnnMatrixAxes::ObsByDim);
    add_subgroup_tabs("varm",   OpenSpec::Kind::Matrix2D, "varm",
                      AnnMatrixAxes::VarByDim);
    add_subgroup_tabs("layers", OpenSpec::Kind::Matrix2D, "layer",
                      AnnMatrixAxes::ObsByVar);

    // obsp / varp: pairwise graphs over cells / genes (scanpy's neighbour
    // connectivities and distances), n × n and sparse. Each becomes an edge-list
    // tab (i, j, <axis>_i, <axis>_j, weight) streamed from the sparse matrix, so
    // a 4.7M-cell kNN graph is never densified. A dense entry is listed in the
    // summary only.
    for (const char* grp : {"obsp", "varp"}) {
        if (!link_exists(file_id, grp) || !is_group(file_id, grp)) continue;
        const std::string axis = std::string(grp).substr(0, 3);   // obs / var
        hid_t g = H5Gopen2(file_id, grp, H5P_DEFAULT);
        std::vector<std::string> listed;
        for (const auto& nm : list_children(g)) {
            std::string enc;
            if (is_group(g, nm.c_str())) {
                hid_t eg = H5Gopen2(g, nm.c_str(), H5P_DEFAULT);
                enc = read_string_attr(eg, "encoding-type");
                H5Gclose(eg);
            }
            if (enc == "csr_matrix" || enc == "csc_matrix") {
                OpenSpec sp{OpenSpec::Kind::EdgeList, root + "/" + grp + "/" + nm,
                            std::string(grp) + "[" + nm + "]", enc};
                sp.key = axis;
                specs.push_back(std::move(sp));
                listed.push_back(nm);
            } else {
                listed.push_back(nm + " (dense, not shown)");
            }
        }
        H5Gclose(g);
        if (!listed.empty()) {
            std::string v;
            for (const auto& l : listed) v += (v.empty() ? "" : ", ") + l;
            add(grp, v);
        }
    }

    // raw: the unfiltered matrix scanpy keeps beside a processed X (raw
    // counts over every gene, while X is normalised and subset). raw/X is
    // cells × raw genes, labelled by obs and raw/var; raw/var is a DataFrame.
    if (link_exists(file_id, "raw") && is_group(file_id, "raw")) {
        hid_t rg = H5Gopen2(file_id, "raw", H5P_DEFAULT);
        int64_t raw_cols = 0;   // raw/X columns = raw/var rows
        if (link_exists(rg, "X")) {
            if (is_group(rg, "X")) {
                hid_t g = H5Gopen2(rg, "X", H5P_DEFAULT);
                std::string xenc = read_string_attr(g, "encoding-type");
                if (xenc == "csr_matrix" || xenc == "csc_matrix") {
                    int64_t shape[2] = {0, 0};
                    read_shape2(g, "shape", shape);
                    raw_cols = shape[1];
                    const std::string dims = std::to_string(shape[0]) + " \xc3\x97 " +
                                             std::to_string(shape[1]);
                    add("raw.X", xenc + "  (" + dims + ")");
                    add("raw.X profile", matrix_profile(fid, root + "/raw/X"));
                    matrix_storage_rows(fid, root + "/raw/X", "raw.X", add_storage);
                    specs.push_back({OpenSpec::Kind::Sparse, root + "/raw/X", "raw.X (preview)",
                                     xenc + "  shape: " + dims,
                                     AnnMatrixAxes::ObsByRawVar, ""});
                }
                H5Gclose(g);
            } else {
                hid_t d = H5Dopen2(rg, "X", H5P_DEFAULT);
                hid_t sp = H5Dget_space(d);
                hsize_t dims[2] = {0, 0};
                const int nd = H5Sget_simple_extent_ndims(sp);
                if (nd == 2) H5Sget_simple_extent_dims(sp, dims, nullptr);
                H5Sclose(sp); H5Dclose(d);
                if (nd == 2) {
                    raw_cols = (int64_t)dims[1];
                    add("raw.X", "dense  (" + std::to_string(dims[0]) + " \xc3\x97 " +
                                 std::to_string(dims[1]) + ")");
                    add("raw.X profile", matrix_profile(fid, root + "/raw/X"));
                    matrix_storage_rows(fid, root + "/raw/X", "raw.X", add_storage);
                    specs.push_back({OpenSpec::Kind::Matrix2D, root + "/raw/X", "raw.X", "dense",
                                     AnnMatrixAxes::ObsByRawVar, ""});
                }
            }
        }
        if (link_exists(rg, "var") && is_group(rg, "var")) {
            hid_t g = H5Gopen2(rg, "var", H5P_DEFAULT);
            add("raw.var", std::to_string(raw_cols) + " rows, " +
                           std::to_string(anndata_column_count(g)) + " columns");
            H5Gclose(g);
            specs.push_back({OpenSpec::Kind::DataFrame, root + "/raw/var", "raw.var", ""});
        }
        H5Gclose(rg);
    }

    // uns (unstructured): one key/value tab surfacing scalars, strings and
    // small arrays (nested dicts flattened with dotted keys). Previously skipped.
    if (link_exists(file_id, "uns") && is_group(file_id, "uns")) {
        hid_t g = H5Gopen2(file_id, "uns", H5P_DEFAULT);
        auto names = list_children(g);
        H5Gclose(g);
        if (!names.empty()) {
            specs.push_back({OpenSpec::Kind::Uns, root + "/uns", "uns", ""});
            add("uns", std::to_string(names.size()) + " entries");
        }
    }

    // Prepend the summary tab.
    summary += storage;
    OpenSpec sum_spec{OpenSpec::Kind::Summary, root.empty() ? "/" : root, "summary", summary};
    specs.insert(specs.begin(), sum_spec);
    return specs;
}

// ── obsp / varp: a sparse graph as a streamed edge list ─────────────────────
//
// One row per stored entry of an n × n CSR / CSC matrix: its row and column
// (0-based), their obs / var names, and the value. Chunks are fixed runs of
// kEdgesPerChunk entries read as hyperslabs of indices / data, so memory is
// bounded by one chunk plus indptr and the names; nothing is densified. The
// file is read on first access, not at construction, so listing tabs is free.
class H5EdgeListSource : public TabularSource {
    static constexpr int64_t kEdgesPerChunk = 1 << 20;
    std::string path_;
    H5FilePtr   file_;
    OpenSpec    spec_;
    std::shared_ptr<arrow::Schema> schema_;
    mutable bool          init_ = false;
    mutable arrow::Status status_;
    mutable bool          csr_ = true;
    mutable int64_t       n_ = 0;          // matrix is n × n (the axis length)
    mutable int64_t       first_ = 0;      // indptr[0]: offset of the first entry
    mutable int64_t       nnz_ = 0;        // entries listed
    mutable std::vector<int64_t>     indptr_;   // monotone, clamped
    mutable std::vector<std::string> names_;
    mutable std::string   footer_;

    void init() const {
        if (init_) return;
        init_ = true;
        hid_t g = H5Gopen2(*file_, spec_.h5_path.c_str(), H5P_DEFAULT);
        if (g < 0) { status_ = arrow::Status::IOError("cannot open ", spec_.h5_path); return; }
        csr_ = read_string_attr(g, "encoding-type") != "csc_matrix";
        int64_t shape[2] = {0, 0};
        read_shape2(g, "shape", shape);
        hid_t ip = H5Dopen2(g, "indptr", H5P_DEFAULT);
        hid_t ix = H5Dopen2(g, "indices", H5P_DEFAULT);
        hid_t dt = H5Dopen2(g, "data", H5P_DEFAULT);
        if (ip < 0 || ix < 0 || dt < 0) {
            if (ip >= 0) H5Dclose(ip);
            if (ix >= 0) H5Dclose(ix);
            if (dt >= 0) H5Dclose(dt);
            H5Gclose(g);
            status_ = arrow::Status::IOError(spec_.h5_path, ": missing indptr / indices / data");
            return;
        }
        // The shape attribute and indptr are untrusted: the listed entries
        // must stay inside indices / data, and indptr must not run backwards.
        const int64_t avail = std::min(h5_len_1d(ix), h5_len_1d(dt));
        const int64_t len = h5_len_1d(ip);
        int64_t major = std::max<int64_t>(0, std::min<int64_t>(csr_ ? shape[0] : shape[1], len - 1));
        indptr_.assign((size_t)major + 1, 0);
        if (major >= 0 && len > 0) {
            hid_t fs = H5Dget_space(ip);
            hsize_t start = 0, count = (hsize_t)(major + 1);
            H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &count, nullptr);
            hid_t ms = H5Screate_simple(1, &count, nullptr);
            if (h5_read(ip, H5T_NATIVE_INT64, ms, fs, indptr_.data()) < 0)
                status_ = arrow::Status::IOError(h5_read_failure(ip));
            H5Sclose(ms); H5Sclose(fs);
        }
        H5Dclose(ip); H5Dclose(ix); H5Dclose(dt);
        H5Gclose(g);
        if (!status_.ok()) return;
        for (size_t k = 0; k < indptr_.size(); ++k) {
            int64_t lo = k ? indptr_[k - 1] : 0;
            indptr_[k] = std::min(std::max(indptr_[k], lo), avail);
        }
        first_ = indptr_.front();
        nnz_ = indptr_.back() - first_;
        n_ = std::max(shape[0], shape[1]);
        std::string idx_name;
        read_anndata_index_labels(*file_, anndata_root(spec_.h5_path) + (spec_.key == "var" ? "/var" : "/obs"), n_,
                                  &names_, &idx_name);
        footer_ = "Format: AnnData " + spec_.display + "  |  " + spec_.footer_hint +
                  "  " + std::to_string(shape[0]) + " \xc3\x97 " + std::to_string(shape[1]) +
                  "  |  " + std::to_string(nnz_) + (nnz_ == 1 ? " edge" : " edges");
    }

public:
    H5EdgeListSource(std::string path, H5FilePtr file, OpenSpec spec)
        : path_(std::move(path)), file_(std::move(file)), spec_(std::move(spec)) {
        const std::string a = spec_.key == "var" ? "var" : "obs";
        schema_ = arrow::schema({arrow::field("i", arrow::int64()),
                                 arrow::field("j", arrow::int64()),
                                 arrow::field(a + "_i", arrow::utf8()),
                                 arrow::field(a + "_j", arrow::utf8()),
                                 arrow::field("weight", arrow::float64())});
    }
    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { init(); return status_.ok() ? nnz_ : 0; }
    int num_chunks() const override {
        init();
        return status_.ok() ? (int)((nnz_ + kEdgesPerChunk - 1) / kEdgesPerChunk) : 0;
    }
    ChunkMeta chunk_meta(int i) const override {
        const int64_t first = (int64_t)i * kEdgesPerChunk;
        return {first, std::max<int64_t>(0, std::min(kEdgesPerChunk, nnz_ - first))};
    }
    arrow::Status read_status() const override { init(); return status_; }
    const std::string& path() const override { return path_; }
    std::string tab_label() const override { return spec_.display; }
    std::string footer() const override { init(); return footer_; }

    arrow::Status read_chunk(int c, const std::vector<int>& cols,
                             std::shared_ptr<arrow::Table>* out) override {
        init();
        ARROW_RETURN_NOT_OK(status_);
        const ChunkMeta m = chunk_meta(c);
        const int64_t p0 = first_ + m.first_row, count = m.num_rows;
        std::vector<int64_t> idx((size_t)count);
        std::vector<double>  val((size_t)count);
        if (count > 0) {
            hid_t g = H5Gopen2(*file_, spec_.h5_path.c_str(), H5P_DEFAULT);
            if (g < 0) return arrow::Status::IOError("cannot open ", spec_.h5_path);
            std::string err;
            for (int pass = 0; pass < 2; ++pass) {
                hid_t d = H5Dopen2(g, pass ? "data" : "indices", H5P_DEFAULT);
                hid_t fs = H5Dget_space(d);
                hsize_t start = (hsize_t)p0, cnt = (hsize_t)count;
                H5Sselect_hyperslab(fs, H5S_SELECT_SET, &start, nullptr, &cnt, nullptr);
                hid_t ms = H5Screate_simple(1, &cnt, nullptr);
                herr_t st = pass ? h5_read(d, H5T_NATIVE_DOUBLE, ms, fs, val.data())
                                 : h5_read(d, H5T_NATIVE_INT64, ms, fs, idx.data());
                if (st < 0 && err.empty()) err = h5_read_failure(d);
                H5Sclose(ms); H5Sclose(fs); H5Dclose(d);
            }
            H5Gclose(g);
            if (!err.empty()) return arrow::Status::IOError(err);
        }
        // The compressed-axis index of each entry: the slice k with
        // indptr[k] <= p < indptr[k+1].
        arrow::Int64Builder ib, jb;
        arrow::StringBuilder nib, njb;
        arrow::DoubleBuilder wb;
        ARROW_RETURN_NOT_OK(ib.Reserve(count));
        ARROW_RETURN_NOT_OK(jb.Reserve(count));
        ARROW_RETURN_NOT_OK(wb.Reserve(count));
        size_t k = (size_t)(std::upper_bound(indptr_.begin(), indptr_.end(), p0) - indptr_.begin());
        k = k ? k - 1 : 0;
        auto name = [&](arrow::StringBuilder& b, int64_t x) {
            if (x >= 0 && x < (int64_t)names_.size()) (void)b.Append(names_[(size_t)x]);
            else                                      (void)b.AppendNull();
        };
        for (int64_t e = 0; e < count; ++e) {
            const int64_t p = p0 + e;
            while (k + 1 < indptr_.size() && indptr_[k + 1] <= p) ++k;
            const int64_t major = (int64_t)k, minor = idx[(size_t)e];
            const int64_t i = csr_ ? major : minor, j = csr_ ? minor : major;
            ib.UnsafeAppend(i);
            jb.UnsafeAppend(j);
            name(nib, i);
            name(njb, j);
            wb.UnsafeAppend(val[(size_t)e]);
        }
        std::shared_ptr<arrow::Array> ai, aj, ani, anj, aw;
        ARROW_RETURN_NOT_OK(ib.Finish(&ai));
        ARROW_RETURN_NOT_OK(jb.Finish(&aj));
        ARROW_RETURN_NOT_OK(nib.Finish(&ani));
        ARROW_RETURN_NOT_OK(njb.Finish(&anj));
        ARROW_RETURN_NOT_OK(wb.Finish(&aw));
        const std::vector<std::shared_ptr<arrow::Array>> all{ai, aj, ani, anj, aw};
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (int ci : cols) {
            if (ci < 0 || ci >= (int)all.size()) continue;
            fields.push_back(schema_->field(ci));
            arrays.push_back(all[(size_t)ci]);
        }
        *out = arrow::Table::Make(arrow::schema(fields), arrays, count);
        return arrow::Status::OK();
    }
};

static std::unique_ptr<TabularSource> make_edge_list_source(const std::string& path,
                                                            H5FilePtr file,
                                                            const OpenSpec& spec) {
    return std::make_unique<H5EdgeListSource>(path, std::move(file), spec);
}

// ── 10x Genomics Cell Ranger HDF5 (filtered_feature_bc_matrix.h5) ─────────
//
// Cell Ranger v3+ keeps one matrix under /matrix: barcodes, data, indices,
// indptr, shape and a features/ group (id, name, feature_type, genome, …).
// v2 keeps one such group per reference genome (/GRCh38, /mm10, …) with
// genes / gene_names instead of features/. Either way the matrix is stored
// features × barcodes as CSC — indptr runs over barcodes — which is exactly a
// cells × features CSR, the orientation AnnData and scanpy present, so that
// is how it is shown. OpenSpec::h5_path is the matrix group; key is "v3"/"v2".

static bool tenx_matrix_group(hid_t parent, const char* name, bool v3) {
    if (!link_exists(parent, name) || !is_group(parent, name)) return false;
    hid_t g = H5Gopen2(parent, name, H5P_DEFAULT);
    if (g < 0) return false;
    bool ok = true;
    for (const char* c : {"barcodes", "data", "indices", "indptr", "shape"})
        ok = ok && link_exists(g, c);
    ok = ok && (v3 ? (link_exists(g, "features") && is_group(g, "features"))
                   : (link_exists(g, "genes") && link_exists(g, "gene_names")));
    H5Gclose(g);
    return ok;
}

// Matrix groups of a 10x file: {"/matrix"} for v3, one per genome for v2.
// Empty when the file is not a 10x matrix file.
static std::vector<std::string> tenx_matrix_groups(hid_t fid, bool* v3) {
    std::vector<std::string> out;
    if (tenx_matrix_group(fid, "matrix", true)) { *v3 = true; return {"/matrix"}; }
    *v3 = false;
    for (const auto& c : list_children(fid))
        if (tenx_matrix_group(fid, c.c_str(), false)) out.push_back("/" + c);
    return out;
}

// Up to `cap` (< 0: all) values of a 1-D dataset as text; empty on any error.
static std::vector<std::string> h5_strings(hid_t loc, const std::string& path,
                                           int64_t cap, int64_t* full = nullptr) {
    std::vector<std::string> out;
    hid_t d = H5Dopen2(loc, path.c_str(), H5P_DEFAULT);
    if (d < 0) return out;
    auto t = read_1d_dataset_table(d, cap, full);
    H5Dclose(d);
    if (!t.ok() || (*t)->num_columns() == 0) return out;
    auto col = (*t)->column(0);
    for (const auto& ch : col->chunks())
        for (int64_t i = 0; i < ch->length(); ++i) out.push_back(cell_to_string(*ch, i));
    return out;
}

// [n_features, n_barcodes] from the matrix group's `shape` dataset.
static bool tenx_shape(hid_t g, int64_t* nf, int64_t* nb) {
    hid_t d = H5Dopen2(g, "shape", H5P_DEFAULT);
    if (d < 0) return false;
    auto t = read_1d_dataset_table(d, 2);
    H5Dclose(d);
    if (!t.ok() || (*t)->num_rows() < 2 ||
        (*t)->column(0)->type()->id() != arrow::Type::INT64) return false;
    auto a = std::static_pointer_cast<arrow::Int64Array>((*t)->column(0)->chunk(0));
    *nf = a->Value(0); *nb = a->Value(1);
    return *nf >= 0 && *nb >= 0;
}

static std::vector<OpenSpec> scan_10x(hid_t fid, const std::vector<std::string>& groups,
                                      bool v3) {
    std::vector<OpenSpec> specs;
    std::string summary;
    auto add = [&](const std::string& k, const std::string& v) {
        summary += k; summary += '\t'; summary += v; summary += '\n';
    };
    add("format", std::string("10x Genomics Cell Ranger HDF5 (") +
                  (v3 ? "v3+" : "v2, one matrix per genome") + ")");
    for (const char* a : {"filetype", "version", "software_version",
                          "chemistry_description"}) {
        std::string v = read_string_attr(fid, a);
        if (!v.empty()) add(a, v);
    }
    for (const auto& gp : groups) {
        hid_t g = H5Gopen2(fid, gp.c_str(), H5P_DEFAULT);
        if (g < 0) continue;
        std::string tag = v3 ? "" : "[" + gp.substr(1) + "]";
        int64_t nf = 0, nb = 0;
        tenx_shape(g, &nf, &nb);
        hid_t dd = H5Dopen2(g, "data", H5P_DEFAULT);
        int64_t nnz = dd >= 0 ? h5_len_1d(dd) : -1;
        if (dd >= 0) H5Dclose(dd);
        add("matrix" + tag, std::to_string(nb) + " barcodes \xc3\x97 " + std::to_string(nf) +
                            " features, " + std::to_string(nnz) + " stored entries");
        if (v3) {
            std::map<std::string, int64_t> by_type;
            for (const auto& t : h5_strings(g, "features/feature_type", -1)) ++by_type[t];
            for (const auto& kv : by_type)
                add("feature_type: " + kv.first, std::to_string(kv.second));
        }
        H5Gclose(g);
        specs.push_back({OpenSpec::Kind::TenxMatrix, gp, "matrix" + tag + " (preview)", "",
                         AnnMatrixAxes::ObsByVar, v3 ? "v3" : "v2"});
        specs.push_back({OpenSpec::Kind::TenxFeatures, gp, v3 ? "features" : "genes" + tag, "",
                         AnnMatrixAxes::ObsByVar, v3 ? "v3" : "v2"});
        specs.push_back({OpenSpec::Kind::TenxBarcodes, gp, "barcodes" + tag, "",
                         AnnMatrixAxes::ObsByVar, v3 ? "v3" : "v2"});
    }
    specs.insert(specs.begin(), OpenSpec{OpenSpec::Kind::Summary, "/", "summary", summary});
    return specs;
}

static std::string build_tenx_table(hid_t file_id, const OpenSpec& spec, int64_t row_cap,
                                    std::shared_ptr<arrow::Table>* out,
                                    std::string* footer) {
    const bool v3 = spec.key == "v3";
    hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
    if (g < 0) return "Cannot open group " + spec.h5_path;
    const std::string ids_path   = v3 ? "features/id"   : "genes";
    const std::string names_path = v3 ? "features/name" : "gene_names";
    std::string err;

    if (spec.kind == OpenSpec::Kind::TenxMatrix) {
        int64_t nf = 0, nb = 0;
        if (!tenx_shape(g, &nf, &nb)) { H5Gclose(g); return spec.h5_path + "/shape: unreadable"; }
        auto r = read_sparse_preview_as(g, nb, nf, /*is_csr=*/true, 1000);
        if (!r.ok()) { H5Gclose(g); return r.status().ToString(); }
        auto t = *r;
        const int64_t nr = t->num_rows(), nc = t->num_columns();
        // Columns: feature names; a name that repeats among the shown columns
        // gets its id appended so every header is unique.
        auto ids = h5_strings(g, ids_path, nc);
        auto names = h5_strings(g, names_path, nc);
        std::map<std::string, int> seen;
        for (const auto& n : names) ++seen[n];
        std::vector<std::string> cols;
        for (int64_t c = 0; c < nc; ++c) {
            std::string n = c < (int64_t)names.size() ? names[(size_t)c] : "";
            std::string id = c < (int64_t)ids.size() ? ids[(size_t)c] : "";
            if (n.empty()) n = id.empty() ? t->field((int)c)->name() : id;
            else if (seen[n] > 1 && !id.empty()) n += " (" + id + ")";
            cols.push_back(n);
        }
        auto rn = t->RenameColumns(cols);
        if (rn.ok()) t = *rn;
        auto bcs = h5_strings(g, "barcodes", nr);
        arrow::StringBuilder b;
        for (int64_t i = 0; i < nr; ++i) {
            if (i < (int64_t)bcs.size()) (void)b.Append(bcs[(size_t)i]); else (void)b.AppendNull();
        }
        std::shared_ptr<arrow::Array> a;
        if (b.Finish(&a).ok()) {
            auto ac = t->AddColumn(0, arrow::field("barcode", arrow::utf8()),
                                   std::make_shared<arrow::ChunkedArray>(a));
            if (ac.ok()) t = *ac;
        }
        *out = t;
        h5_note_preview(nr, nb, nc, nf);
        *footer = "Format: 10x Genomics HDF5 matrix  |  preview: first " + std::to_string(nr) +
                  " of " + std::to_string(nb) + " barcodes, first " + std::to_string(nc) +
                  " of " + std::to_string(nf) + " features  |  shown cells \xc3\x97 features "
                  "(stored features \xc3\x97 barcodes)";
    } else if (spec.kind == OpenSpec::Kind::TenxFeatures) {
        int64_t full = 0;
        std::shared_ptr<arrow::Table> t;
        if (v3) {
            hid_t fg = H5Gopen2(g, "features", H5P_DEFAULT);
            auto r = read_anndata_dataframe(fg, row_cap, &full);
            H5Gclose(fg);
            if (!r.ok()) err = r.status().ToString(); else t = *r;
            if (t) {
                // _all_tag_keys lists the names of the optional feature columns;
                // it is not per-feature, so it is not a column. Put the
                // documented columns first.
                std::vector<int> order;
                for (const char* want : {"id", "name", "feature_type", "genome"}) {
                    int i = t->schema()->GetFieldIndex(want);
                    if (i >= 0) order.push_back(i);
                }
                for (int i = 0; i < t->num_columns(); ++i) {
                    const std::string& n = t->field(i)->name();
                    if (n == "_all_tag_keys") continue;
                    if (std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
                }
                auto sel = t->SelectColumns(order);
                if (sel.ok()) t = *sel;
            }
        } else {
            int64_t full2 = 0;
            auto ids = h5_strings(g, ids_path, row_cap, &full);
            auto names = h5_strings(g, names_path, row_cap, &full2);
            arrow::StringBuilder bi, bn;
            for (size_t i = 0; i < ids.size(); ++i) {
                (void)bi.Append(ids[i]);
                if (i < names.size()) (void)bn.Append(names[i]); else (void)bn.AppendNull();
            }
            std::shared_ptr<arrow::Array> ai, an;
            (void)bi.Finish(&ai); (void)bn.Finish(&an);
            t = arrow::Table::Make(arrow::schema({arrow::field("id", arrow::utf8()),
                                                  arrow::field("name", arrow::utf8())}),
                                   {ai, an});
        }
        if (t) {
            *out = t;
            int64_t shown = t->num_rows();
            h5_note_preview(shown, full, t->num_columns(), t->num_columns());
            *footer = "Format: 10x Genomics features";
            *footer += shown < full ? "  |  preview: first " + std::to_string(shown) + " of " +
                                      std::to_string(full) + " rows"
                                    : "  |  Rows: " + std::to_string(shown);
        }
    } else {   // TenxBarcodes
        hid_t d = H5Dopen2(g, "barcodes", H5P_DEFAULT);
        int64_t full = 0;
        auto r = read_1d_dataset_table(d, row_cap, &full);
        if (d >= 0) H5Dclose(d);
        if (!r.ok()) err = r.status().ToString();
        else {
            auto rn = (*r)->RenameColumns({"barcode"});
            *out = rn.ok() ? *rn : *r;
            int64_t shown = (*out)->num_rows();
            h5_note_preview(shown, full, 1, 1);
            *footer = "Format: 10x Genomics barcodes";
            *footer += shown < full ? "  |  preview: first " + std::to_string(shown) + " of " +
                                      std::to_string(full) + " rows"
                                    : "  |  Rows: " + std::to_string(shown);
        }
    }
    H5Gclose(g);
    return err;
}

// ── Loom (loompy / velocyto / SCope) ───────────────────────────────────────
//
// /matrix is a genes × cells 2-D dataset, /layers/<name> are more of the same
// shape (velocyto's spliced / unspliced), /row_attrs holds one 1-D dataset per
// gene attribute and /col_attrs one per cell attribute. Matrices are shown
// cells × genes, the orientation AnnData and scanpy use, labelled by the cell
// and gene ID attributes. Writers name those differently, hence the lists.

static const char* const kLoomCellIds[] = {"CellID", "cell_id", "CellIDs", "obs_names",
                                           "cell_names", "Barcode", "barcode"};
static const char* const kLoomGeneIds[] = {"Gene", "gene", "Genes", "var_names",
                                           "gene_names", "gene_symbols", "Accession",
                                           "gene_ids"};

static bool is_loom(hid_t fid) {
    if (!link_exists(fid, "matrix") || is_group(fid, "matrix")) return false;
    if (!link_exists(fid, "row_attrs") || !is_group(fid, "row_attrs")) return false;
    if (!link_exists(fid, "col_attrs") || !is_group(fid, "col_attrs")) return false;
    hid_t d = H5Dopen2(fid, "matrix", H5P_DEFAULT);
    if (d < 0) return false;
    hid_t sp = H5Dget_space(d);
    int nd = H5Sget_simple_extent_ndims(sp);
    H5Sclose(sp); H5Dclose(d);
    return nd == 2;
}

// The first attribute in `group` named in `names` that exists, or "".
template <size_t N>
static std::string loom_label_attr(hid_t fid, const char* group, const char* const (&names)[N]) {
    hid_t g = H5Gopen2(fid, group, H5P_DEFAULT);
    if (g < 0) return "";
    std::string found;
    for (const char* n : names)
        if (link_exists(g, n)) { found = n; break; }
    H5Gclose(g);
    return found;
}

static void loom_dims(hid_t d, int64_t* rows, int64_t* cols) {
    hid_t sp = H5Dget_space(d);
    hsize_t dims[2] = {0, 0};
    if (H5Sget_simple_extent_ndims(sp) == 2) H5Sget_simple_extent_dims(sp, dims, nullptr);
    H5Sclose(sp);
    *rows = (int64_t)dims[0]; *cols = (int64_t)dims[1];
}

static std::string join_names(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& x : v) { if (!out.empty()) out += ", "; out += x; }
    return out.empty() ? "(none)" : out;
}

static std::vector<OpenSpec> scan_loom(hid_t fid) {
    std::vector<OpenSpec> specs;
    std::string summary;
    auto add = [&](const std::string& k, const std::string& v) {
        summary += k; summary += '\t'; summary += v; summary += '\n';
    };
    add("format", "Loom");
    std::string ver = read_string_attr(fid, "LOOM_SPEC_VERSION");            // spec v2
    if (ver.empty() && link_exists(fid, "attrs") &&                           // spec v3:
        link_exists(fid, "attrs/LOOM_SPEC_VERSION")) {                         // a scalar dataset
        hid_t d = H5Dopen2(fid, "attrs/LOOM_SPEC_VERSION", H5P_DEFAULT);
        if (d >= 0) { ver = h5_value_to_string(d); H5Dclose(d); }
    }
    if (!ver.empty()) add("spec version", ver);
    int64_t ng = 0, nc = 0;
    { hid_t d = H5Dopen2(fid, "matrix", H5P_DEFAULT); loom_dims(d, &ng, &nc); H5Dclose(d); }
    add("matrix", std::to_string(nc) + " cells \xc3\x97 " + std::to_string(ng) +
                  " genes (stored genes \xc3\x97 cells)");
    std::vector<std::string> layers;
    if (link_exists(fid, "layers") && is_group(fid, "layers")) {
        hid_t g = H5Gopen2(fid, "layers", H5P_DEFAULT);
        layers = list_children(g);
        H5Gclose(g);
    }
    add("layers", join_names(layers));
    for (const char* grp : {"row_attrs", "col_attrs", "row_graphs", "col_graphs"}) {
        if (!link_exists(fid, grp) || !is_group(fid, grp)) continue;
        hid_t g = H5Gopen2(fid, grp, H5P_DEFAULT);
        add(grp, join_names(list_children(g)));
        H5Gclose(g);
    }
    specs.push_back({OpenSpec::Kind::Summary, "/", "summary", summary});
    specs.push_back({OpenSpec::Kind::LoomMatrix, "/matrix", "matrix (preview)", ""});
    specs.push_back({OpenSpec::Kind::LoomAttrs, "/col_attrs", "cells", ""});
    specs.push_back({OpenSpec::Kind::LoomAttrs, "/row_attrs", "genes", ""});
    for (const auto& l : layers)
        specs.push_back({OpenSpec::Kind::LoomMatrix, "/layers/" + l, "layers[" + l + "]", ""});
    return specs;
}

static std::string build_loom_table(hid_t file_id, const OpenSpec& spec, int64_t row_cap,
                                    std::shared_ptr<arrow::Table>* out,
                                    std::string* footer) {
    if (spec.kind == OpenSpec::Kind::LoomAttrs) {
        hid_t g = H5Gopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
        if (g < 0) return "Cannot open group " + spec.h5_path;
        int64_t full = 0;
        auto r = read_anndata_dataframe(g, row_cap, &full);
        H5Gclose(g);
        if (!r.ok()) return r.status().ToString();
        *out = *r;
        int64_t shown = (*out)->num_rows();
        h5_note_preview(shown, full, (*out)->num_columns(), (*out)->num_columns());
        *footer = "Format: Loom " + spec.display + " (" + spec.h5_path.substr(1) + ")";
        *footer += shown < full ? "  |  preview: first " + std::to_string(shown) + " of " +
                                  std::to_string(full) + " rows"
                                : "  |  Rows: " + std::to_string(shown);
        return "";
    }

    // LoomMatrix: read the corner [genes 0..200) × [cells 0..1000) and emit it
    // transposed — one column per gene, one row per cell.
    hid_t d = H5Dopen2(file_id, spec.h5_path.c_str(), H5P_DEFAULT);
    if (d < 0) return "Cannot open dataset " + spec.h5_path;
    int64_t G = 0, C = 0;
    loom_dims(d, &G, &C);
    const int64_t ng = std::min<int64_t>(G, kDense2DColCap);
    const int64_t nc = std::min<int64_t>(C, kDense2DRowCap);
    hid_t t = H5Dget_type(d);
    const bool integral = H5Tget_class(t) == H5T_INTEGER;
    H5Tclose(t);
    std::vector<double>  dbuf;
    std::vector<int64_t> ibuf;
    if (ng > 0 && nc > 0) {
        hid_t fs = H5Dget_space(d);
        hsize_t start[2] = {0, 0}, count[2] = {(hsize_t)ng, (hsize_t)nc};
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, start, nullptr, count, nullptr);
        hid_t ms = H5Screate_simple(2, count, nullptr);
        herr_t st;
        if (integral) { ibuf.resize((size_t)(ng * nc));
                        st = h5_read(d, H5T_NATIVE_INT64, ms, fs, ibuf.data()); }
        else          { dbuf.resize((size_t)(ng * nc));
                        st = h5_read(d, H5T_NATIVE_DOUBLE, ms, fs, dbuf.data()); }
        H5Sclose(ms); H5Sclose(fs);
        if (st < 0) { std::string e = h5_read_failure(d); H5Dclose(d); return e; }
    }
    H5Dclose(d);

    // Labels.
    std::string cell_attr = loom_label_attr(file_id, "col_attrs", kLoomCellIds);
    std::string gene_attr = loom_label_attr(file_id, "row_attrs", kLoomGeneIds);
    std::vector<std::string> cells = cell_attr.empty() ? std::vector<std::string>{}
        : h5_strings(file_id, "col_attrs/" + cell_attr, nc);
    std::vector<std::string> genes = gene_attr.empty() ? std::vector<std::string>{}
        : h5_strings(file_id, "row_attrs/" + gene_attr, ng);
    // A repeated gene label gets a second ID attribute (or its index) appended.
    std::vector<std::string> alt;
    for (const char* n : {"Accession", "gene_ids", "var_names"})
        if (gene_attr != n && link_exists(file_id, ("row_attrs/" + std::string(n)).c_str())) {
            alt = h5_strings(file_id, "row_attrs/" + std::string(n), ng);
            break;
        }
    std::map<std::string, int> seen;
    for (const auto& gname : genes) ++seen[gname];

    arrow::FieldVector fields;
    std::vector<std::shared_ptr<arrow::Array>> cols;
    arrow::StringBuilder cb;
    for (int64_t c = 0; c < nc; ++c) {
        if (c < (int64_t)cells.size()) (void)cb.Append(cells[(size_t)c]); else (void)cb.AppendNull();
    }
    std::shared_ptr<arrow::Array> ca;
    (void)cb.Finish(&ca);
    fields.push_back(arrow::field(cell_attr.empty() ? "cell" : cell_attr, arrow::utf8()));
    cols.push_back(ca);
    for (int64_t gi = 0; gi < ng; ++gi) {
        std::string name = gi < (int64_t)genes.size() ? genes[(size_t)gi] : "";
        if (name.empty()) name = "gene" + std::to_string(gi);
        else if (seen[name] > 1)
            name += gi < (int64_t)alt.size() ? " (" + alt[(size_t)gi] + ")"
                                             : " #" + std::to_string(gi);
        std::shared_ptr<arrow::Array> a;
        if (integral) {
            arrow::Int64Builder b;
            (void)b.AppendValues(ibuf.data() + gi * nc, nc);
            (void)b.Finish(&a);
            fields.push_back(arrow::field(name, arrow::int64()));
        } else {
            arrow::DoubleBuilder b;
            (void)b.AppendValues(dbuf.data() + gi * nc, nc);
            (void)b.Finish(&a);
            fields.push_back(arrow::field(name, arrow::float64()));
        }
        cols.push_back(a);
    }
    *out = arrow::Table::Make(arrow::schema(fields), cols, nc);
    h5_note_preview(nc, C, ng, G);   // shown cells × genes (stored genes × cells)
    *footer = "Format: Loom " + spec.display + "  |  preview: first " + std::to_string(nc) +
              " of " + std::to_string(C) + " cells, first " + std::to_string(ng) + " of " +
              std::to_string(G) + " genes  |  shown cells \xc3\x97 genes (stored genes \xc3\x97 cells)";
    return "";
}

// The value type a dense dataset streams as, as its preview shows it: int64
// for integer data, double for floating point; false for anything else
// (text, compound), which is not streamed.
static bool h5_dense_value_type(hid_t d, bool* ints) {
    hid_t t = H5Dget_type(d);
    const H5T_class_t cls = H5Tget_class(t);
    H5Tclose(t);
    *ints = cls == H5T_INTEGER;
    return cls == H5T_INTEGER || cls == H5T_FLOAT;
}

// --matrix long for a matrix tab: the whole matrix as entry rows; nullptr for
// a matrix that cannot be read that way (text data).
static std::unique_ptr<TabularSource> make_h5_long_matrix(const H5FilePtr& file,
                                                         const std::string& path,
                                                         const OpenSpec& spec) {
    auto wide = make_h5_matrix_stream(file, path, spec);
    return wide ? H5LongMatrixSource::make(std::move(wide)) : nullptr;
}

// A whole-matrix stream for a capped matrix tab, labelled as its preview is;
// nullptr for a matrix that is not streamed (text data).
static std::unique_ptr<TabularSource> make_h5_matrix_stream(const H5FilePtr& file,
                                                           const std::string& path,
                                                           const OpenSpec& spec) {
    using Plan = H5MatrixStreamSource::Plan;
    using Layout = H5MatrixStreamSource::Layout;
    const hid_t fid = *file;
    Plan p;
    p.h5_path = spec.h5_path;
    p.label = spec.display;
    switch (spec.kind) {
    case OpenSpec::Kind::Sparse: {                 // AnnData CSR group
        hid_t g = H5Gopen2(fid, spec.h5_path.c_str(), H5P_DEFAULT);
        if (g < 0) return nullptr;
        const std::string enc = read_string_attr(g, "encoding-type");
        const bool csr = enc == "csr_matrix";
        if (!csr && enc != "csc_matrix") { H5Gclose(g); return nullptr; }
        int64_t shape[2] = {0, 0};
        read_shape2(g, "shape", shape);
        if (hid_t ip = H5Dopen2(g, "indptr", H5P_DEFAULT); ip >= 0) {
            int64_t& major = csr ? shape[0] : shape[1];   // the axis indptr spans
            major = std::min<int64_t>(major, std::max<int64_t>(0, h5_len_1d(ip) - 1));
            H5Dclose(ip);
        }
        H5Gclose(g);
        // CSC: transposed to CSR in memory when the export first reads.
        p.layout = csr ? Layout::Csr : Layout::Csc; p.rows = shape[0]; p.cols = shape[1];
        p.format = "AnnData";                      // sparse previews are double
        break;
    }
    case OpenSpec::Kind::Matrix2D:
    case OpenSpec::Kind::Dataset2D: {
        hid_t d = H5Dopen2(fid, spec.h5_path.c_str(), H5P_DEFAULT);
        if (d < 0) return nullptr;
        int64_t r = 0, c = 0;
        loom_dims(d, &r, &c);
        const bool numeric = h5_dense_value_type(d, &p.ints);
        H5Dclose(d);
        if (!numeric) return nullptr;
        p.rows = r; p.cols = c;
        p.format = spec.kind == OpenSpec::Kind::Matrix2D ? "AnnData" : "HDF5";
        break;
    }
    case OpenSpec::Kind::TenxMatrix: {             // CSC by barcode = CSR by shown cell
        hid_t g = H5Gopen2(fid, spec.h5_path.c_str(), H5P_DEFAULT);
        if (g < 0) return nullptr;
        int64_t nf = 0, nb = 0;
        const bool ok = tenx_shape(g, &nf, &nb);
        if (ok) {
            if (hid_t ip = H5Dopen2(g, "indptr", H5P_DEFAULT); ip >= 0) {
                nb = std::min<int64_t>(nb, std::max<int64_t>(0, h5_len_1d(ip) - 1));
                H5Dclose(ip);
            }
            // Feature names; a name that repeats gets its id appended, so
            // every header is unique (the preview's rule, over all features).
            const bool v3 = spec.key == "v3";
            auto ids = h5_strings(g, v3 ? "features/id" : "genes", nf);
            auto names = h5_strings(g, v3 ? "features/name" : "gene_names", nf);
            std::map<std::string, int> seen;
            for (const auto& n : names) ++seen[n];
            for (int64_t c = 0; c < nf; ++c) {
                std::string n = c < (int64_t)names.size() ? names[(size_t)c] : "";
                std::string id = c < (int64_t)ids.size() ? ids[(size_t)c] : "";
                if (n.empty()) n = id.empty() ? "col" + std::to_string(c) : id;
                else if (seen[n] > 1 && !id.empty()) n += " (" + id + ")";
                p.col_names.push_back(std::move(n));
            }
            p.row_labels = h5_strings(g, "barcodes", nb);
        }
        H5Gclose(g);
        if (!ok) return nullptr;
        p.layout = Layout::Csr; p.rows = nb; p.cols = nf;
        p.row_header = "barcode";
        p.long_row = "barcode"; p.long_col = "feature";
        p.format = "10x Genomics";
        return H5MatrixStreamSource::from_plan(file, path, std::move(p));
    }
    case OpenSpec::Kind::LoomMatrix: {             // stored genes × cells
        hid_t d = H5Dopen2(fid, spec.h5_path.c_str(), H5P_DEFAULT);
        if (d < 0) return nullptr;
        int64_t G = 0, C = 0;
        loom_dims(d, &G, &C);
        const bool numeric = h5_dense_value_type(d, &p.ints);
        H5Dclose(d);
        if (!numeric) return nullptr;
        p.layout = Layout::DenseT; p.rows = C; p.cols = G;
        const std::string cell_attr = loom_label_attr(fid, "col_attrs", kLoomCellIds);
        const std::string gene_attr = loom_label_attr(fid, "row_attrs", kLoomGeneIds);
        if (!cell_attr.empty()) p.row_labels = h5_strings(fid, "col_attrs/" + cell_attr, C);
        p.row_header = cell_attr.empty() ? "cell" : cell_attr;
        p.long_row = p.row_header;
        p.long_col = gene_attr.empty() ? "gene" : gene_attr;
        std::vector<std::string> genes = gene_attr.empty() ? std::vector<std::string>{}
            : h5_strings(fid, "row_attrs/" + gene_attr, G);
        std::vector<std::string> alt;
        for (const char* n : {"Accession", "gene_ids", "var_names"})
            if (gene_attr != n && link_exists(fid, ("row_attrs/" + std::string(n)).c_str())) {
                alt = h5_strings(fid, "row_attrs/" + std::string(n), G);
                break;
            }
        std::map<std::string, int> seen;
        for (const auto& gname : genes) ++seen[gname];
        for (int64_t gi = 0; gi < G; ++gi) {
            std::string name = gi < (int64_t)genes.size() ? genes[(size_t)gi] : "";
            if (name.empty()) name = "gene" + std::to_string(gi);
            else if (seen[name] > 1)
                name += gi < (int64_t)alt.size() ? " (" + alt[(size_t)gi] + ")"
                                                 : " #" + std::to_string(gi);
            p.col_names.push_back(std::move(name));
        }
        p.format = "Loom";
        return H5MatrixStreamSource::from_plan(file, path, std::move(p));
    }
    default:
        return nullptr;
    }
    // AnnData labels by the matrix's axes (a generic 2-D dataset has none:
    // col0, col1, ...).
    // The long form names the axes obs / var (or the indexes' own names);
    // an embedding's columns are dimensions.
    if (spec.kind != OpenSpec::Kind::Dataset2D) {
        if (spec.axes == AnnMatrixAxes::ObsByVar || spec.axes == AnnMatrixAxes::ObsByRawVar) {
            std::string idx;
            read_anndata_index_labels(fid, anndata_root(spec.h5_path) +
                                          (spec.axes == AnnMatrixAxes::ObsByRawVar ? "/raw/var" : "/var"),
                                      p.cols, &p.col_names, &idx);
            p.long_col = idx.empty() || idx == "_index" ? "var" : idx;
        } else {
            if (!spec.key.empty())
                for (int64_t c = 0; c < p.cols; ++c) p.col_names.push_back(spec.key + std::to_string(c + 1));
            p.long_col = "dim";
        }
        const bool by_var = spec.axes == AnnMatrixAxes::VarByDim;
        std::string idx;
        read_anndata_index_labels(fid, anndata_root(spec.h5_path) + (by_var ? "/var" : "/obs"),
                                  p.rows, &p.row_labels, &idx);
        const std::string rname = idx.empty() || idx == "_index" ? (by_var ? "var" : "obs") : idx;
        if (!p.row_labels.empty()) p.row_header = rname;
        p.long_row = rname;
    }
    return H5MatrixStreamSource::from_plan(file, path, std::move(p));
}

// ── Hdf5Source::open_first ──────────────────────────────────────────────────

std::string Hdf5Source::open_first(const std::string& path,
                                      std::unique_ptr<Hdf5Source>* out,
                                      int64_t df_row_cap, bool matrix_long) {
    // Silence HDF5's stderr error spew for missing attrs etc.
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    register_hdf5_filters();
    hid_t fid = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fid < 0) return "Cannot open '" + path + "' as HDF5";
    H5FilePtr file(new hid_t(fid), [](hid_t* p){
        if (*p >= 0) H5Fclose(*p); delete p;
    });

    // AnnData detection: root attribute encoding-type == "anndata"
    // OR the modern heuristic /obs + /var + X presence.
    std::string root_enc = read_string_attr(fid, "encoding-type");
    bool is_anndata = (root_enc == "anndata") ||
        (link_exists(fid, "obs") && link_exists(fid, "var") &&
         link_exists(fid, "X"));

    // Refuse the legacy (pre-anndata-0.7) layout up-front. Signature:
    // root has no encoding-type, and obs/var are compound *datasets*
    // instead of groups-of-columns. Decoding compound types + the
    // legacy h5sparse X format is more work than it's worth without
    // a real demand signal, and silently rendering an empty summary
    // is worse than refusing.
    if (is_anndata && root_enc != "anndata" &&
        link_exists(fid, "obs") && !is_group(fid, "obs")) {
        return "'" + path + "': legacy AnnData layout (pre-0.7) is not "
               "supported. Re-save with a recent anndata: "
               "`python -c \"import anndata; "
               "anndata.read_h5ad('" + path + "')"
               ".write_h5ad('out.h5ad')\"`";
    }

    // MuData (.h5mu): one AnnData per modality under /mod/<name>, plus the
    // joint obs / var at the root. Tabs: a summary, the joint obs / var, then
    // each modality's AnnData tabs prefixed "<name>:".
    const bool is_mudata = read_string_attr(fid, "encoding-type") == "MuData" ||
                           (link_exists(fid, "mod") && is_group(fid, "mod"));
    if (is_mudata) {
        std::vector<OpenSpec> specs;
        std::string summary = "format\tMuData\n";
        hid_t mg = H5Gopen2(fid, "mod", H5P_DEFAULT);
        const std::vector<std::string> mods = mg >= 0 ? list_children(mg) : std::vector<std::string>{};
        if (mg >= 0) H5Gclose(mg);
        summary += "modalities\t";
        for (size_t i = 0; i < mods.size(); ++i) summary += (i ? ", " : "") + mods[i];
        summary += "\n";
        for (const char* df : {"obs", "var"})
            if (link_exists(fid, df) && is_group(fid, df)) {
                hid_t g = H5Gopen2(fid, df, H5P_DEFAULT);
                summary += std::string(df) + "\t" + std::to_string(anndata_column_count(g)) +
                           " columns (joint, all modalities)\n";
                H5Gclose(g);
                specs.push_back({OpenSpec::Kind::DataFrame, std::string("/") + df, df, "joint"});
            }
        for (const std::string& m : mods) {
            for (OpenSpec sp : scan_anndata(fid, "/mod/" + m)) {
                if (sp.kind == OpenSpec::Kind::Summary) {
                    // the modality's X line, for the MuData summary
                    for (size_t at = 0; (at = sp.footer_hint.find("X\t", at)) != std::string::npos; ++at)
                        if (at == 0 || sp.footer_hint[at - 1] == '\n') {
                            const size_t e = sp.footer_hint.find('\n', at);
                            summary += m + ":X\t" + sp.footer_hint.substr(at + 2, e - at - 2) + "\n";
                            break;
                        }
                }
                sp.display = m + ":" + sp.display;
                specs.push_back(std::move(sp));
            }
        }
        if (hsize_t fsz = 0; H5Fget_filesize(fid, &fsz) >= 0)
            summary += "file size\t" + h5_size_label((double)fsz) + "\n";
        specs.insert(specs.begin(), OpenSpec{OpenSpec::Kind::Summary, "/", "summary", summary});
        auto all = std::make_shared<std::vector<OpenSpec>>(specs);
        OpenSpec first = specs.front();
        std::vector<OpenSpec> siblings(specs.begin() + 1, specs.end());
        return build_one(path, std::move(file), std::move(first),
                         std::move(all), std::move(siblings), out, df_row_cap);
    }

    bool tenx_v3 = false;
    std::vector<std::string> tenx_groups;
    if (!is_anndata) tenx_groups = tenx_matrix_groups(fid, &tenx_v3);
    std::vector<OpenSpec> specs = is_anndata ? scan_anndata(fid)
                                : !tenx_groups.empty() ? scan_10x(fid, tenx_groups, tenx_v3)
                                : is_loom(fid) ? scan_loom(fid)
                                : scan_generic(fid);
    if (specs.empty()) return "'" + path + "': no viewable HDF5 datasets";
    // --matrix long: matrix tabs become entry rows of the whole matrix, so
    // their label says "(long)" rather than "(preview)".
    if (matrix_long)
        for (auto& sp : specs) {
            using K = OpenSpec::Kind;
            if (sp.kind != K::Matrix2D && sp.kind != K::Dataset2D && sp.kind != K::Sparse &&
                sp.kind != K::TenxMatrix && sp.kind != K::LoomMatrix) continue;
            sp.long_form = true;
            if (auto at = sp.display.find(" (preview)"); at != std::string::npos) sp.display.erase(at);
            sp.display += " (long)";
        }

    auto all = std::make_shared<std::vector<OpenSpec>>(specs);
    OpenSpec first = specs.front();
    std::vector<OpenSpec> siblings(specs.begin() + 1, specs.end());
    return build_one(path, std::move(file), std::move(first),
                       std::move(all), std::move(siblings), out, df_row_cap);
}

}  // namespace h5v
