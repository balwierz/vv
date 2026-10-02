// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// --filter expressions, --select, and region modifiers.

#include "internal.hpp"

// ── Simple value-predicate filter (--filter) ─────────────────────────────────
//
// Grammar:
//   filter   := and-clause ( OR  and-clause )*
//   clause   := atom       ( AND atom       )*
//   atom     := IDENT OP LITERAL
//   OP       := == | != | < | <= | > | >=
//   LITERAL  := int | float | "double-quoted" | 'single-quoted'
//
// Case-insensitive AND/OR. Whitespace-separated tokens. Strings may
// contain anything but the surrounding quote (no escapes — keep simple).
//
// The expression is evaluated per row over a Table whose column order
// matches the source's full schema, so atoms reference columns by their
// schema field index (resolved at parse time from the column name).
// (FilterAtom + FilterExpr are defined in vv/vvcore.hpp, included near the top.)

// Token for a backtick-quoted name with no closing backtick.
static constexpr const char kFilterBadBacktick[] = "\x01`";

// A column-name token: a bare word, or `name` with its backticks removed.
static bool filter_is_backticked(const std::string& t) {
    return t.size() >= 2 && t.front() == '`' && t.back() == '`';
}
static std::string filter_column_name(const std::string& t) {
    return filter_is_backticked(t) ? t.substr(1, t.size() - 2) : t;
}

static std::vector<std::string> filter_tokenize(const std::string& s) {
    std::vector<std::string> toks;
    size_t i = 0;
    while (i < s.size()) {
        if (std::isspace((unsigned char)s[i])) { ++i; continue; }
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i++];
            std::string lit = std::string(1, q);
            while (i < s.size() && s[i] != q) lit += s[i++];
            if (i < s.size()) lit += s[i++];  // closing quote
            toks.push_back(lit);
            continue;
        }
        // `name`: a column name that is not a bare word — it has a space or
        // operator characters (`Sample ID`, `End)`). A doubled backtick
        // stands for one inside it. The token keeps the backticks so the
        // parser can tell it from a word; an unterminated one is lexed as
        // kFilterBadBacktick.
        if (s[i] == '`') {
            std::string name = "`";
            bool closed = false;
            for (++i; i < s.size(); ++i) {
                if (s[i] != '`') { name += s[i]; continue; }
                if (i + 1 < s.size() && s[i + 1] == '`') { name += '`'; ++i; continue; }
                ++i; closed = true; break;
            }
            toks.push_back(closed ? name + "`" : std::string(kFilterBadBacktick));
            continue;
        }
        // `in (...)` punctuation, each its own token.
        if (s[i]=='(' || s[i]==')' || s[i]==',') {
            toks.push_back(std::string(1, s[i++]));
            continue;
        }
        // Operator characters as a chunk: == != < <= > >= ~ !~
        if (s[i]=='='||s[i]=='!'||s[i]=='<'||s[i]=='>'||s[i]=='~') {
            std::string op(1, s[i++]);
            // '!' pairs with '=' (!=) and with '~' (!~); the others only '='.
            if (i < s.size() && (s[i]=='=' || (op=="!" && s[i]=='~')))
                op += s[i++];
            toks.push_back(op);
            continue;
        }
        // Bare word: identifier or numeric literal. The terminator set must
        // include the new punctuation, or `Gene~"BRCA"` lexes as one word
        // `Gene~` and `in("A","B")` as a single token.
        std::string w;
        while (i < s.size() && !std::isspace((unsigned char)s[i])
               && s[i]!='"' && s[i]!='\'' && s[i]!='`'
               && s[i]!='=' && s[i]!='!' && s[i]!='<' && s[i]!='>'
               && s[i]!='~' && s[i]!='(' && s[i]!=')' && s[i]!=',')
            w += s[i++];
        if (!w.empty()) toks.push_back(w);
    }
    return toks;
}

// A column name as a filter token: bare when it is a plain identifier that is
// not a filter keyword, else in backticks with any backtick doubled.
std::string filter_quote_name(const std::string& name) {
    static const char* const kWords[] = {"and", "or", "not", "is", "null", "in",
        "contains", "startswith", "endswith", "has", "lacks"};
    bool bare = !name.empty() && (std::isalpha((unsigned char)name[0]) || name[0] == '_');
    for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '.')) { bare = false; break; }
    std::string lower;
    for (char c : name) lower += (char)std::tolower((unsigned char)c);
    for (const char* w : kWords) if (lower == w) bare = false;
    if (bare) return name;
    std::string q = "`";
    for (char c : name) { q += c; if (c == '`') q += '`'; }
    return q + "`";
}

// Split a filter expression at its top-level OR words, keeping each branch's
// text as written (quoted strings, `names` and in-lists are not split). The
// grammar has no grouping, so ANDing a condition into a filter means adding
// it to every branch.
std::vector<std::string> filter_split_or(const std::string& s) {
    std::vector<std::string> out;
    auto boundary = [](char c) {
        return std::isspace((unsigned char)c) || c == '"' || c == '\'' || c == '`' ||
               c == '(' || c == ')';
    };
    size_t start = 0, i = 0;
    int depth = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"' || c == '\'') {                    // quoted literal
            size_t e = s.find(c, i + 1);
            i = e == std::string::npos ? s.size() : e + 1;
            continue;
        }
        if (c == '`') {                                 // `name`, `` inside
            for (++i; i < s.size(); ++i) {
                if (s[i] != '`') continue;
                if (i + 1 < s.size() && s[i + 1] == '`') { ++i; continue; }
                ++i; break;
            }
            continue;
        }
        if (c == '(') ++depth;
        if (c == ')' && depth > 0) --depth;
        if (depth == 0 && i + 2 <= s.size() && (i == 0 || boundary(s[i - 1])) &&
            std::tolower((unsigned char)s[i]) == 'o' &&
            std::tolower((unsigned char)s[i + 1]) == 'r' &&
            (i + 2 == s.size() || boundary(s[i + 2]))) {
            out.push_back(s.substr(start, i - start));
            start = i + 2;
            i += 2;
            continue;
        }
        ++i;
    }
    out.push_back(s.substr(start));
    for (auto& b : out) {
        while (!b.empty() && std::isspace((unsigned char)b.front())) b.erase(0, 1);
        while (!b.empty() && std::isspace((unsigned char)b.back()))  b.pop_back();
    }
    return out;
}

