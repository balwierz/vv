// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// A hierarchical array store — the storage under the AnnData / MuData / Loom /
// 10x / generic-hierarchy readers in hdf5.cpp. Implemented by Hdf5Store
// (libhdf5, hdf5.cpp) and ZarrStore (zarr.cpp). Paths are absolute, "/" is the
// root, "/obs/cell_type/codes" a nested array. Shared by src/formats only.

#pragma once

#include "internal.hpp"

namespace h5v {

enum class NodeKind { Missing, Group, Array, Other };
// The value class of an array: Int / Float numbers, String text, Enum
// (HDF5 enumerations, read as their member names; h5py's {FALSE, TRUE} as
// booleans), Bool (Zarr's bool), Other (compound, opaque, ...).
enum class VClass { Int, Float, String, Enum, Bool, Other };

struct ArrayInfo {
    std::vector<int64_t> shape;     // rank 0 = a scalar
    VClass      cls = VClass::Other;
    std::string dtype;              // display label: int64, float32, string, string[12], enum ...
    size_t      itemsize = 0;
};

// How an array is laid out in the store (the summary's storage rows).
struct StorageInfo {
    std::string dtype;
    int64_t     n = 0;              // elements
    size_t      itemsize = 0;
    bool        chunked = false;
    std::vector<int64_t> chunk;     // chunk shape when chunked
    int64_t     chunk_elems = 0;
    std::string filters;            // "gzip 4 + shuffle", "blosc lz4 5 + shuffle", "no compression"
    uint64_t    stored = 0;         // bytes in the store
};

// One row of the generic hierarchy tab.
struct HierarchyRow {
    std::string path;
    std::string kind;               // "Group" | "Dataset" | "Other"
    std::string shape;              // empty for groups
    std::string dtype;              // empty for groups
    int         n_attrs = 0;
    std::vector<int64_t> dims;      // datasets
};

class Store {
public:
    virtual ~Store() = default;
    virtual NodeKind kind(const std::string& path) const = 0;
    // Child names of a group, in name order (HDF5's name index).
    virtual std::vector<std::string> children(const std::string& group) const = 0;
    virtual bool has_attr(const std::string& path, const char* name) const = 0;
    // A scalar string attribute; "" when absent or not a string.
    virtual std::string attr_string(const std::string& path, const char* name) const = 0;
    // An integer list attribute (e.g. shape), at most 1024 values; empty when absent.
    virtual std::vector<int64_t> attr_ints(const std::string& path, const char* name) const = 0;
    virtual bool attr_bool(const std::string& path, const char* name) const = 0;
    virtual std::optional<ArrayInfo> info(const std::string& path) const = 0;
    // Elements [off, off + len) of a 1-D array (a scalar: its one value) as
    // an int64 / float64 / utf8 / boolean column (enums as member names).
    virtual arrow::Result<std::shared_ptr<arrow::Array>> read_column(const std::string& path,
                                                                     int64_t off, int64_t len) const = 0;
    virtual arrow::Status read_i64(const std::string& path, int64_t off, int64_t len, int64_t* out) const = 0;
    virtual arrow::Status read_f64(const std::string& path, int64_t off, int64_t len, double* out) const = 0;
    // A [r0, r0 + nr) × [c0, c0 + nc) block of a 2-D array, row-major.
    virtual arrow::Status read_block_i64(const std::string& path, int64_t r0, int64_t nr, int64_t c0,
                                         int64_t nc, int64_t* out) const = 0;
    virtual arrow::Status read_block_f64(const std::string& path, int64_t r0, int64_t nr, int64_t c0,
                                         int64_t nc, double* out) const = 0;
    virtual std::optional<StorageInfo> storage(const std::string& path) const = 0;
    // Every node below the root, in visit order (the generic hierarchy tab).
    virtual std::vector<HierarchyRow> hierarchy() const = 0;
    virtual int64_t total_bytes() const = 0;           // -1 when unknown
    virtual std::string format_name() const = 0;       // "HDF5", "Zarr v2", ...
    // Why `path` cannot be read (a missing codec, ...), without the path.
    virtual std::string read_why(const std::string& path) const = 0;
};

using StorePtr = std::shared_ptr<const Store>;

// The tabs of an AnnData / MuData / 10x / Loom / generic store, as for HDF5.
std::string open_store_source(StorePtr store, const std::string& path,
                              std::unique_ptr<TabularSource>* out,
                              int64_t df_row_cap = kDataFrameRowCap, bool matrix_long = false);

}  // namespace h5v
