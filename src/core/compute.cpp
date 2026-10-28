// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// --compute 'name=expr': row-wise derived columns.

#include "internal.hpp"

#include <cmath>

// ── Expressions ──────────────────────────────────────────────────────────────
//
// Grammar:
//   spec   := NAME '=' expr
//   expr   := term  (('+' | '-') term)*
//   term   := unary (('*' | '/') unary)*
//   unary  := '-' unary | atom
//   atom   := NUMBER | column | FUNC '(' expr (',' expr)* ')' | '(' expr ')'
//   column := IDENT | `any name`          (`` inside backticks: one backtick)
//   FUNC   := len abs log10 log2 round min max
//
// IDENT is a letter or '_' followed by letters, digits, '_' and '.', so a
// flattened `st.a` is a bare name and `End-Start` is a subtraction.
//
// A result is int64 when every operand is an integer (integer, date / time
// and boolean columns, integer literals) and no '/', log or float operand
// takes part; else float64. Null in, null out; a division by zero, the log
// of a number <= 0, an int64 overflow and a non-finite float are null.
// Evaluated per chunk over plain vectors (no arrow::compute: its kernels are
// dropped by --gc-sections in the static build).

namespace {

enum class Op { Num, Col, Neg, Add, Sub, Mul, Div, Len, Abs, Log10, Log2, Round, Min, Max };

struct Node {
    Op op = Op::Num;
    bool is_int = false;
    int64_t ival = 0;
    double dval = 0;
    std::string name;        // Col / Len: the column as written
    int col = -1;            // Col / Len: resolved schema index
    int digits = 0;          // Round
    std::vector<std::unique_ptr<Node>> kids;
};

struct Tok {
    enum Kind { End, Num, Ident, Quoted, Punct } kind = End;
    std::string text;
    size_t pos = 0;
};

class Parser {
    const std::string& s_;
    size_t i_ = 0;
    Tok tok_;
    std::string err_;