static bool filter_parse_op(const std::string& t, FilterAtom::Op* op) {
    if (t == "==") { *op = FilterAtom::Eq; return true; }
    if (t == "!=") { *op = FilterAtom::Ne; return true; }
    if (t == "<")  { *op = FilterAtom::Lt; return true; }
    if (t == "<=") { *op = FilterAtom::Le; return true; }
    if (t == ">")  { *op = FilterAtom::Gt; return true; }
    if (t == ">=") { *op = FilterAtom::Ge; return true; }
    if (t == "~")  { *op = FilterAtom::Match;    return true; }
    if (t == "!~") { *op = FilterAtom::NotMatch; return true; }
    return false;
}


// Case-insensitive token compare, used for the word operators and AND / OR.
static bool filter_tok_is(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t k = 0; k < a.size(); ++k)
        if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]))
            return false;
    return true;
}

// Strip surrounding quotes from a literal token, if present.
static std::string filter_unquote(const std::string& lit) {
    if (lit.size() >= 2 && (lit.front() == '"' || lit.front() == '\'')
        && lit.front() == lit.back())
        return lit.substr(1, lit.size() - 2);
    return lit;
}

// The members of `col in @path`: one value per line — the line's first
// tab-separated field, trimmed; blank lines skipped. A gzip or zstd file
// (detected by its magic bytes) is decompressed. Returns "" or an error.
static std::string read_filter_set_file(const std::string& path,
                                        std::vector<std::string>* out) {
    auto rf = arrow::io::ReadableFile::Open(path);
    if (!rf.ok()) return "cannot open '" + path + "' for 'in @': " + rf.status().message();
    std::shared_ptr<arrow::io::InputStream> in = *rf;
    auto head = (*rf)->ReadAt(0, 4);
    if (auto st = (*rf)->Seek(0); !st.ok()) return path + ": " + st.message();
    if (head.ok() && (*head)->size() >= 2) {
        const uint8_t* b = (*head)->data();
        std::optional<arrow::Compression::type> comp;
        if (b[0] == 0x1f && b[1] == 0x8b) comp = arrow::Compression::GZIP;
        else if ((*head)->size() >= 4 && b[0] == 0x28 && b[1] == 0xb5 && b[2] == 0x2f && b[3] == 0xfd)
            comp = arrow::Compression::ZSTD;
        if (comp) {
            auto codec = arrow::util::Codec::Create(*comp);
            if (!codec.ok()) return path + ": " + codec.status().message();
            auto ci = arrow::io::CompressedInputStream::Make(codec->get(), *rf);
            if (!ci.ok()) return path + ": " + ci.status().message();
            in = *ci;
        }
    }
    std::string text;
    for (;;) {
        auto buf = in->Read(1 << 20);
        if (!buf.ok()) return path + ": " + buf.status().message();
        if ((*buf)->size() == 0) break;
        text.append(reinterpret_cast<const char*>((*buf)->data()), (size_t)(*buf)->size());
    }
    size_t p = 0;
    while (p < text.size()) {
        size_t nl = text.find('\n', p);
        std::string line = text.substr(p, (nl == std::string::npos ? text.size() : nl) - p);
        p = (nl == std::string::npos) ? text.size() : nl + 1;
        if (auto tab = line.find('\t'); tab != std::string::npos) line.resize(tab);
        while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && std::isspace((unsigned char)line[lead])) ++lead;
        if (lead) line.erase(0, lead);
        if (!line.empty()) out->push_back(std::move(line));
    }
    return "";
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian; Howard Hinnant's
// days_from_civil).
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// A date / time literal as a temporal column stores it: days (date32),
// milliseconds (date64) or the timestamp's unit since the epoch. Accepts
// YYYY-MM-DD, optionally followed by [ T]HH:MM[:SS[.fraction]] and a zone
// (Z or ±HH:MM / ±HHMM); a date alone is midnight. A literal with a zone is
// converted to UTC (how Arrow stores a timestamp); one without is taken as
// written. False if `lit` is not such a literal or `type` is not a date /
// timestamp.
static bool parse_temporal_literal(const std::string& lit, const arrow::DataType& type,
                                   int64_t* out) {
    int Y, M, D, h = 0, mi = 0, sec = 0, n = 0;
    if (std::sscanf(lit.c_str(), "%4d-%2d-%2d%n", &Y, &M, &D, &n) != 3 || n != 10) return false;
    if (M < 1 || M > 12 || D < 1 || D > 31) return false;
    size_t p = 10;
    int64_t frac_ns = 0, offset_s = 0;
    if (p < lit.size() && (lit[p] == ' ' || lit[p] == 'T')) {
        int m2 = 0;
        if (std::sscanf(lit.c_str() + p + 1, "%2d:%2d%n", &h, &mi, &m2) != 2 || m2 != 5) return false;
        p += 1 + 5;
        if (p < lit.size() && lit[p] == ':') {
            int m3 = 0;
            if (std::sscanf(lit.c_str() + p + 1, "%2d%n", &sec, &m3) != 1 || m3 != 2) return false;
            p += 3;
            if (p < lit.size() && lit[p] == '.') {
                ++p;
                int digits = 0;
                while (p < lit.size() && std::isdigit((unsigned char)lit[p])) {
                    if (digits < 9) { frac_ns = frac_ns * 10 + (lit[p] - '0'); ++digits; }
                    ++p;
                }
                if (digits == 0) return false;
                for (; digits < 9; ++digits) frac_ns *= 10;
            }
        }
        if (h > 23 || mi > 59 || sec > 60) return false;
    }
    if (p < lit.size()) {
        if (lit[p] == 'Z' && p + 1 == lit.size()) {
            ++p;
        } else if ((lit[p] == '+' || lit[p] == '-')) {
            int oh = 0, om = 0, m4 = 0;
            const char* z = lit.c_str() + p + 1;
            if (std::sscanf(z, "%2d:%2d%n", &oh, &om, &m4) == 2 && m4 == 5) {}
            else if (std::sscanf(z, "%2d%2d%n", &oh, &om, &m4) == 2 && m4 == 4) {}
            else return false;
            offset_s = (lit[p] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
            p += 1 + m4;
        }
        if (p != lit.size()) return false;
    }
    const int64_t days = days_from_civil(Y, (unsigned)M, (unsigned)D);
    const int64_t secs = days * 86400 + h * 3600 + mi * 60 + sec - offset_s;
    switch (type.id()) {
        case arrow::Type::DATE32:
            if (days < INT32_MIN || days > INT32_MAX) return false;
            *out = days;
            return true;
        case arrow::Type::DATE64:
            return !__builtin_mul_overflow(days, (int64_t)86400000, out);
        case arrow::Type::TIMESTAMP: {
            // A literal outside the unit's int64 range (year 2262+ in ns) is
            // not representable: reject it rather than overflow.
            int64_t scale = 1, frac = 0;
            switch (static_cast<const arrow::TimestampType&>(type).unit()) {
                case arrow::TimeUnit::SECOND: break;
                case arrow::TimeUnit::MILLI:  scale = 1000;       frac = frac_ns / 1000000; break;
                case arrow::TimeUnit::MICRO:  scale = 1000000;    frac = frac_ns / 1000;    break;
                case arrow::TimeUnit::NANO:   scale = 1000000000; frac = frac_ns;           break;
            }
            int64_t v;
            if (__builtin_mul_overflow(secs, scale, &v) || __builtin_add_overflow(v, frac, &v))
                return false;
            *out = v;
            return true;
        }
        default: return false;
    }
}

bool is_date_or_timestamp(const arrow::DataType& t) {
    return t.id() == arrow::Type::DATE32 || t.id() == arrow::Type::DATE64 ||
           t.id() == arrow::Type::TIMESTAMP;
}

// Hash an In / NotIn atom's members once, so a row costs one lookup instead
// of a scan of the list (and, on a numeric column, a stod per member).
static void hash_filter_set(FilterAtom* a, const arrow::DataType* col_type = nullptr) {
    auto text = std::make_shared<std::unordered_set<std::string>>(a->set_lits.begin(),
                                                                   a->set_lits.end());
    auto num = std::make_shared<std::unordered_set<double>>();
    for (const auto& m : a->set_lits) {
        // On a date / timestamp column a date literal is its stored count.
        int64_t t;
        if (col_type && is_date_or_timestamp(*col_type) && parse_temporal_literal(m, *col_type, &t)) {
            num->insert((double)t);
            continue;
        }
        char* end = nullptr;
        double d = std::strtod(m.c_str(), &end);
        if (col_type && col_type->id() == arrow::Type::FLOAT) d = (float)d;   // as the cells
        if (end && end != m.c_str() && !*end) num->insert(d);
    }
    a->set_text = std::move(text);
    a->set_num = std::move(num);
}

// Parse the user's `--filter` expression. Returns true on success and
// populates `out`. On failure, writes a human-readable reason to `err`.
bool parse_filter_expr(const std::string& expr,
                              const arrow::Schema& schema,
                              FilterExpr* out, std::string* err) {
    out->groups.clear();
    auto toks = filter_tokenize(expr);
    if (toks.empty()) { *err = "empty filter expression"; return false; }
    for (const auto& t : toks)
        if (t == kFilterBadBacktick) {
            *err = "unterminated `name` (a column name opened with ` needs a closing `)";
            return false;
        }
    auto eq_ci = [](const std::string& a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t k = 0; k < a.size(); ++k)
            if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k]))
                return false;
        return true;
    };
    out->groups.emplace_back();
    size_t i = 0;
    while (i < toks.size()) {
        if (i + 2 > toks.size()) {
            *err = "expected '<column> <op> <value>' near token '" + toks[i] + "'";
            return false;
        }
        FilterAtom a;
        const std::string col = filter_column_name(toks[i]);
        a.col_idx = schema.GetFieldIndex(col);
        if (a.col_idx < 0) {
            *err = "unknown column '" + col + "' in filter";
            // A name with a space or operator characters splits into several
            // tokens; say how to write it.
            if (!filter_is_backticked(toks[i]) && i + 1 < toks.size())
                for (int f = 0; f < schema.num_fields(); ++f) {
                    const std::string& n = schema.field(f)->name();
                    if (n.rfind(col, 0) == 0 && n.size() > col.size()) {
                        *err += " (for a name like '" + n + "', write `" + n + "`)";
                        break;
                    }
                }
            return false;
        }
        // Word operators are operators only in operator position, so a column
        // genuinely named `in`, `is` or `contains` stays filterable.
        const std::string& opt = toks[i + 1];
        size_t next = 0;                       // index just past this atom

        auto need_literal = [&](size_t at, std::string* dst) {
            if (at >= toks.size()) {
                *err = "expected a value after '" + opt + "'";
                return false;
            }
            if (filter_is_backticked(toks[at])) {
                *err = toks[at] + " names a column; a value is a number or a "
                       "quoted string";
                return false;
            }
            *dst = filter_unquote(toks[at]);
            return true;
        };

        if (filter_tok_is(opt, "has") || filter_tok_is(opt, "lacks")) {
            // has / lacks NAME[,NAME…] — bits of an integer column (SAM FLAG):
            // has = every listed bit set, lacks = none set. A member is a
            // flag name or a number (4, 0x904).
            a.op = filter_tok_is(opt, "has") ? FilterAtom::Has : FilterAtom::Lacks;
            a.kind = FilterAtom::K_Int;
            if (!is_integer_type(schema.field(a.col_idx)->type()->id())) {
                *err = "'" + opt + "' tests bits of an integer column; '" + col +
                       "' is " + type_label(*schema.field(a.col_idx)->type());
                return false;
            }
            size_t p = i + 2;
            int64_t mask = 0;
            for (;;) {
                if (p >= toks.size()) { *err = "expected a flag after '" + opt + "'"; return false; }
                const std::string& t = toks[p];
                int64_t bit = -1;
                for (const auto& [nm, v] : kSamFlagNames)
                    if (filter_tok_is(t, nm)) { bit = v; break; }
                if (bit < 0) {
                    try {
                        size_t used = 0;
                        long long v = std::stoll(t, &used, 0);   // 4, 0x904
                        if (used == t.size() && v >= 0) bit = v;
                    } catch (...) {}
                }
                if (bit < 0) {
                    std::string names;
                    for (const auto& [nm, v] : kSamFlagNames) names += std::string(names.empty() ? "" : " ") + nm;
                    *err = "unknown flag '" + t + "' after '" + opt +
                           "' (a number, or one of: " + names + ")";
                    return false;
                }
                mask |= bit;
                if (p + 1 < toks.size() && toks[p + 1] == ",") { p += 2; continue; }
                break;
            }
            a.i_lit = mask;
            next = p + 1;
        } else if (filter_tok_is(opt, "is")) {
            // is null | is not null
            if (i + 2 < toks.size() && filter_tok_is(toks[i+2], "null")) {
                a.op = FilterAtom::IsNull; a.kind = FilterAtom::K_None;
                next = i + 3;
            } else if (i + 3 < toks.size() && filter_tok_is(toks[i+2], "not")
                       && filter_tok_is(toks[i+3], "null")) {
                a.op = FilterAtom::NotNull; a.kind = FilterAtom::K_None;
                next = i + 4;
            } else {
                *err = "expected 'is null' or 'is not null'";
                return false;
            }
        } else if (filter_tok_is(opt, "in") ||
                   (filter_tok_is(opt, "not") && i + 2 < toks.size() &&
                    filter_tok_is(toks[i+2], "in"))) {
            // in (a, b, c) | not in (a, b, c) | [not] in @file
            bool negate = filter_tok_is(opt, "not");
            size_t p = i + (negate ? 3 : 2);
            a.op   = negate ? FilterAtom::NotIn : FilterAtom::In;
            a.kind = FilterAtom::K_String;
            // in (a, b, c), or in @path / in @"path with spaces": the members
            // come from a file (read_filter_set_file).
            if (p < toks.size() && !toks[p].empty() && toks[p][0] == '@') {
                std::string path = toks[p].substr(1);
                if (path.empty() && p + 1 < toks.size()) path = filter_unquote(toks[++p]);
                if (path.empty()) { *err = "expected a file after 'in @'"; return false; }
                if (auto ferr = read_filter_set_file(path, &a.set_lits); !ferr.empty()) {
                    *err = ferr;
                    return false;
                }
                if (a.set_lits.empty()) { *err = "'" + path + "' lists no values for 'in @'"; return false; }
                next = p + 1;
            } else {
                if (p >= toks.size() || toks[p] != "(") {
                    *err = "expected '(' or '@file' after 'in'";
                    return false;
                }
                ++p;
                while (p < toks.size() && toks[p] != ")") {
                    if (toks[p] == ",") { ++p; continue; }
                    if (filter_is_backticked(toks[p])) {
                        *err = toks[p] + " names a column; 'in' takes values";
                        return false;
                    }
                    a.set_lits.push_back(filter_unquote(toks[p]));
                    ++p;
                }
                if (p >= toks.size()) { *err = "unterminated 'in (' list"; return false; }
                if (a.set_lits.empty()) { *err = "'in ()' needs at least one value"; return false; }
                next = p + 1;                      // past ')'
            }
            hash_filter_set(&a, schema.field(a.col_idx)->type().get());
        } else if (filter_tok_is(opt, "contains") ||
                   filter_tok_is(opt, "startswith") ||
                   filter_tok_is(opt, "endswith")) {
            a.op = filter_tok_is(opt, "contains")   ? FilterAtom::Contains
                 : filter_tok_is(opt, "startswith") ? FilterAtom::StartsWith
                                                    : FilterAtom::EndsWith;
            a.kind = FilterAtom::K_String;
            if (!need_literal(i + 2, &a.s_lit)) return false;
            next = i + 3;
        } else if (filter_parse_op(opt, &a.op)) {
            if (i + 2 >= toks.size()) {
                *err = "expected a value after '" + opt + "'";
                return false;
            }
            const std::string& lit = toks[i+2];
            if (filter_is_backticked(lit)) {
                *err = lit + " names a column; a value is a number or a quoted string";
                return false;
            }
            if (a.op == FilterAtom::Match || a.op == FilterAtom::NotMatch) {
                // The pattern is always text, never a number.
                a.kind  = FilterAtom::K_String;
                a.s_lit = filter_unquote(lit);
                try { std::regex probe(a.s_lit, std::regex::ECMAScript); (void)probe; }
                catch (const std::regex_error& e) {
                    *err = "bad regex '" + a.s_lit + "': " + e.what();
                    return false;
                }
            } else if (display_type(*schema.field(a.col_idx)) == arrow::Type::BOOL) {
                // A boolean column compares with true / false (bare or
                // quoted, any case) as 1 / 0; text never equalled a bool cell
                // and a bare `true` was rejected as a bad number.
                const std::string v = filter_unquote(lit);
                if (eq_ci(v, "true"))       a.i_lit = 1;
                else if (eq_ci(v, "false")) a.i_lit = 0;
                else {
                    *err = "boolean column '" + col + "' compares with true or false, not '" + v + "'";
                    return false;
                }
                a.kind = FilterAtom::K_Int;
            } else if (lit.size() >= 2 &&
                       (lit.front() == '"' || lit.front() == '\'') &&
                       lit.front() == lit.back()) {
                a.kind  = FilterAtom::K_String;
                a.s_lit = lit.substr(1, lit.size() - 2);
                // A date / timestamp column compares a date literal as a
                // point in time, in the column's own unit; any other text
                // against such a column is an error, not an empty match.
                const auto& ct = *schema.field(a.col_idx)->type();
                if (is_date_or_timestamp(ct)) {
                    int64_t t;
                    if (!parse_temporal_literal(a.s_lit, ct, &t)) {
                        *err = "'" + a.s_lit + "' is not a date / time (YYYY-MM-DD[ HH:MM[:SS[.fff]]]"
                               "[Z|±HH:MM]) within the range of " + ct.ToString() +
                               " column '" + col + "'";
                        return false;
                    }
                    a.kind  = FilterAtom::K_Int;
                    a.i_lit = t;
                }
            } else if (lit.find_first_of(".eE") != std::string::npos) {
                try { a.f_lit = std::stod(lit); }
                catch (...) { *err = "bad number '" + lit + "'"; return false; }
                a.kind = FilterAtom::K_Double;
            } else {
                try { a.i_lit = std::stoll(lit); }
                catch (...) { *err = "bad integer '" + lit + "'"; return false; }
                a.kind = FilterAtom::K_Int;
            }
            next = i + 3;
        } else {
            *err = "expected an operator (== != < <= > >= ~ !~ contains "
                   "startswith endswith in 'is null' has lacks), got '" + opt + "'";
            return false;
        }

        out->groups.back().push_back(std::move(a));
        i = next;
        if (i >= toks.size()) break;
        if (eq_ci(toks[i], "and")) { ++i; continue; }
        if (eq_ci(toks[i], "or"))  { ++i; out->groups.emplace_back(); continue; }
        *err = "expected AND / OR, got '" + toks[i] + "'";
        return false;
    }
    return true;
}

