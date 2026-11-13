// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Export sinks and input-stream helpers.

#include "internal.hpp"

// ── Delimited output ─────────────────────────────────────────────────────────

FILE* out_stream() {
    return (t_export && t_export->out) ? t_export->out : stdout;
}
bool export_canceled() {
    return t_export && t_export->progress && t_export->progress->cancel.load();
}
void export_count(int64_t rows) {
    if (t_export && t_export->progress) t_export->progress->rows += rows;
}

void out_write(const char* p, size_t n) {
    if (t_export && t_export->hash) t_export->hash->update(p, n);
    else std::fwrite(p, 1, n, out_stream());
}

std::string csv_field_text(const std::string& val, char sep) {
    bool needs_quote = val.find(sep)  != std::string::npos ||
                       val.find('"')  != std::string::npos ||
                       val.find('\n') != std::string::npos ||
                       val.find('\r') != std::string::npos;
    if (!needs_quote) return val;
    std::string q = "\"";
    for (char c : val) {
        if (c == '"') q += '"';   // double the quote
        q += c;
    }
    q += '"';
    return q;
}

void write_csv_field(const std::string& val, char sep) {
    const std::string t = csv_field_text(val, sep);
    out_write(t.data(), t.size());
}

// Suffix-match helper shared by DelimitedSource::open and open_source.
static bool fends(const std::string& s, const std::string& sfx) {
    return s.size() >= sfx.size() &&
           s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}
// Case-insensitive variant. Used for filename extensions so that
// `.bigwig` (ENCODE convention), `.bigWig` (UCSC docs), `.BIGWIG`
// (Windows habit) all resolve to the same matcher. A URL is matched without
// its ?query / #fragment, so a pre-signed https://.../x.parquet?X-Amz-...
// dispatches as .parquet.
bool fends_ci(const std::string& s_in, const std::string& sfx) {
    if (s_in.find("://") != std::string::npos && s_in.find_first_of("?#") != std::string::npos) {
        const std::string s = strip_url_query(s_in);
        if (s.size() != s_in.size()) return fends_ci(s, sfx);
    }
    const std::string& s = s_in;
    if (s.size() < sfx.size()) return false;
    for (size_t i = 0; i < sfx.size(); ++i) {
        char a = s[s.size() - sfx.size() + i];
        char b = sfx[i];
        if (std::tolower((unsigned char)a) !=
            std::tolower((unsigned char)b)) return false;
    }
    return true;
}

// ── Preamble helpers and stream wrappers ──────────────────────────────────────
