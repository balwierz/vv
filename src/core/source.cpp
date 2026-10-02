// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// TabularSource helpers shared by the readers (retention, column selection).

#include "internal.hpp"

// ── Tabular source abstraction ────────────────────────────────────────────────
//
// Common interface for Parquet, CSV, TSV, BED, etc.
// "Chunks" map to row groups (Parquet) or read batches (delimited).

// (ChunkMeta + TabularSource are defined in vv/vvcore.hpp, included near the
// top. Concrete sources below subclass TabularSource.)

// Field indices to display in human-facing views, with `hidden_for_display`
// names filtered out and `max_cols` honoured. Used by print_table, the
// vertical-head view, and the TUI. Delimited / Parquet output paths keep
// all columns and bypass this helper.
static std::vector<int> visible_field_indices(const TabularSource& src,
                                              int max_cols) {
    auto schema = src.schema();
    auto hidden = src.hidden_for_display();
    std::set<std::string> hide(hidden.begin(), hidden.end());
    std::vector<int> out;
    int n = schema->num_fields();
    int limit = (max_cols > 0) ? max_cols : n;
    for (int i = 0; i < n && (int)out.size() < (size_t)limit; ++i) {
        if (hide.count(schema->field(i)->name())) continue;
        out.push_back(i);
    }
    return out;
}

// Resolve `cfg.select_cols` into source field indices.
//
// Empty spec: the visible-only set (human-facing views, default) or all
// fields (`include_hidden=true`, for export paths — `--tsv` / `--csv` /
// `--json` / `--parquet` / `--arrow`).
//
// Otherwise the spec is a comma-separated list of terms, resolved in order.
// Output order follows the spec, so `--select End,Start` also reorders:
//
//   Chr           an exact field name
//   chr*, ?_pct   a glob (fnmatch(3), case-sensitive) — `*` and `?` only
//   2-4, 5-       a 1-based, inclusive index range (`N-` runs to the end)
//   @numeric      a type class: @numeric, @string, @list, @bool, @temporal
//   !TERM         exclusion — remove everything TERM matches
//
// An exact field name ALWAYS wins over pattern interpretation, so a column
// literally called `3-9` or `chr*` stays addressable by name.
//
// Unresolvable terms are appended to `*unknown_out`: a plain name that
// doesn't exist, or a pattern that matched nothing (the latter prefixed so
// the caller can word it differently — a silently-empty `!pct_*` typo would
// otherwise be data loss in `--parquet`).
static constexpr const char kNoMatchPrefix[] = "\x01";   // internal marker
// Reported when a non-empty spec resolved cleanly but left nothing selected
// (e.g. `--select 'Chr,!C*'`). Every output path would otherwise write an
// empty, zero-column result and exit 0 — `--parquet` even announces "[20 rows
// → out.parquet]" over a 0x0 file. Pre-existing for `--select ','`; the
// pattern syntax makes it easy to reach by accident, so it is an error now.
static constexpr const char kEmptyResultMarker[] = "\x02";

static bool select_type_class_matches(const std::string& cls,
                                      const arrow::DataType& t) {
    if (cls == "numeric")  return is_numeric_type(t.id());
    if (cls == "string")   return t.id() == arrow::Type::STRING ||
                                  t.id() == arrow::Type::LARGE_STRING;
    if (cls == "bool")     return t.id() == arrow::Type::BOOL;
    if (cls == "list")     return t.id() == arrow::Type::LIST ||
                                  t.id() == arrow::Type::LARGE_LIST ||
                                  t.id() == arrow::Type::FIXED_SIZE_LIST ||
                                  t.id() == arrow::Type::MAP;
    if (cls == "temporal") return t.id() == arrow::Type::DATE32 ||
                                  t.id() == arrow::Type::DATE64 ||
                                  t.id() == arrow::Type::TIME32 ||
                                  t.id() == arrow::Type::TIME64 ||
                                  t.id() == arrow::Type::TIMESTAMP ||
                                  t.id() == arrow::Type::DURATION;
    return false;
}

// Does this token merely LOOK like an index range ("digits-digits" or
// "digits-")? Deliberately unbounded, unlike select_parse_range: a header
// called "0-10" or "500-1000" is range-shaped even when those numbers are
// nowhere near the column count.
static bool select_looks_like_range(const std::string& t) {
    auto dash = t.find('-');
    if (dash == std::string::npos || dash == 0) return false;
    const std::string a = t.substr(0, dash), b = t.substr(dash + 1);
    auto all_digits = [](const std::string& s) {
        return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
    };
    return all_digits(a) && (b.empty() || all_digits(b));
}