// Walk a ChunkedArray to the array holding `row`, returning it plus the
// offset inside it. One copy of the loop that cell_as_int / cell_as_double /
// cell_as_string / cell_is_null all used to carry separately.
static const arrow::Array* locate_cell(const arrow::Table& tbl, int col,
                                       int64_t row, int64_t* off) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) { *off = r; return ch.get(); }
        r -= ch->length();
    }
    return nullptr;
}

// True when the cell exists and holds a null. Distinct from "could not read
// it": `is null` must match an actual null, not an unsupported type.
static bool cell_is_null(const arrow::Table& tbl, int col, int64_t row) {
    int64_t off = 0;
    const arrow::Array* a = locate_cell(tbl, col, row, &off);
    return a && a->IsNull(off);
}

// Get the int64 / double / string value of cell (col_idx, row) in `tbl`.
// Returns false for nulls or unsupported types.
// Resolve a possibly dictionary-encoded cell to its underlying value array and
// index. For a plain array this is just (&a, r); for a DictionaryArray it is
// the dictionary's array at the decoded index. Returns false when the cell is
// null (a null dictionary index or a null dictionary value). The filter and
// stats accessors all route through this so a categorical column is compared by
// its decoded value, not skipped as an unhandled type.
static bool resolve_dict_cell(const arrow::Array& a, int64_t r,
                              const arrow::Array** out_arr, int64_t* out_idx) {
    const arrow::Array* arr = &a;
    int64_t idx = r;
    if (a.type_id() == arrow::Type::DICTIONARY) {
        const auto& d = static_cast<const arrow::DictionaryArray&>(a);
        if (d.IsNull(r)) return false;
        arr = d.dictionary().get();
        idx = d.GetValueIndex(r);
    }
    if (arr->IsNull(idx)) return false;
    *out_arr = arr;
    *out_idx = idx;
    return true;
}

