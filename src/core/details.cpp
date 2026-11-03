// File details (`--info`, vvg's Details ▸ File): the "File" section every
// format shares, the source's own sections, and the schema's key-value
// metadata. Read from the file system, footers and headers, never the data.
#include "internal.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace {

void add_row(DetailItem& sec, std::string key, std::string value) {
    sec.children.push_back({std::move(key), std::move(value), {}});
}

// The first bytes of a local file; fewer when it is shorter or unreadable.
std::string head_bytes(const std::string& path, size_t n) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::string b(n, '\0');
    f.read(b.data(), (std::streamsize)n);
    b.resize((size_t)f.gcount());
    return b;
}

// gzip with BGZF's "BC" extra subfield: block-compressed, so htslib can
// index it and read it in parallel.
bool is_bgzf_header(const std::string& b) {
    return b.size() >= 16 && (uint8_t)b[0] == 0x1f && (uint8_t)b[1] == 0x8b &&
           (b[3] & 4) && b[12] == 'B' && b[13] == 'C';
}

// Index and sidecar files next to `path` that vv or htslib use.
std::vector<std::string> index_files(const std::string& path) {
    std::string stem = path;
    if (const size_t dot = path.rfind('.'), slash = path.find_last_of("/\\");
        dot != std::string::npos && (slash == std::string::npos || dot > slash))
        stem = path.substr(0, dot);
    std::vector<std::string> found;
    std::error_code ec;
    for (const std::string& c : {path + ".tbi", path + ".csi", path + ".bai", stem + ".bai",
                                 path + ".crai", path + ".fai", path + ".gzi", path + ".idx"})
        if (std::filesystem::is_regular_file(c, ec) &&
            std::find(found.begin(), found.end(), c) == found.end())
            found.push_back(c);
    for (auto& f : found) f = std::filesystem::path(f).filename().string();
    return found;
}

// A value on one line: newlines and tabs written out, other control bytes
// as '?'; cut to `max_w` columns with the full size when longer.
std::string one_line(const std::string& v, int max_w) {
    std::string s;
    for (char c : v) {
        if (c == '\n')      s += "\\n";
        else if (c == '\t') s += "\\t";
        else if ((unsigned char)c < 0x20) s += '?';
        else                s += c;
    }
    if (max_w > 0 && display_width(s) > max_w)
        s = truncate(s, max_w) + " (" + detail_size((int64_t)v.size()) + ")";
    return s;
}

// `max_w` cuts the values of the Metadata section only: a writer's JSON
// (pandas, a LociSSD manifest) runs to kilobytes on one line, while a path or
// a column's min / max is shown whole.
void text_rows(const std::vector<DetailItem>& items, int depth, int max_w, bool color,
               std::string* out) {
    for (const DetailItem& it : items) {
        out->append(2 * depth, ' ');
        if (color) *out += g_color.meta_key;
        *out += it.key;
        if (color) *out += g_color.reset;
        if (!it.value.empty()) *out += (depth == 0 ? "  " : ": ") + one_line(it.value, max_w);
        *out += '\n';
        text_rows(it.children, depth + 1, depth == 0 && it.key == "Metadata" ? max_w : 0,
                  color, out);
    }
}

// The children of one item as a JSON object: a row with no rows of its own
// is "key": "value"; one with rows is an object holding them, its own value
// under "value". A key repeated among siblings gets " #2", " #3", ….
void json_object(const std::vector<DetailItem>& items, const std::string* value) {
    std::printf("{");
    bool first = true;
    if (value && !value->empty()) {
        std::printf("\"value\": ");
        json_emit_string(*value);
        first = false;
    }
    std::map<std::string, int> seen;
    for (const DetailItem& it : items) {
        std::string key = it.key;
        if (const int n = ++seen[it.key]; n > 1) key += " #" + std::to_string(n);
        std::printf("%s", first ? "" : ", ");
        first = false;
        json_emit_string(key);
        std::printf(": ");
        if (it.children.empty()) json_emit_string(it.value);
        else                     json_object(it.children, &it.value);
    }
    std::printf("}");
}

}  // namespace

std::string detail_size(int64_t bytes) {
    char buf[32];
    if      (bytes < 1024)             std::snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
    else if (bytes < 1024 * 1024)      std::snprintf(buf, sizeof(buf), "%.1f KiB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.2f MiB", bytes / (1024.0 * 1024));
    else                               std::snprintf(buf, sizeof(buf), "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
    return buf;
}

std::string detail_ratio(int64_t raw, int64_t stored) {
    if (stored <= 0 || raw <= 0) return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f\xc3\x97", (double)raw / (double)stored);
    return buf;
}

FileDetails file_details(TabularSource& src) {
    FileDetails d;
    DetailItem file{"File", "", {}};
    const std::string& path = src.path();
    add_row(file, "Path", display_path(path));
    const std::string fmt = format_label_of(src);
    if (!fmt.empty()) add_row(file, "Format", fmt);
    std::error_code ec;
    if (!path.empty() && path != "-" && !is_remote_url(path) &&
        std::filesystem::is_regular_file(path, ec)) {
        const auto sz = std::filesystem::file_size(path, ec);
        if (!ec) add_row(file, "Size", digits_with_sep(std::to_string(sz)) + " bytes" +
                                       (sz >= 1024 ? " (" + detail_size((int64_t)sz) + ")" : ""));
        const std::string head = head_bytes(path, 18);
        const StreamCodec c = sniff_codec((const uint8_t*)head.data(), head.size());
        if (c != StreamCodec::None)
            add_row(file, "Compression",
                    std::string(codec_label(c)) + (is_bgzf_header(head) ? " (BGZF)" : ""));
        std::string idx;
        for (const auto& f : index_files(path)) idx += (idx.empty() ? "" : ", ") + f;
        if (!idx.empty()) add_row(file, "Index", idx);
    }
    if (const int64_t n = src.total_rows(); n >= 0)
        add_row(file, "Rows", digits_with_sep(std::to_string(n)));
    if (auto sch = src.schema())
        add_row(file, "Columns", digits_with_sep(std::to_string(sch->num_fields())));
    if (const std::string w = src.created_by(); !w.empty()) add_row(file, "Created by", w);
    d.push_back(std::move(file));

    src.add_details(d);

    // Parquet's ARROW:schema is consumed by the Arrow reader and never
    // reaches here; it would only repeat the columns.
    if (auto sch = src.schema(); sch && sch->metadata() && sch->metadata()->size() > 0) {
        const auto& kv = *sch->metadata();
        DetailItem meta{"Metadata", "", {}};
        for (int64_t i = 0; i < kv.size(); ++i) add_row(meta, kv.key(i), kv.value(i));
        d.push_back(std::move(meta));
    }
    return d;
}

std::string details_text(const FileDetails& d, int max_value) {
    std::string out;
    text_rows(d, 0, max_value, /*color=*/false, &out);
    return out;
}

const DetailItem* find_detail(const FileDetails& d, const std::string& key) {
    for (const DetailItem& it : d)
        if (it.key == key) return &it;
    return nullptr;
}

void print_details(const FileDetails& d, bool json) {
    if (json) {
        json_object(d, nullptr);
        std::printf("\n");
        return;
    }
    std::string out;
    text_rows(d, 0, 100, /*color=*/true, &out);
    std::fputs(out.c_str(), stdout);
}
