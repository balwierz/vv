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

void write_csv_field(const std::string& val, char sep) {
    FILE* out = out_stream();
    bool needs_quote = val.find(sep)  != std::string::npos ||
                       val.find('"')  != std::string::npos ||
                       val.find('\n') != std::string::npos ||
                       val.find('\r') != std::string::npos;
    if (!needs_quote) {
        std::fputs(val.c_str(), out);
        return;
    }
    std::fputc('"', out);
    for (char c : val) {
        if (c == '"') std::fputc('"', out);   // double the quote
        std::fputc(c, out);
    }
    std::fputc('"', out);
}

// Suffix-match helper shared by DelimitedSource::open and open_source.
static bool fends(const std::string& s, const std::string& sfx) {
    return s.size() >= sfx.size() &&
           s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}
// Case-insensitive variant. Used for filename extensions so that
// `.bigwig` (ENCODE convention), `.bigWig` (UCSC docs), `.BIGWIG`
// (Windows habit) all resolve to the same matcher.
bool fends_ci(const std::string& s, const std::string& sfx) {
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