// An integer-typed cell as int64; false for null, a non-integer type (floats
// included), or a uint64 beyond int64.
static bool cell_as_int(const arrow::Table& tbl, int col, int64_t row,
                         int64_t* out) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) {
            const arrow::Array* a; int64_t i;
            if (!resolve_dict_cell(*ch, r, &a, &i)) return false;
            switch (a->type_id()) {
                case arrow::Type::INT64:  *out = static_cast<const arrow::Int64Array&>(*a).Value(i);  return true;
                case arrow::Type::INT32:  *out = static_cast<const arrow::Int32Array&>(*a).Value(i);  return true;
                case arrow::Type::INT16:  *out = static_cast<const arrow::Int16Array&>(*a).Value(i);  return true;
                case arrow::Type::INT8:   *out = static_cast<const arrow::Int8Array&>(*a).Value(i);   return true;
                case arrow::Type::UINT32: *out = (int64_t)static_cast<const arrow::UInt32Array&>(*a).Value(i); return true;
                case arrow::Type::UINT16: *out = static_cast<const arrow::UInt16Array&>(*a).Value(i); return true;
                case arrow::Type::UINT8:  *out = static_cast<const arrow::UInt8Array&>(*a).Value(i);  return true;
                case arrow::Type::UINT64: {
                    const uint64_t u = static_cast<const arrow::UInt64Array&>(*a).Value(i);
                    if (u > (uint64_t)INT64_MAX) return false;   // compare as double
                    *out = (int64_t)u;
                    return true;
                }
                // Temporal columns are integers underneath (days, ms, us,
                // ns since the epoch): read the count exactly, so a
                // nanosecond timestamp compares without double rounding.
                case arrow::Type::DATE32: *out = static_cast<const arrow::Date32Array&>(*a).Value(i); return true;
                case arrow::Type::DATE64: *out = static_cast<const arrow::Date64Array&>(*a).Value(i); return true;
                case arrow::Type::TIMESTAMP: *out = static_cast<const arrow::TimestampArray&>(*a).Value(i); return true;
                case arrow::Type::TIME32: *out = static_cast<const arrow::Time32Array&>(*a).Value(i); return true;
                case arrow::Type::TIME64: *out = static_cast<const arrow::Time64Array&>(*a).Value(i); return true;
                case arrow::Type::DURATION: *out = static_cast<const arrow::DurationArray&>(*a).Value(i); return true;
                case arrow::Type::BOOL: *out = static_cast<const arrow::BooleanArray&>(*a).Value(i) ? 1 : 0; return true;
                // FLOAT / DOUBLE are not integers: truncating them made
                // `Score > 0` compare 0.05 as 0 and match nothing. The caller
                // compares them as doubles.
                default: return false;
            }
        }
        r -= ch->length();
    }
    return false;
}
// Extract any value of a type is_numeric_type() accepts from array `a` at
// index `r` as a double. Returns false on null or a genuinely non-numeric
// type. Branches on type_id() (no per-cell dynamic_pointer_cast / RTTI).
// Temporal types yield their underlying epoch / elapsed count, so they sort
// chronologically and scale in heatmaps; decimals honour their scale.
bool array_value_as_double(const arrow::Array& a, int64_t r, double* out) {
    if (a.IsNull(r)) return false;
    if (a.type_id() == arrow::Type::DICTIONARY) {
        // Dictionary-encoded numeric column: read the decoded value.
        const auto& d = static_cast<const arrow::DictionaryArray&>(a);
        return array_value_as_double(*d.dictionary(), d.GetValueIndex(r), out);
    }
    switch (a.type_id()) {
        case arrow::Type::DOUBLE: *out = static_cast<const arrow::DoubleArray&>(a).Value(r); return true;
        case arrow::Type::FLOAT:  *out = static_cast<const arrow::FloatArray&>(a).Value(r);  return true;
        case arrow::Type::INT64:  *out = (double)static_cast<const arrow::Int64Array&>(a).Value(r);  return true;
        case arrow::Type::INT32:  *out = (double)static_cast<const arrow::Int32Array&>(a).Value(r);  return true;
        case arrow::Type::INT16:  *out = (double)static_cast<const arrow::Int16Array&>(a).Value(r);  return true;
        case arrow::Type::INT8:   *out = (double)static_cast<const arrow::Int8Array&>(a).Value(r);   return true;
        case arrow::Type::UINT64: *out = (double)static_cast<const arrow::UInt64Array&>(a).Value(r); return true;
        case arrow::Type::UINT32: *out = (double)static_cast<const arrow::UInt32Array&>(a).Value(r); return true;
        case arrow::Type::UINT16: *out = (double)static_cast<const arrow::UInt16Array&>(a).Value(r); return true;
        case arrow::Type::UINT8:  *out = (double)static_cast<const arrow::UInt8Array&>(a).Value(r);  return true;
        case arrow::Type::DATE32: *out = (double)static_cast<const arrow::Date32Array&>(a).Value(r); return true;
        case arrow::Type::DATE64: *out = (double)static_cast<const arrow::Date64Array&>(a).Value(r); return true;
        case arrow::Type::TIME32: *out = (double)static_cast<const arrow::Time32Array&>(a).Value(r); return true;
        case arrow::Type::TIME64: *out = (double)static_cast<const arrow::Time64Array&>(a).Value(r); return true;
        case arrow::Type::TIMESTAMP: *out = (double)static_cast<const arrow::TimestampArray&>(a).Value(r); return true;
        case arrow::Type::DURATION:  *out = (double)static_cast<const arrow::DurationArray&>(a).Value(r);  return true;
        case arrow::Type::DECIMAL128: {
            const auto& arr = static_cast<const arrow::Decimal128Array&>(a);
            const arrow::Decimal128 v(arr.GetValue(r));
            const int scale = static_cast<const arrow::Decimal128Type&>(*a.type()).scale();
            // Decimal128::ToDouble is not correctly rounded (99.99 at scale 2
            // came out 99.99000000000001, so `dec == 99.99` matched nothing).
            // An unscaled value and power of ten that are both exact doubles
            // divide to the nearest double, the value strtod gives the text.
            static constexpr double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
                1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
                1e20, 1e21, 1e22};
            const int64_t hi = v.high_bits();
            const uint64_t lo = v.low_bits();
            const bool fits = (hi == 0 && lo < (1ULL << 53)) ||
                              (hi == -1 && lo >= (uint64_t)-(int64_t)(1LL << 53) && lo != 0);
            if (fits && scale >= 0 && scale <= 22)
                *out = (double)(int64_t)lo / kPow10[scale];
            else
                *out = v.ToDouble(scale);
            return true;
        }
        case arrow::Type::DECIMAL256: {
            const auto& arr = static_cast<const arrow::Decimal256Array&>(a);
            *out = arrow::Decimal256(arr.GetValue(r)).ToDouble(
                static_cast<const arrow::Decimal256Type&>(*a.type()).scale());
            return true;
        }
        default: return false;
    }
}