    void fail(const std::string& msg, size_t pos) {
        if (!err_.empty()) return;
        err_ = msg + "\n  " + s_ + "\n  " + std::string(pos, ' ') + "^";
    }
    void next() {
        while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) ++i_;
        tok_ = Tok{};
        tok_.pos = i_;
        if (i_ >= s_.size()) return;
        const char c = s_[i_];
        if (std::isdigit((unsigned char)c) || (c == '.' && i_ + 1 < s_.size() &&
                                               std::isdigit((unsigned char)s_[i_ + 1]))) {
            size_t j = i_;
            while (j < s_.size() && (std::isdigit((unsigned char)s_[j]) || s_[j] == '.')) ++j;
            if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
                size_t k = j + 1;
                if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
                if (k < s_.size() && std::isdigit((unsigned char)s_[k])) {
                    j = k;
                    while (j < s_.size() && std::isdigit((unsigned char)s_[j])) ++j;
                }
            }
            tok_.kind = Tok::Num;
            tok_.text = s_.substr(i_, j - i_);
            i_ = j;
            return;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i_;
            while (j < s_.size() && (std::isalnum((unsigned char)s_[j]) || s_[j] == '_' || s_[j] == '.')) ++j;
            tok_.kind = Tok::Ident;
            tok_.text = s_.substr(i_, j - i_);
            i_ = j;
            return;
        }
        if (c == '`') {
            std::string name;
            for (size_t j = i_ + 1; j < s_.size(); ++j) {
                if (s_[j] != '`') { name += s_[j]; continue; }
                if (j + 1 < s_.size() && s_[j + 1] == '`') { name += '`'; ++j; continue; }
                tok_.kind = Tok::Quoted;
                tok_.text = name;
                i_ = j + 1;
                return;
            }
            fail("unterminated `name`", i_);
            i_ = s_.size();
            return;
        }
        tok_.kind = Tok::Punct;
        tok_.text = std::string(1, c);
        ++i_;
    }
    bool punct(char c) const { return tok_.kind == Tok::Punct && tok_.text[0] == c; }
    void expect(char c) {
        if (punct(c)) { next(); return; }
        fail(std::string("expected '") + c + "'", tok_.pos);
    }

    std::unique_ptr<Node> binary(Op op, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
        auto n = std::make_unique<Node>();
        n->op = op;
        n->kids.push_back(std::move(a));
        n->kids.push_back(std::move(b));
        return n;
    }
    std::unique_ptr<Node> expr() {
        auto a = term();
        while (err_.empty() && (punct('+') || punct('-'))) {
            const Op op = punct('+') ? Op::Add : Op::Sub;
            next();
            a = binary(op, std::move(a), term());
        }
        return a;
    }
    std::unique_ptr<Node> term() {
        auto a = unary();
        while (err_.empty() && (punct('*') || punct('/'))) {
            const Op op = punct('*') ? Op::Mul : Op::Div;
            next();
            a = binary(op, std::move(a), unary());
        }
        return a;
    }
    std::unique_ptr<Node> unary() {
        if (punct('-')) {
            next();
            auto n = std::make_unique<Node>();
            n->op = Op::Neg;
            n->kids.push_back(unary());
            return n;
        }
        return atom();
    }
    std::unique_ptr<Node> atom() {
        auto n = std::make_unique<Node>();
        const Tok t = tok_;
        if (t.kind == Tok::Num) {
            next();
            const bool is_float = t.text.find_first_of(".eE") != std::string::npos;
            char* end = nullptr;
            errno = 0;
            if (is_float) n->dval = std::strtod(t.text.c_str(), &end);
            else          n->ival = std::strtoll(t.text.c_str(), &end, 10);
            if (!end || *end || errno == ERANGE) fail("bad number '" + t.text + "'", t.pos);
            n->op = Op::Num;
            n->is_int = !is_float;
            if (n->is_int) n->dval = (double)n->ival;
            return n;
        }
        if (t.kind == Tok::Quoted) {
            next();
            n->op = Op::Col;
            n->name = t.text;
            return n;
        }
        if (t.kind == Tok::Ident) {
            next();
            if (!punct('(')) {
                n->op = Op::Col;
                n->name = t.text;
                return n;
            }
            static const std::pair<const char*, Op> kFuncs[] = {
                {"len", Op::Len}, {"abs", Op::Abs}, {"log10", Op::Log10}, {"log2", Op::Log2},
                {"round", Op::Round}, {"min", Op::Min}, {"max", Op::Max}};
            bool known = false;
            for (const auto& [name, op] : kFuncs)
                if (t.text == name) { n->op = op; known = true; }
            if (!known) {
                fail("unknown function '" + t.text + "' (len, abs, log10, log2, round, min, max)", t.pos);
                return n;
            }
            next();                                     // '('
            if (n->op == Op::Len) {
                // len() takes a column: its text or list length.
                if (tok_.kind != Tok::Ident && tok_.kind != Tok::Quoted) {
                    fail("len() takes a column name", tok_.pos);
                    return n;
                }
                n->name = tok_.text;
                next();
                expect(')');
                return n;
            }
            n->kids.push_back(expr());
            while (err_.empty() && punct(',')) {
                next();
                if (n->op == Op::Round && n->kids.size() == 1) {
                    // round(x, d): d an integer literal, possibly negative.
                    bool neg = false;
                    if (punct('-')) { neg = true; next(); }
                    if (tok_.kind != Tok::Num || tok_.text.find_first_of(".eE") != std::string::npos) {
                        fail("round()'s digits must be a whole number", tok_.pos);
                        return n;
                    }
                    n->digits = std::atoi(tok_.text.c_str()) * (neg ? -1 : 1);
                    next();
                    if (punct(',')) { fail("round() takes one or two arguments", tok_.pos); return n; }
                    continue;
                }
                n->kids.push_back(expr());
            }
            expect(')');
            const size_t k = n->kids.size();
            if ((n->op == Op::Min || n->op == Op::Max) && k < 2)
                fail(t.text + "() takes two or more arguments", t.pos);
            if ((n->op == Op::Abs || n->op == Op::Log10 || n->op == Op::Log2) && k != 1)
                fail(t.text + "() takes one argument", t.pos);
            return n;
        }
        if (punct('(')) {
            next();
            n = expr();
            expect(')');
            return n;
        }
        fail(t.kind == Tok::End ? "expression ends early" : "unexpected '" + t.text + "'", t.pos);
        return n;
    }

