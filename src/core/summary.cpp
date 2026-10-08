// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// summarize_columns: the per-column statistics behind --describe,
// --value-counts and vvg's Column tab, in one pass over a source.

#include "internal.hpp"

#include <cmath>
#include <limits>
#include <unordered_map>

namespace {

struct Acc {
    bool        numeric = false, track = false;
    int64_t     count = 0, nulls = 0;
    double      d_min = std::numeric_limits<double>::infinity();
    double      d_max = -std::numeric_limits<double>::infinity();
    long double sum = 0.0L;
    std::string s_min, s_max;
    // Standard deviation by Welford's update (stable where a sum of squares
    // cancels).
    int64_t     w_n = 0;
    long double w_mean = 0.0L, w_m2 = 0.0L;
    // Percentiles: every non-NaN value up to the cap, then a uniform reservoir
    // sample of that size (Algorithm R, fixed seed so output is reproducible).
    std::vector<double> sample;
    int64_t     sample_seen = 0;
    uint64_t    rng = 0x9e3779b97f4a7c15ULL;
    // Distinct values with their counts, until more than distinct_cap.
    std::unordered_map<std::string, int64_t> counts;
    bool        overflow = false;
};

uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

}  // namespace

std::string summarize_columns(TabularSource& src, const SummaryOptions& opt,
                              std::vector<ColumnSummary>* out) {
    out->clear();
    auto schema = src.schema();
    const size_t n = opt.cols.size();
    for (int c : opt.cols)
        if (c < 0 || c >= schema->num_fields()) return "column index out of range";
    // Value keys are identities: never merge two floats by rounding them.
    std::optional<ExactFloats> exact;
    if (opt.value_counts) exact.emplace();

    std::vector<Acc> acc(n);
    out->resize(n);
    int64_t n_numeric = 0;
    for (size_t k = 0; k < n; ++k) {
        auto f = schema->field(opt.cols[k]);
        ColumnSummary& cs = (*out)[k];
        cs.name     = f->name();
        cs.type     = type_label(*f->type());
        cs.numeric  = is_numeric_type(f->type()->id());
        cs.temporal = cs.numeric && is_date_or_timestamp(*f->type());
        acc[k].numeric = cs.numeric;
        acc[k].track   = opt.value_counts || !cs.numeric;
        n_numeric += cs.numeric;
    }

    // Values kept for the percentiles: 16 Mi doubles (128 MiB) shared by the
    // numeric columns, at least 1024 each so a wide matrix still gets an
    // estimate.
    const bool want_pct = !opt.percentiles.empty();
    int64_t sample_cap = std::max<int64_t>(1024, (int64_t{1} << 24) /
                                                 std::max<int64_t>(1, n_numeric));
    // Test hook: a small cap exercises the sampled path on a small file.
    if (const char* e = std::getenv("VV_DESCRIBE_SAMPLE_CAP"); e && std::atoll(e) > 0)
        sample_cap = std::atoll(e);

    const bool have_filter = opt.filter && !opt.filter->groups.empty();
    const std::vector<int> read_set = have_filter
        ? union_with_filter(opt.cols, *opt.filter) : opt.cols;
    std::vector<int> pos(n, -1);   // each column's place in read_set
    for (size_t k = 0; k < n; ++k)
        for (size_t j = 0; j < read_set.size(); ++j)
            if (read_set[j] == opt.cols[k]) { pos[k] = (int)j; break; }

    int64_t rows_left = opt.max_rows < 0 ? INT64_MAX : opt.max_rows;
    for (int c = 0; rows_left > 0; ++c) {
        if (opt.cancel && opt.cancel->load()) return kExportCanceled;
        src.ensure(c);
        if (c >= src.num_chunks()) break;
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, read_set, &tbl).ok()) continue;
        if (opt.rows_done && tbl) *opt.rows_done += tbl->num_rows();
        if (have_filter) tbl = apply_filter(tbl, *opt.filter, read_set);
        if (!tbl || tbl->num_rows() == 0) continue;
        const int64_t take = std::min(tbl->num_rows(), rows_left);
        if (take < tbl->num_rows()) tbl = tbl->Slice(0, take);

        for (size_t k = 0; k < n; ++k) {
            Acc& a = acc[k];
            for (auto& ch : tbl->column(pos[k])->chunks()) {
                const int64_t len = ch->length();
                for (int64_t r = 0; r < len; ++r) {
                    if (ch->IsNull(r)) { a.nulls++; continue; }
                    a.count++;
                    std::string s;
                    if (a.numeric) {
                        double d;
                        if (array_value_as_double(*ch, r, &d)) {
                            if (d < a.d_min) a.d_min = d;
                            if (d > a.d_max) a.d_max = d;
                            a.sum += d;
                            ++a.w_n;
                            const long double delta = d - a.w_mean;
                            a.w_mean += delta / a.w_n;
                            a.w_m2   += delta * (d - a.w_mean);
                            if (want_pct && !std::isnan(d)) {
                                if ((int64_t)a.sample.size() < sample_cap) {
                                    a.sample.push_back(d);
                                } else {
                                    uint64_t j = splitmix(a.rng) % (uint64_t)(a.sample_seen + 1);
                                    if (j < (uint64_t)sample_cap) a.sample[j] = d;
                                }
                                ++a.sample_seen;
                            }
                        }
                        if (!a.track || a.overflow) continue;
                        s = cell_to_string(*ch, r);
                    } else {
                        s = cell_to_string(*ch, r);
                        if (a.count == 1 || s < a.s_min) a.s_min = s;
                        if (a.count == 1 || s > a.s_max) a.s_max = s;
                        if (a.overflow) continue;
                    }
                    auto it = a.counts.find(s);
                    if (it != a.counts.end()) { ++it->second; continue; }
                    if (a.counts.size() >= opt.distinct_cap) {
                        a.overflow = true;
                        std::unordered_map<std::string, int64_t>().swap(a.counts);
                        continue;
                    }
                    a.counts.emplace(std::move(s), 1);
                }
            }
        }
        rows_left -= take;
    }
    // A stream that failed part-way (a malformed record, a truncated file)
    // must not be summarised as if the rows read so far were the whole table.
    if (!src.read_status().ok())
        return shorten_reader_error(src.read_status().ToString());

    for (size_t k = 0; k < n; ++k) {
        Acc& a = acc[k];
        ColumnSummary& cs = (*out)[k];
        cs.count = a.count;
        cs.nulls = a.nulls;
        cs.s_min = std::move(a.s_min);
        cs.s_max = std::move(a.s_max);
        if (cs.numeric) {
            cs.min  = a.d_min;
            cs.max  = a.d_max;
            cs.sum  = (double)a.sum;
            cs.mean = a.count ? (double)(a.sum / (long double)a.count) : 0;
            // n - 1 in the denominator, as R's sd() and pandas' std().
            if (a.w_n >= 2) {
                cs.std = (double)std::sqrt(a.w_m2 / (long double)(a.w_n - 1));
                cs.has_std = true;
            }
        }
        // Percentiles by linear interpolation between the closest ranks (R's
        // type 7, NumPy's and pandas' default).
        if (!a.sample.empty()) {
            auto& v = a.sample;
            std::sort(v.begin(), v.end());
            for (double p : opt.percentiles) {
                const double h = (double)(v.size() - 1) * p / 100.0;
                const size_t lo = (size_t)std::floor(h);
                const size_t hi = std::min(lo + 1, v.size() - 1);
                cs.percentiles.push_back(v[lo] + (h - (double)lo) * (v[hi] - v[lo]));
            }
            cs.percentiles_sampled = a.sample_seen > (int64_t)v.size();
            std::vector<double>().swap(v);
        }
        if (!a.track || a.overflow) continue;
        cs.distinct = (int64_t)a.counts.size();
        if (!opt.value_counts) continue;
        cs.values.reserve(a.counts.size() + 1);
        for (auto& [v, c] : a.counts) cs.values.push_back({v, c, false});
        std::unordered_map<std::string, int64_t>().swap(a.counts);
        if (a.nulls) cs.values.push_back({"", a.nulls, true});
        auto as_num = [](const std::string& v) {
            char* end = nullptr;
            double d = std::strtod(v.c_str(), &end);
            return (end && !*end && !v.empty()) ? d : std::numeric_limits<double>::quiet_NaN();
        };
        const bool numeric = cs.numeric;
        std::sort(cs.values.begin(), cs.values.end(),
                  [&](const ValueCount& x, const ValueCount& y) {
            if (x.count != y.count) return x.count > y.count;
            if (x.null != y.null) return y.null;            // null last among equals
            if (numeric) {
                double p = as_num(x.value), q = as_num(y.value);
                if (p < q) return true;
                if (q < p) return false;
            }
            return x.value < y.value;
        });
    }
    return "";
}