bool cell_as_double(const arrow::Table& tbl, int col, int64_t row,
                     double* out) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) return array_value_as_double(*ch, r, out);
        r -= ch->length();
    }
    return false;
}
static bool cell_as_string(const arrow::Table& tbl, int col, int64_t row,
                            std::string* out) {
    auto chunked = tbl.column(col);
    int64_t r = row;
    for (const auto& ch : chunked->chunks()) {
        if (r < ch->length()) {
            const arrow::Array* a; int64_t i;
            if (!resolve_dict_cell(*ch, r, &a, &i)) return false;
            if (a->type_id() == arrow::Type::STRING)
                { *out = static_cast<const arrow::StringArray&>(*a).GetString(i); return true; }
            if (a->type_id() == arrow::Type::LARGE_STRING)
                { *out = static_cast<const arrow::LargeStringArray&>(*a).GetString(i); return true; }
            return false;
        }
        r -= ch->length();
    }
    return false;
}

// Translate `a.col_idx` (a schema-level source-column index) into the
// position of that column inside the projected `tbl` passed to the
// filter. Returns -1 if absent (treated as "no match").
static int filter_col_in_table(const FilterAtom& a,
                                const std::vector<int>& read_indices) {
    for (size_t k = 0; k < read_indices.size(); ++k)
        if (read_indices[k] == a.col_idx) return (int)k;
    return -1;
}