// Parse "N-M" / "N-" as a 1-based inclusive range. Returns false unless the
// whole token is consumed, so "2-4x" and "-3" are not ranges.
static bool select_parse_range(const std::string& t, int n_fields,
                               int* lo, int* hi) {
    auto dash = t.find('-');
    if (dash == std::string::npos || dash == 0) return false;
    const std::string a = t.substr(0, dash), b = t.substr(dash + 1);
    auto all_digits = [](const std::string& s) {
        return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
    };
    if (!all_digits(a)) return false;
    if (!b.empty() && !all_digits(b)) return false;
    long la = std::strtol(a.c_str(), nullptr, 10);
    long lb = b.empty() ? n_fields : std::strtol(b.c_str(), nullptr, 10);
    if (la < 1 || lb < la) return false;
    *lo = (int)std::min<long>(la, n_fields);
    *hi = (int)std::min<long>(lb, n_fields);
    return la <= n_fields;
}

std::vector<int> select_field_indices(
 const TabularSource& src, const Config& cfg,
 std::vector<std::string>* unknown_out,
 bool include_hidden) {
    auto schema = src.schema();
    const int n_fields = schema->num_fields();

    // The set a bare `!exclusion` starts from. This MUST mirror the empty-spec
    // branch: all fields for export paths, the visible set otherwise.
    // Seeding unconditionally from the visible set would silently drop hidden
    // columns (e.g. LociSSD's derived MaxEndSoFar) from --parquet output —
    // the worst failure mode for a converter.
    auto seed_all = [&]() {
        if (include_hidden) {
            std::vector<int> v;
            v.reserve((size_t)n_fields);
            for (int i = 0; i < n_fields; ++i) v.push_back(i);
            return v;
        }
        return visible_field_indices(src, 0);   // no -c clamp; applied below
    };

    if (cfg.select_cols.empty()) {
        if (include_hidden) {
            int limit = (cfg.max_cols > 0) ? std::min(cfg.max_cols, n_fields)
                                           : n_fields;
            std::vector<int> out;
            for (int i = 0; i < limit; ++i) out.push_back(i);
            return out;
        }
        return visible_field_indices(src, cfg.max_cols);
    }

    // Exact-name lookup that also handles DUPLICATE field names: Arrow's
    // GetFieldIndex returns -1 when a name is ambiguous, which would drop the
    // term through to pattern interpretation and silently select something
    // else entirely. Scan for every field with this name instead.
    auto exact_matches = [&](const std::string& name) {
        std::vector<int> hits;
        for (int i = 0; i < n_fields; ++i)
            if (schema->field(i)->name() == name) hits.push_back(i);
        return hits;
    };

    // Does this file's own header namespace look like index ranges? Binned
    // matrices (Hi-C bins, age/distance bins) really do have columns named
    // "0-10", "10-20", ... On such a file a bare `N-M` term must NOT be
    // reinterpreted positionally: a typo'd bin name would then silently
    // select three unrelated columns instead of erroring. The file's own
    // names win over vv's syntax.
    const bool schema_has_range_names = [&] {
        for (int i = 0; i < n_fields; ++i)
            if (select_looks_like_range(schema->field(i)->name())) return true;
        return false;
    }();

    // Resolve one term to the field indices it names. `matched` is false when
    // a pattern/range/class matched nothing (distinct from an unknown name).
    auto resolve = [&](const std::string& term, bool* matched) {
        std::vector<int> hits;
        *matched = false;
        // 1. Exact name always wins, whatever the term looks like.
        hits = exact_matches(term);
        if (!hits.empty()) { *matched = true; return hits; }
        // 2. Type class.
        if (term.size() > 1 && term[0] == '@') {
            std::string cls = term.substr(1);
            for (char& c : cls) c = (char)std::tolower((unsigned char)c);
            for (int i = 0; i < n_fields; ++i)
                if (select_type_class_matches(cls, *schema->field(i)->type()))
                    hits.push_back(i);
            *matched = !hits.empty();
            return hits;
        }
        // 3. 1-based inclusive index range — unless this file's own column
        //    names are range-shaped, in which case the namespace wins.
        int lo = 0, hi = 0;
        if (!schema_has_range_names &&
            select_parse_range(term, n_fields, &lo, &hi)) {
            for (int i = lo; i <= hi; ++i) hits.push_back(i - 1);
            *matched = !hits.empty();
            return hits;
        }
        // 4. Glob.
        if (term.find_first_of("*?") != std::string::npos) {
            for (int i = 0; i < n_fields; ++i)
                if (fnmatch(term.c_str(), schema->field(i)->name().c_str(),
                            0) == 0)
                    hits.push_back(i);
            *matched = !hits.empty();
            return hits;
        }
        return hits;   // plain name, not found
    };

    std::vector<int> out;
    // Whether any term has been applied yet. A bare leading `!` starts from
    // the full set, but only ONCE — using out.empty() as the proxy would
    // re-seed after an exclusion had emptied the accumulator, so
    // `--select 'Chr,!Chr,!Score'` would resurrect the very column the user
    // excluded. `!X` must never be able to add X back.
    bool seeded = false;
    auto add = [&](int idx) {
        for (int have : out) if (have == idx) return;   // dedupe, keep first
        out.push_back(idx);
    };

    size_t pos = 0;
    while (pos <= cfg.select_cols.size()) {
        size_t comma = cfg.select_cols.find(',', pos);
        std::string term = cfg.select_cols.substr(pos,
            comma == std::string::npos ? std::string::npos : comma - pos);
        while (!term.empty() && std::isspace((unsigned char)term.front())) term.erase(0, 1);
        while (!term.empty() && std::isspace((unsigned char)term.back()))  term.pop_back();
        if (!term.empty()) {
            // An exact field named "!x" is still reachable by name, so check
            // for that before treating a leading '!' as exclusion.
            bool exclude = term[0] == '!' && term.size() > 1 &&
                           schema->GetFieldIndex(term) < 0;
            const std::string body = exclude ? term.substr(1) : term;
            bool matched = false;
            std::vector<int> hits = resolve(body, &matched);
            if (!matched) {
                if (unknown_out) {
                    // Distinguish "no such column" from "pattern matched
                    // nothing": both are errors, but the wording differs.
                    // resolve() already ruled out an exact name, so anything
                    // shaped like a pattern IS one.
                    int rlo = 0, rhi = 0;
                    bool is_pattern =
                        body.find_first_of("*?") != std::string::npos ||
                        (body.size() > 1 && body[0] == '@') ||
                        (!schema_has_range_names &&
                         select_parse_range(body, n_fields, &rlo, &rhi));
                    unknown_out->push_back(
                        (is_pattern ? std::string(kNoMatchPrefix) : std::string()) + term);
                }
            } else if (exclude) {
                if (!seeded) { out = seed_all(); seeded = true; }
                for (int h : hits)
                    out.erase(std::remove(out.begin(), out.end(), h), out.end());
            } else {
                seeded = true;
                for (int h : hits) add(h);
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    // Every term resolved, yet nothing is selected. Don't hand back an empty
    // column set that each output path would render as a valid empty result.
    if (out.empty() && unknown_out && unknown_out->empty())
        unknown_out->push_back(kEmptyResultMarker);
    return out;
}

// Levenshtein distance, capped: we only care whether a name is *close* to a
// real one, so bail out as soon as the whole row exceeds `max`.
static int edit_distance(const std::string& a, const std::string& b, int max) {
    if (std::abs((int)a.size() - (int)b.size()) > max) return max + 1;
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = (int)i;
        int row_min = cur[0];
        for (size_t j = 1; j <= b.size(); ++j) {
            int cost = (std::tolower((unsigned char)a[i - 1]) ==
                        std::tolower((unsigned char)b[j - 1])) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > max) return max + 1;
        prev.swap(cur);
    }
    return prev[b.size()];
}

// Format the "unknown column(s) in --select" message, appending a did-you-mean
// for each name that is close to a real field. Shared by every --select
// consumer (delimited / markdown / json / parquet / arrow / describe) so the
// wording is identical wherever a typo is caught.
std::string unknown_columns_error(const TabularSource& src,
                                  const std::vector<std::string>& unknown) {
    auto schema = src.schema();
    // Two distinct failures share this path: a name that doesn't exist, and a
    // pattern that matched nothing. Report them separately — a silently-empty
    // `!pct_*` typo is data loss in --parquet, not a no-op.
    std::vector<std::string> names, patterns;
    for (const auto& u : unknown) {
        if (!u.empty() && u[0] == kEmptyResultMarker[0])
            return "--select resolved to no columns (every term was excluded "
                   "or empty); nothing would be written";
        if (!u.empty() && u[0] == kNoMatchPrefix[0]) patterns.push_back(u.substr(1));
        else                                         names.push_back(u);
    }
    if (names.empty() && !patterns.empty()) {
        std::string msg = "--select pattern(s) matched no column: ";
        for (size_t k = 0; k < patterns.size(); ++k) {
            if (k) msg += ", ";
            msg += patterns[k];
        }
        return msg;
    }
    std::string msg = "unknown column(s) in --select: ";
    for (size_t k = 0; k < names.size(); ++k) {
        if (k) msg += ", ";
        msg += names[k];
        // Compare the NAME, not the `!` that marks an exclusion — otherwise
        // `--select '!Chrr'` never gets a suggestion because the leading '!'
        // eats the whole edit budget.
        const std::string& probe =
            (names[k].size() > 1 && names[k][0] == '!') ? names[k].substr(1)
                                                        : names[k];
        // Allow one edit per 4 characters (min 1, max 3) — tight enough that
        // an unrelated name never gets suggested.
        int budget = std::min(3, std::max(1, (int)probe.size() / 4));
        int best = budget + 1;
        std::string best_name;
        for (int i = 0; i < schema->num_fields(); ++i) {
            const std::string& f = schema->field(i)->name();
            int d = edit_distance(probe, f, budget);
            if (d < best) { best = d; best_name = f; }
        }
        if (!best_name.empty()) msg += " (did you mean '" + best_name + "'?)";
    }
    for (size_t k = 0; k < patterns.size(); ++k) {
        msg += (k || !names.empty()) ? ", " : "";
        msg += patterns[k] + " (pattern matched no column)";
    }
    return msg;
}