public:
    explicit Parser(const std::string& s) : s_(s) { next(); }

    // NAME '=' expr; false with *err set on a syntax error.
    bool spec(std::string* name, std::unique_ptr<Node>* root, std::string* err) {
        if (tok_.kind != Tok::Ident && tok_.kind != Tok::Quoted) {
            fail("expected 'name=expression'", tok_.pos);
        } else {
            *name = tok_.text;
            next();
            if (!punct('=')) fail("expected '=' after the new column's name", tok_.pos);
            else {
                next();
                *root = expr();
                if (err_.empty() && tok_.kind != Tok::End)
                    fail("unexpected '" + tok_.text + "'", tok_.pos);
            }
        }
        *err = err_;
        return err_.empty();
    }
};

void collect_names(const Node& n, std::vector<std::string>* out) {
    if (n.op == Op::Col || n.op == Op::Len) out->push_back(n.name);
    for (const auto& k : n.kids) collect_names(*k, out);
}

bool int_type(arrow::Type::type id) {
    switch (id) {
        case arrow::Type::INT8: case arrow::Type::INT16: case arrow::Type::INT32: case arrow::Type::INT64:
        case arrow::Type::UINT8: case arrow::Type::UINT16: case arrow::Type::UINT32: case arrow::Type::UINT64:
        case arrow::Type::DATE32: case arrow::Type::DATE64: case arrow::Type::TIMESTAMP:
        case arrow::Type::TIME32: case arrow::Type::TIME64: case arrow::Type::DURATION:
        case arrow::Type::BOOL:
            return true;
        default:
            return false;
    }
}
bool float_type(arrow::Type::type id) {
    return id == arrow::Type::HALF_FLOAT || id == arrow::Type::FLOAT || id == arrow::Type::DOUBLE ||
           id == arrow::Type::DECIMAL128 || id == arrow::Type::DECIMAL256;
}
bool len_type(arrow::Type::type id) {
    switch (id) {
        case arrow::Type::STRING: case arrow::Type::LARGE_STRING: case arrow::Type::STRING_VIEW:
        case arrow::Type::BINARY: case arrow::Type::LARGE_BINARY: case arrow::Type::FIXED_SIZE_BINARY:
        case arrow::Type::LIST: case arrow::Type::LARGE_LIST: case arrow::Type::FIXED_SIZE_LIST:
        case arrow::Type::MAP:
            return true;
        default:
            return false;
    }
}
arrow::Type::type value_type_id(const arrow::DataType& t) {
    if (t.id() == arrow::Type::DICTIONARY)
        return static_cast<const arrow::DictionaryType&>(t).value_type()->id();
    return t.id();
}

// Resolve column names against `schema` and set every node's result type.
bool resolve(Node& n, const arrow::Schema& schema, std::string* err) {
    for (auto& k : n.kids)
        if (!resolve(*k, schema, err)) return false;
    auto all_int = [&] {
        for (const auto& k : n.kids) if (!k->is_int) return false;
        return true;
    };
    switch (n.op) {
        case Op::Num: return true;
        case Op::Col:
        case Op::Len: {
            n.col = schema.GetFieldIndex(n.name);
            if (n.col < 0) {
                *err = "unknown column '" + n.name + "'";
                const std::string near = closest_column_name(schema, n.name);
                if (!near.empty()) *err += " (did you mean '" + near + "'?)";
                return false;
            }
            const auto& type = *schema.field(n.col)->type();
            const auto id = value_type_id(type);
            if (n.op == Op::Len) {
                if (!len_type(id)) {
                    *err = "len(" + n.name + "): the column is " + type_label(type) +
                           ", not text or a list";
                    return false;
                }
                n.is_int = true;
                return true;
            }
            if (!int_type(id) && !float_type(id)) {
                *err = "column '" + n.name + "' is " + type_label(type) +
                       ", not a number" + (len_type(id) ? " (len(" + n.name + ") is its length)" : "");
                return false;
            }
            n.is_int = int_type(id);
            return true;
        }
        case Op::Neg: case Op::Abs: n.is_int = n.kids[0]->is_int; return true;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Min: case Op::Max:
            n.is_int = all_int(); return true;
        case Op::Round: n.is_int = n.kids[0]->is_int; return true;
        case Op::Div: case Op::Log10: case Op::Log2: n.is_int = false; return true;
    }
    return true;
}