// Compile-once cache for `~` / `!~`. eval_atom runs per row — recompiling the
// pattern for every cell would dominate the scan. thread_local because the Qt
// frontend evaluates filters on a worker thread.
static const std::regex* filter_regex_for(const std::string& pat) {
    thread_local std::map<std::string, std::regex> cache;
    auto it = cache.find(pat);
    if (it == cache.end()) {
        try {
            it = cache.emplace(pat, std::regex(pat, std::regex::ECMAScript)).first;
        } catch (const std::regex_error&) {
            return nullptr;   // rejected at parse time; belt and braces
        }
    }
    return &it->second;
}

bool eval_atom(const arrow::Table& tbl, int64_t row, const FilterAtom& a,
                const std::vector<int>& read_indices) {
    int tcol = filter_col_in_table(a, read_indices);
    if (tcol < 0) return false;

    // Null predicates come first: every other branch treats a null as "no
    // match", which is right for them and wrong here.
    if (a.op == FilterAtom::IsNull)  return  cell_is_null(tbl, tcol, row);
    if (a.op == FilterAtom::NotNull) {
        int64_t off = 0;
        const arrow::Array* arr = locate_cell(tbl, tcol, row, &off);
        return arr && !arr->IsNull(off);
    }

    if (a.op == FilterAtom::Has || a.op == FilterAtom::Lacks) {
        int64_t v;
        if (!cell_as_int(tbl, tcol, row, &v)) return false;   // null: no match
        return a.op == FilterAtom::Has ? (v & a.i_lit) == a.i_lit
                                       : (v & a.i_lit) == 0;
    }

    // String / set predicates read the cell as text whatever the literal
    // looked like, so `Chr in (1,2)` works on a string chrom column.
    switch (a.op) {
        case FilterAtom::Match:
        case FilterAtom::NotMatch: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            const std::regex* re = filter_regex_for(a.s_lit);
            if (!re) return false;
            bool hit = std::regex_search(s, *re);
            return a.op == FilterAtom::Match ? hit : !hit;
        }
        case FilterAtom::Contains:
        case FilterAtom::NotContains: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            bool hit = s.find(a.s_lit) != std::string::npos;
            return a.op == FilterAtom::Contains ? hit : !hit;
        }
        case FilterAtom::StartsWith: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            return s.rfind(a.s_lit, 0) == 0;
        }
        case FilterAtom::EndsWith: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) return false;
            return s.size() >= a.s_lit.size() &&
                   s.compare(s.size() - a.s_lit.size(), a.s_lit.size(),
                             a.s_lit) == 0;
        }
        case FilterAtom::In:
        case FilterAtom::NotIn: {
            std::string s;
            if (!cell_as_string(tbl, tcol, row, &s)) {
                // Numeric column: compare as numbers, so `Start in (100, 200)`
                // behaves as written.
                double d;
                if (!cell_as_double(tbl, tcol, row, &d)) return false;
                const bool hit = a.set_num && a.set_num->count(d);
                return a.op == FilterAtom::In ? hit : !hit;
            }
            const bool hit = a.set_text && a.set_text->count(s);
            return a.op == FilterAtom::In ? hit : !hit;
        }
        default: break;   // fall through to the ordering comparisons
    }

    if (a.kind == FilterAtom::K_String) {
        std::string s;
        if (!cell_as_string(tbl, tcol, row, &s)) return false;
        int c = s.compare(a.s_lit);
        switch (a.op) {
            case FilterAtom::Eq: return c == 0;
            case FilterAtom::Ne: return c != 0;
            case FilterAtom::Lt: return c <  0;
            case FilterAtom::Le: return c <= 0;
            case FilterAtom::Gt: return c >  0;
            case FilterAtom::Ge: return c >= 0;
            default: break;   // string/set/null ops handled above
        }
    } else if (a.kind == FilterAtom::K_Int) {
        int64_t v;
        if (cell_as_int(tbl, tcol, row, &v)) {
            switch (a.op) {
                case FilterAtom::Eq: return v == a.i_lit;
                case FilterAtom::Ne: return v != a.i_lit;
                case FilterAtom::Lt: return v <  a.i_lit;
                case FilterAtom::Le: return v <= a.i_lit;
                case FilterAtom::Gt: return v >  a.i_lit;
                case FilterAtom::Ge: return v >= a.i_lit;
                default: break;   // handled above
            }
        }
        // Fall back to double if the column isn't integral.
        double d;
        if (!cell_as_double(tbl, tcol, row, &d)) return false;
        double L = (double)a.i_lit;
        if (tbl.column(tcol)->type()->id() == arrow::Type::FLOAT) L = (float)L;
        switch (a.op) {
            case FilterAtom::Eq: return d == L;
            case FilterAtom::Ne: return d != L;
            case FilterAtom::Lt: return d <  L;
            case FilterAtom::Le: return d <= L;
            case FilterAtom::Gt: return d >  L;
            case FilterAtom::Ge: return d >= L;
            default: break;   // handled above
        }
    } else {
        double d;
        if (!cell_as_double(tbl, tcol, row, &d)) return false;
        // A float32 cell is compared with the literal rounded to float32:
        // the column holds 0.05f (0.0500000007…), which `Score == 0.05`
        // never equalled and `Score > 0.05` wrongly matched.
        double L = a.f_lit;
        if (tbl.column(tcol)->type()->id() == arrow::Type::FLOAT) L = (float)L;
        switch (a.op) {
            case FilterAtom::Eq: return d == L;
            case FilterAtom::Ne: return d != L;
            case FilterAtom::Lt: return d <  L;
            case FilterAtom::Le: return d <= L;
            case FilterAtom::Gt: return d >  L;
            case FilterAtom::Ge: return d >= L;
            default: break;   // handled above
        }
    }
    return false;
}