// One node's values over a chunk: ints or doubles, with a validity byte.
struct Vec {
    bool is_int = false;
    std::vector<int64_t> i;
    std::vector<double>  d;
    std::vector<uint8_t> ok;
};

// A cell's length: text in code points, binary in bytes, a list in elements.
bool cell_length(const arrow::Array& a, int64_t r, int64_t* out) {
    if (a.IsNull(r)) return false;
    switch (a.type_id()) {
        case arrow::Type::DICTIONARY: {
            const auto& d = static_cast<const arrow::DictionaryArray&>(a);
            return cell_length(*d.dictionary(), d.GetValueIndex(r), out);
        }
        case arrow::Type::STRING: case arrow::Type::LARGE_STRING: case arrow::Type::STRING_VIEW: {
            const std::string_view v =
                a.type_id() == arrow::Type::STRING ? static_cast<const arrow::StringArray&>(a).GetView(r)
                : a.type_id() == arrow::Type::LARGE_STRING
                    ? static_cast<const arrow::LargeStringArray&>(a).GetView(r)
                    : static_cast<const arrow::StringViewArray&>(a).GetView(r);
            int64_t n = 0;
            for (unsigned char c : v) n += (c & 0xC0) != 0x80;   // code points
            *out = n;
            return true;
        }
        default: break;
    }
    switch (a.type_id()) {
        case arrow::Type::BINARY:       *out = static_cast<const arrow::BinaryArray&>(a).value_length(r); return true;
        case arrow::Type::LARGE_BINARY: *out = static_cast<const arrow::LargeBinaryArray&>(a).value_length(r); return true;
        case arrow::Type::FIXED_SIZE_BINARY:
            *out = static_cast<const arrow::FixedSizeBinaryArray&>(a).byte_width(); return true;
        case arrow::Type::LIST:  *out = static_cast<const arrow::ListArray&>(a).value_length(r); return true;
        case arrow::Type::MAP:   *out = static_cast<const arrow::MapArray&>(a).value_length(r); return true;
        case arrow::Type::LARGE_LIST: *out = static_cast<const arrow::LargeListArray&>(a).value_length(r); return true;
        case arrow::Type::FIXED_SIZE_LIST:
            *out = static_cast<const arrow::FixedSizeListArray&>(a).value_length(r); return true;
        default: return false;
    }
}

}  // namespace

// ── ComputeSource ────────────────────────────────────────────────────────────
//
// A decorator that appends one column per --compute spec. A spec may use the
// columns before it, computed ones included. Chunks, row counts and every
// other accessor forward to the inner source.
class ComputeSource : public TabularSource {
    struct Spec {
        std::string name;
        std::unique_ptr<Node> root;
        std::vector<int> refs;   // the schema columns it reads
    };
    std::unique_ptr<TabularSource> inner_;
    std::vector<Spec>              specs_;
    std::shared_ptr<arrow::Schema> schema_;
    int                            n_inner_ = 0;

    static void refs_of(const Node& n, std::vector<int>* out) {
        if (n.col >= 0 && std::find(out->begin(), out->end(), n.col) == out->end()) out->push_back(n.col);
        for (const auto& k : n.kids) refs_of(*k, out);
    }

    // `cols` holds the chunk's schema columns by index (nullptr: not read).
    void eval(const Node& n, const std::vector<std::shared_ptr<arrow::ChunkedArray>>& cols,
              int64_t rows, Vec* v) const {
        v->is_int = n.is_int;
        v->ok.assign((size_t)rows, 1);
        if (n.is_int) v->i.assign((size_t)rows, 0);
        else          v->d.assign((size_t)rows, 0.0);
        auto finite = [&](size_t r) { if (!std::isfinite(v->d[r])) v->ok[r] = 0; };
        switch (n.op) {
            case Op::Num:
                if (n.is_int) std::fill(v->i.begin(), v->i.end(), n.ival);
                else          std::fill(v->d.begin(), v->d.end(), n.dval);
                return;
            case Op::Col:
            case Op::Len: {
                size_t r = 0;
                for (const auto& ch : cols[n.col]->chunks()) {
                    for (int64_t k = 0; k < ch->length(); ++k, ++r) {
                        if (n.op == Op::Len) v->ok[r] = cell_length(*ch, k, &v->i[r]);
                        else if (n.is_int)   v->ok[r] = array_value_as_int64(*ch, k, &v->i[r]);
                        else                 v->ok[r] = array_value_as_double(*ch, k, &v->d[r]);
                    }
                }
                return;
            }
            default: break;
        }
        std::vector<Vec> k(n.kids.size());
        for (size_t j = 0; j < n.kids.size(); ++j) eval(*n.kids[j], cols, rows, &k[j]);
        // An operand as a double (an int operand of a float node).
        auto dbl = [&](const Vec& x, size_t r) { return x.is_int ? (double)x.i[r] : x.d[r]; };
        for (size_t r = 0; r < (size_t)rows; ++r) {
            bool ok = true;
            for (const auto& x : k) ok = ok && x.ok[r];
            if (!ok) { v->ok[r] = 0; continue; }
            switch (n.op) {
                case Op::Neg:
                    if (n.is_int) v->ok[r] = !__builtin_sub_overflow((int64_t)0, k[0].i[r], &v->i[r]);
                    else          v->d[r] = -k[0].d[r];
                    break;
                case Op::Abs:
                    if (n.is_int) {
                        v->ok[r] = k[0].i[r] != INT64_MIN;
                        v->i[r] = v->ok[r] && k[0].i[r] < 0 ? -k[0].i[r] : k[0].i[r];
                    } else {
                        v->d[r] = std::fabs(k[0].d[r]);
                    }
                    break;
                case Op::Add:
                    if (n.is_int) v->ok[r] = !__builtin_add_overflow(k[0].i[r], k[1].i[r], &v->i[r]);
                    else { v->d[r] = dbl(k[0], r) + dbl(k[1], r); finite(r); }
                    break;
                case Op::Sub:
                    if (n.is_int) v->ok[r] = !__builtin_sub_overflow(k[0].i[r], k[1].i[r], &v->i[r]);
                    else { v->d[r] = dbl(k[0], r) - dbl(k[1], r); finite(r); }
                    break;
                case Op::Mul:
                    if (n.is_int) v->ok[r] = !__builtin_mul_overflow(k[0].i[r], k[1].i[r], &v->i[r]);
                    else { v->d[r] = dbl(k[0], r) * dbl(k[1], r); finite(r); }
                    break;
                case Op::Div:
                    v->d[r] = dbl(k[0], r) / dbl(k[1], r);
                    finite(r);
                    break;
                case Op::Log10:
                case Op::Log2: {
                    const double x = dbl(k[0], r);
                    if (x <= 0) { v->ok[r] = 0; break; }
                    v->d[r] = n.op == Op::Log10 ? std::log10(x) : std::log2(x);
                    finite(r);
                    break;
                }
                case Op::Round:
                    if (n.is_int) {
                        // round(int, d < 0): to tens, hundreds, … (half away from zero).
                        int64_t x = k[0].i[r];
                        if (n.digits < 0) {
                            int64_t p = 1;
                            for (int e = 0; e < -n.digits && p <= INT64_MAX / 10; ++e) p *= 10;
                            const int64_t q = x / p, rem = x % p;
                            x = q * p;
                            if (2 * (rem < 0 ? -rem : rem) >= p) x += rem < 0 ? -p : p;
                        }
                        v->i[r] = x;
                    } else {
                        const double p = std::pow(10.0, n.digits);
                        v->d[r] = std::round(k[0].d[r] * p) / p;
                        finite(r);
                    }
                    break;
                case Op::Min:
                case Op::Max:
                    if (n.is_int) {
                        int64_t m = k[0].i[r];
                        for (size_t j = 1; j < k.size(); ++j)
                            m = n.op == Op::Min ? std::min(m, k[j].i[r]) : std::max(m, k[j].i[r]);
                        v->i[r] = m;
                    } else {
                        double m = dbl(k[0], r);
                        for (size_t j = 1; j < k.size(); ++j)
                            m = n.op == Op::Min ? std::min(m, dbl(k[j], r)) : std::max(m, dbl(k[j], r));
                        v->d[r] = m;
                    }
                    break;
                default: break;
            }
        }
    }