// Apply `expr` to `tbl`, returning the subset of rows that match.
// Builds contiguous matching runs and concatenates them — avoids Arrow's
// compute kernels (which get GC'd from our static build).
std::shared_ptr<arrow::Table> apply_filter(
 const std::shared_ptr<arrow::Table>& tbl, const FilterExpr& expr,
 const std::vector<int>& read_indices) {
    int64_t n = tbl->num_rows();
    std::vector<std::shared_ptr<arrow::Table>> runs;
    int64_t run_start = -1;
    auto flush = [&](int64_t end) {
        if (run_start >= 0) {
            runs.push_back(tbl->Slice(run_start, end - run_start));
            run_start = -1;
        }
    };
    for (int64_t r = 0; r < n; ++r) {
        bool any = false;
        for (const auto& clause : expr.groups) {
            bool all = true;
            for (const auto& a : clause) {
                if (!eval_atom(*tbl, r, a, read_indices)) { all = false; break; }
            }
            if (all) { any = true; break; }
        }
        if (any) { if (run_start < 0) run_start = r; }
        else     { flush(r); }
    }
    flush(n);
    if (runs.empty())
        return tbl->Slice(0, 0);
    if (runs.size() == 1) return runs[0];
    auto cr = arrow::ConcatenateTables(runs);
    return cr.ok() ? cr.ValueOrDie() : tbl;
}

// Field indices the filter expression references, union'd with `base`.
// Used to widen the read-projection so the filter has the cells it needs.
std::vector<int> union_with_filter(
 const std::vector<int>& base, const FilterExpr& expr) {
    std::set<int> s(base.begin(), base.end());
    for (auto& g : expr.groups) for (auto& a : g) s.insert(a.col_idx);
    return std::vector<int>(s.begin(), s.end());
}