    arrow::Status to_array(const Vec& v, std::shared_ptr<arrow::Array>* out) const {
        const int64_t n = (int64_t)v.ok.size();
        if (v.is_int) {
            arrow::Int64Builder b;
            ARROW_RETURN_NOT_OK(b.AppendValues(v.i.data(), n, v.ok.data()));
            return b.Finish(out);
        }
        arrow::DoubleBuilder b;
        ARROW_RETURN_NOT_OK(b.AppendValues(v.d.data(), n, v.ok.data()));
        return b.Finish(out);
    }

public:
    static std::string open(std::unique_ptr<TabularSource> inner, const std::vector<std::string>& texts,
                            std::unique_ptr<TabularSource>* out) {
        auto self = std::unique_ptr<ComputeSource>(new ComputeSource());
        auto in_schema = inner->schema();
        self->n_inner_ = in_schema->num_fields();
        arrow::FieldVector fields = in_schema->fields();
        for (const std::string& text : texts) {
            Spec sp;
            std::string err;
            if (!Parser(text).spec(&sp.name, &sp.root, &err)) return "--compute: " + err;
            auto sofar = arrow::schema(fields);
            if (sofar->GetFieldIndex(sp.name) >= 0 || !sofar->GetAllFieldsByName(sp.name).empty())
                return "--compute: a column named '" + sp.name + "' exists already";
            if (!resolve(*sp.root, *sofar, &err)) return "--compute " + sp.name + ": " + err;
            refs_of(*sp.root, &sp.refs);
            fields.push_back(arrow::field(sp.name, sp.root->is_int ? arrow::int64() : arrow::float64()));
            self->specs_.push_back(std::move(sp));
        }
        self->schema_ = arrow::schema(fields);
        self->inner_ = std::move(inner);
        *out = std::move(self);
        return "";
    }

    const TabularSource& inner() const { return *inner_; }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

    arrow::Status read_chunk(int i, const std::vector<int>& col_indices,
                             std::shared_ptr<arrow::Table>* out) override {
        return read(i, -1, col_indices, out);
    }
    arrow::Status read_first(int64_t rows, const std::vector<int>& cols,
                             std::shared_ptr<arrow::Table>* out) override {
        return read(0, rows, cols, out);
    }
    arrow::Status read(int i, int64_t rows, const std::vector<int>& col_indices,
                       std::shared_ptr<arrow::Table>* out) {
        // The specs the request needs, with the computed columns they read.
        std::vector<bool> need(specs_.size(), false);
        for (int c : col_indices)
            if (c >= n_inner_) need[(size_t)(c - n_inner_)] = true;
        for (size_t s = specs_.size(); s-- > 0;)
            if (need[s])
                for (int c : specs_[s].refs)
                    if (c >= n_inner_) need[(size_t)(c - n_inner_)] = true;
        std::vector<int> inner_req;
        auto add = [&](int c) {
            if (c < n_inner_ && std::find(inner_req.begin(), inner_req.end(), c) == inner_req.end())
                inner_req.push_back(c);
        };
        for (int c : col_indices) add(c);
        for (size_t s = 0; s < specs_.size(); ++s)
            if (need[s]) for (int c : specs_[s].refs) add(c);

        std::shared_ptr<arrow::Table> in_tbl;
        ARROW_RETURN_NOT_OK(rows >= 0 ? inner_->read_first(rows, inner_req, &in_tbl)
                                      : inner_->read_chunk(i, inner_req, &in_tbl));
        if (!in_tbl) { *out = nullptr; return arrow::Status::OK(); }
        const int64_t n = in_tbl->num_rows();

        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols((size_t)schema_->num_fields());
        for (size_t k = 0; k < inner_req.size(); ++k) cols[(size_t)inner_req[k]] = in_tbl->column((int)k);
        for (size_t s = 0; s < specs_.size(); ++s) {
            if (!need[s]) continue;
            Vec v;
            eval(*specs_[s].root, cols, n, &v);
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(to_array(v, &arr));
            cols[(size_t)n_inner_ + s] = std::make_shared<arrow::ChunkedArray>(arr);
        }
        arrow::FieldVector fields;
        std::vector<std::shared_ptr<arrow::ChunkedArray>> picked;
        for (int c : col_indices) {
            fields.push_back(schema_->field(c));
            picked.push_back(cols[(size_t)c]);
        }
        *out = arrow::Table::Make(arrow::schema(fields), picked, n);
        return arrow::Status::OK();
    }