// Evaluate a parsed filter over the entire source, returning the source row
// indices that match, in source order. Drains streaming sources and reads
// every chunk with all columns so any referenced column is present. The GUI
// uses this to build a filtered view; CLI export paths use apply_filter on
// the already-projected table instead.
std::vector<int64_t> filter_rows(TabularSource& src, const FilterExpr& expr) {
    std::vector<int64_t> keep;
    int nf = src.schema()->num_fields();
    std::vector<int> all((size_t)nf);
    for (int i = 0; i < nf; ++i) all[(size_t)i] = i;
    src.set_retain_all(true);            // re-reads every chunk after draining
    while (true) {                       // drain streaming sources
        int n = src.num_chunks();
        src.ensure(n);
        if (src.num_chunks() == n) break;
    }
    for (int c = 0; c < src.num_chunks(); ++c) {
        ChunkMeta m = src.chunk_meta(c);
        std::shared_ptr<arrow::Table> tbl;
        if (!src.read_chunk(c, all, &tbl).ok() || !tbl) continue;
        int64_t n = tbl->num_rows();
        for (int64_t r = 0; r < n; ++r) {
            bool any = false;
            for (const auto& clause : expr.groups) {
                bool good = true;
                for (const auto& a : clause)
                    if (!eval_atom(*tbl, r, a, all)) { good = false; break; }
                if (good) { any = true; break; }
            }
            if (any) keep.push_back(m.first_row + r);
        }
    }
    return keep;
}

// Project `tbl` (which may contain extra columns loaded for the filter) down
// to the user's requested columns, in `requested_indices`-source-order.
std::shared_ptr<arrow::Table> project_to_requested(
 const std::shared_ptr<arrow::Table>& tbl,
 const std::vector<int>& read_indices,
 const std::vector<int>& requested) {
    if (read_indices == requested) return tbl;
    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    arrow::FieldVector fields;
    for (int r : requested) {
        for (size_t k = 0; k < read_indices.size(); ++k) {
            if (read_indices[k] == r) {
                cols.push_back(tbl->column((int)k));
                fields.push_back(tbl->schema()->field((int)k));
                break;
            }
        }
    }
    return arrow::Table::Make(arrow::schema(fields), cols, tbl->num_rows());
}

// Apply --regions-file (read BED chrom/start/end, append to cfg.region) and
// --slop N (pad every window by N bp on each side). Mutates `cfg` to reflect
// the effective region list in cfg.region. Returns "" on success.
// Declared in vvcore.hpp (external linkage) so GUI frontends can offer region
// queries; parse_region_list / Region stay internal to this TU.
std::string apply_region_modifiers(Config& cfg) {
    // Reject a malformed -r / --region up front. parse_region_list silently
    // drops a token it can't parse, which would turn an invalid region (e.g.
    // "chr1:-5-10" or "chr1:5x") into a whole-file query — surface it instead.
    if (!cfg.region.empty()) {
        size_t pos = 0;
        while (pos <= cfg.region.size()) {
            size_t comma = cfg.region.find(',', pos);
            std::string tok = cfg.region.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) {
                Region r{};
                if (!parse_region_one(tok, &r, cfg.coords_one_based))
                    return "Invalid region '" + tok +
                           "' (expected chrom[:start[-end]])";
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }

    // 0) Canonicalise to UCSC (0-based half-open). --coords NCBI applies only
    // to -r / --region inputs; --regions-file entries are always BED (UCSC)
    // per the spec. After this, cfg.region is guaranteed UCSC convention
    // and downstream call sites can ignore cfg.coords_one_based.
    if (cfg.coords_one_based && !cfg.region.empty()) {
        auto regs = parse_region_list(cfg.region, /*one_based=*/true);
        std::string acc;
        for (auto& r : regs) {
            if (!acc.empty()) acc += ",";
            acc += r.chrom + ":";
            if (r.start != INT64_MIN) acc += std::to_string(r.start);
            acc += "-";
            if (r.end != INT64_MAX) acc += std::to_string(r.end);
        }
        cfg.region = acc;
    }
    cfg.coords_one_based = false;

    // 1) Read --regions-file and append its windows.
    if (!cfg.regions_file.empty()) {
        std::ifstream f(cfg.regions_file);
        if (!f.is_open())
            return "Cannot open --regions-file '" + cfg.regions_file + "'";
        std::string line;
        std::string acc = cfg.region;
        while (std::getline(f, line)) {
            // strip CR/LF and trailing whitespace
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            // skip blank lines and BED preamble
            if (line.empty()) continue;
            if (line[0] == '#') continue;
            if (line.rfind("track", 0) == 0 || line.rfind("browser", 0) == 0) continue;
            // First three TSV fields: chrom, start, end.
            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            size_t t3 = line.find('\t', t2 + 1);
            std::string chrom = line.substr(0, t1);
            std::string s     = line.substr(t1 + 1, t2 - t1 - 1);
            std::string e     = (t3 == std::string::npos)
                                ? line.substr(t2 + 1)
                                : line.substr(t2 + 1, t3 - t2 - 1);
            if (chrom.empty() || s.empty() || e.empty()) continue;
            if (!acc.empty()) acc += ",";
            acc += chrom + ":" + s + "-" + e;
        }
        cfg.region = acc;
    }

    // 2) Apply --slop by re-serialising the parsed region list.
    if (cfg.slop != 0 && !cfg.region.empty()) {
        auto regs = parse_region_list(cfg.region, cfg.coords_one_based);
        std::string acc;
        for (auto& r : regs) {
            // Don't expand open bounds — INT64_MIN/MAX stay sentinels.
            if (r.start != INT64_MIN) r.start = std::max<int64_t>(0, r.start - cfg.slop);
            if (r.end   != INT64_MAX) r.end   = r.end + cfg.slop;
            if (!acc.empty()) acc += ",";
            acc += r.chrom + ":";
            if (r.start != INT64_MIN) acc += std::to_string(r.start);
            acc += "-";
            if (r.end != INT64_MAX) acc += std::to_string(r.end);
        }
        cfg.region = acc;
    }
    return "";
}