    int64_t total_rows()      const override { return inner_->total_rows(); }
    int     num_chunks()      const override { return inner_->num_chunks(); }
    ChunkMeta chunk_meta(int i) const override { return inner_->chunk_meta(i); }
    void    ensure(int i)           override { inner_->ensure(i); }
    void    set_retain_all(bool b)  override { inner_->set_retain_all(b); }
    bool    random_access() const   override { return inner_->random_access(); }
    bool    evicted_any()     const override { return inner_->evicted_any(); }
    int64_t chunk_bytes(int i) const override { return inner_->chunk_bytes(i); }
    arrow::Status read_status() const override { return inner_->read_status(); }
    bool    region_applied()  const override { return inner_->region_applied(); }
    bool    has_slices()      const override { return inner_->has_slices(); }
    bool    is_matrix_tab()   const override { return inner_->is_matrix_tab(); }
    bool    change_slice(int d, bool abs, int64_t t) override { return inner_->change_slice(d, abs, t); }
    const std::string& path() const override { return inner_->path(); }
    std::string tab_label()   const override { return inner_->tab_label(); }
    std::string created_by()  const override { return inner_->created_by(); }
    std::string top_banner()  const override { return inner_->top_banner(); }
    std::vector<std::string> preamble_above() const override { return inner_->preamble_above(); }
    std::vector<std::string> preamble_below() const override { return inner_->preamble_below(); }
    std::vector<std::string> hidden_for_display() const override { return inner_->hidden_for_display(); }
    bool show_cells_in_full() const override { return inner_->show_cells_in_full(); }
    std::string sequence_alphabet() const override { return inner_->sequence_alphabet(); }
    PreviewLimit preview_limit() const override { return inner_->preview_limit(); }
    std::string format_cell(int col_idx, std::string val) const override {
        return col_idx < n_inner_ ? inner_->format_cell(col_idx, std::move(val)) : val;
    }
    int min_col_width(int col_idx) const override {
        return col_idx < n_inner_ ? inner_->min_col_width(col_idx) : 4;
    }
    std::string footer() const override {
        std::string s = inner_->footer() + "  |  computed: ";
        for (size_t k = 0; k < specs_.size(); ++k) s += (k ? ", " : "") + specs_[k].name;
        return s;
    }
};

std::string open_compute_source(std::unique_ptr<TabularSource> inner, const std::vector<std::string>& specs,
                                std::unique_ptr<TabularSource>* out) {
    return ComputeSource::open(std::move(inner), specs, out);
}

const TabularSource* compute_source_inner(const TabularSource& src) {
    auto* c = dynamic_cast<const ComputeSource*>(&src);
    return c ? &c->inner() : nullptr;
}

bool compute_columns_read(const std::vector<std::string>& specs, std::vector<std::string>* names,
                          std::vector<std::string>* defined_out) {
    std::vector<std::string>& defined = *defined_out;
    for (const std::string& text : specs) {
        std::string name, err;
        std::unique_ptr<Node> root;
        if (!Parser(text).spec(&name, &root, &err)) return false;
        std::vector<std::string> used;
        collect_names(*root, &used);
        for (auto& u : used)
            if (std::find(defined.begin(), defined.end(), u) == defined.end()) names->push_back(u);
        defined.push_back(name);
    }
    return true;
}
