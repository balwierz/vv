// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Cell text for the exporters: cell_to_string()'s text appended to a buffer,
// with the switch on the type done once per column (pick_appender) rather than
// once per cell, and no std::string built per cell. Types without an appender
// here go through cell_to_string itself; for the rest the text must stay
// identical to it, which the export goldens of tiny.alltypes.arrow pin and
// VV_FORMAT_CHECK=1 (writers.cpp) verifies cell by cell.

#include "internal.hpp"

#include <arrow/util/decimal.h>

namespace {

template <typename T>
inline void append_int(std::string& out, T v) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, r.ptr);
}

template <typename ArrT>
void app_int(const arrow::Array& a, int64_t row, std::string& out) {
    append_int(out, static_cast<const ArrT&>(a).Value(row));
}

void app_bool(const arrow::Array& a, int64_t row, std::string& out) {
    out += static_cast<const arrow::BooleanArray&>(a).Value(row) ? "true" : "false";
}

template <typename ArrT, bool kSingle>
void app_float_exact(const arrow::Array& a, int64_t row, std::string& out) {
    char buf[40];
    out.append(buf, exact_float_chars(static_cast<const ArrT&>(a).Value(row), kSingle, buf));
}

template <typename ArrT>
void app_float_g6(const arrow::Array& a, int64_t row, std::string& out) {
    char buf[32];
    const int n = std::snprintf(buf, sizeof buf, "%.6g",
                                (double)static_cast<const ArrT&>(a).Value(row));
    out.append(buf, (size_t)n);
}

template <typename ArrT>
void app_string(const arrow::Array& a, int64_t row, std::string& out) {
    const auto v = static_cast<const ArrT&>(a).GetView(row);
    out.append(v.data(), v.size());
}

template <typename ArrT>
void app_binary(const arrow::Array& a, int64_t row, std::string& out) {
    const auto v = static_cast<const ArrT&>(a).GetView(row);
    append_binary_text(reinterpret_cast<const uint8_t*>(v.data()), (int64_t)v.size(), out);
}

void app_fixed_binary(const arrow::Array& a, int64_t row, std::string& out) {
    auto& fa = static_cast<const arrow::FixedSizeBinaryArray&>(a);
    append_binary_text(fa.GetValue(row), fa.byte_width(), out);
}

void app_dictionary(const arrow::Array& a, int64_t row, std::string& out) {
    auto& da = static_cast<const arrow::DictionaryArray&>(a);
    const int64_t k = da.GetValueIndex(row);
    const auto& dict = da.dictionary();
    if (k >= 0 && k < dict->length()) append_cell(*dict, k, out);
    else out += NULL_SYMBOL;
}

void app_fallback(const arrow::Array& a, int64_t row, std::string& out) {
    out += cell_to_string(a, row);
}

// ── Dates and times, as Arrow's StringFormatter writes them
// (arrow/util/formatting.h), which is what Scalar::ToString and so
// cell_to_string produce for these types.

// Days Arrow formats: from the start of year -32767 to the end of 32767.
constexpr int64_t kMinDay = -12687428, kEndDay = 11248738;

void out_of_range(std::string& out, int64_t v) {
    out += "<value out of range: ";
    append_int(out, v);
    out += '>';
}

inline void append_2(std::string& out, unsigned v) {
    out += char('0' + v / 10);
    out += char('0' + v % 10);
}

// YYYY-MM-DD of a day count since 1970-01-01 (proleptic Gregorian; the year
// has at least four digits and a '-' sign before year 0).
void append_ymd(std::string& out, int64_t days) {
    // civil_from_days (H. Hinnant), the algorithm of the date library Arrow
    // vendors.
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp  = (5 * doy + 2) / 153;
    const unsigned d   = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m   = mp < 10 ? mp + 3 : mp - 9;
    const int64_t  y   = (int64_t)yoe + era * 400 + (m <= 2);
    const uint64_t ay  = y < 0 ? (uint64_t)-y : (uint64_t)y;
    if (y < 0) out += '-';
    if (ay >= 10000) out += char('0' + ay / 10000);
    append_2(out, (unsigned)(ay / 100 % 100));
    append_2(out, (unsigned)(ay % 100));
    out += '-';
    append_2(out, m);
    out += '-';
    append_2(out, d);
}

// HH:MM:SS, then the fraction of a second in the unit's digits.
template <int64_t kPerSec>
void append_hms(std::string& out, int64_t since_midnight) {
    const int64_t s = since_midnight / kPerSec;
    append_2(out, (unsigned)(s / 3600));
    out += ':';
    append_2(out, (unsigned)(s / 60 % 60));
    out += ':';
    append_2(out, (unsigned)(s % 60));
    if constexpr (kPerSec > 1) {
        constexpr int kDigits = kPerSec == 1000 ? 3 : kPerSec == 1000000 ? 6 : 9;
        int64_t sub = since_midnight % kPerSec;
        char buf[9];
        for (int i = kDigits - 1; i >= 0; --i) { buf[i] = char('0' + sub % 10); sub /= 10; }
        out += '.';
        out.append(buf, kDigits);
    }
}

void app_date32(const arrow::Array& a, int64_t row, std::string& out) {
    const int64_t v = static_cast<const arrow::Date32Array&>(a).Value(row);
    if (v < kMinDay || v >= kEndDay) return out_of_range(out, v);
    append_ymd(out, v);
}

// Date64 holds milliseconds; the day is the count truncated toward zero (so a
// pre-1970 instant that is not midnight shows the day after), as Arrow does.
void app_date64(const arrow::Array& a, int64_t row, std::string& out) {
    constexpr int64_t kMsPerDay = 86400000;
    const int64_t v = static_cast<const arrow::Date64Array&>(a).Value(row);
    if (v < kMinDay * kMsPerDay || v >= kEndDay * kMsPerDay) return out_of_range(out, v);
    append_ymd(out, v / kMsPerDay);
}

// "YYYY-MM-DD HH:MM:SS[.fff…]", then "Z" when the type has any time zone: the
// value is shown as stored (UTC), not converted.
template <int64_t kPerSec, bool kZone>
void app_timestamp(const arrow::Array& a, int64_t row, std::string& out) {
    constexpr int64_t kPerDay = 86400 * kPerSec;
    const int64_t v = static_cast<const arrow::TimestampArray&>(a).Value(row);
    if constexpr (kPerSec != 1000000000) {   // a nanosecond count is always in range
        if (v < kMinDay * kPerDay || v >= kEndDay * kPerDay) return out_of_range(out, v);
    }
    int64_t days = v / kPerDay, rem = v % kPerDay;
    if (rem < 0) { rem += kPerDay; --days; }
    append_ymd(out, days);
    out += ' ';
    append_hms<kPerSec>(out, rem);
    if constexpr (kZone) out += 'Z';
}

template <typename ArrT, int64_t kPerSec>
void app_time(const arrow::Array& a, int64_t row, std::string& out) {
    const int64_t v = static_cast<const ArrT&>(a).Value(row);
    if (v < 0 || v >= 86400 * kPerSec) return out_of_range(out, v);
    append_hms<kPerSec>(out, v);
}

void app_decimal128(const arrow::Array& a, int64_t row, std::string& out) {
    auto& da = static_cast<const arrow::Decimal128Array&>(a);
    const int32_t scale = static_cast<const arrow::Decimal128Type&>(*a.type()).scale();
    out += arrow::Decimal128(da.GetValue(row)).ToString(scale);
}

void app_decimal256(const arrow::Array& a, int64_t row, std::string& out) {
    auto& da = static_cast<const arrow::Decimal256Array&>(a);
    const int32_t scale = static_cast<const arrow::Decimal256Type&>(*a.type()).scale();
    out += arrow::Decimal256(da.GetValue(row)).ToString(scale);
}

template <bool kZone>
CellAppender timestamp_appender(arrow::TimeUnit::type unit) {
    switch (unit) {
        case arrow::TimeUnit::SECOND: return app_timestamp<1, kZone>;
        case arrow::TimeUnit::MILLI:  return app_timestamp<1000, kZone>;
        case arrow::TimeUnit::MICRO:  return app_timestamp<1000000, kZone>;
        case arrow::TimeUnit::NANO:   return app_timestamp<1000000000, kZone>;
    }
    return app_fallback;
}

}  // namespace

CellAppender pick_appender(const arrow::DataType& t) {
    using T = arrow::Type;
    switch (t.id()) {
        case T::BOOL:   return app_bool;
        case T::INT8:   return app_int<arrow::Int8Array>;
        case T::INT16:  return app_int<arrow::Int16Array>;
        case T::INT32:  return app_int<arrow::Int32Array>;
        case T::INT64:  return app_int<arrow::Int64Array>;
        case T::UINT8:  return app_int<arrow::UInt8Array>;
        case T::UINT16: return app_int<arrow::UInt16Array>;
        case T::UINT32: return app_int<arrow::UInt32Array>;
        case T::UINT64: return app_int<arrow::UInt64Array>;
        case T::FLOAT:
            return t_exact_floats ? app_float_exact<arrow::FloatArray, true>
                                  : app_float_g6<arrow::FloatArray>;
        case T::DOUBLE:
            return t_exact_floats ? app_float_exact<arrow::DoubleArray, false>
                                  : app_float_g6<arrow::DoubleArray>;
        case T::STRING:       return app_string<arrow::StringArray>;
        case T::LARGE_STRING: return app_string<arrow::LargeStringArray>;
        case T::STRING_VIEW:  return app_string<arrow::StringViewArray>;
        case T::BINARY:       return app_binary<arrow::BinaryArray>;
        case T::LARGE_BINARY: return app_binary<arrow::LargeBinaryArray>;
        case T::BINARY_VIEW:  return app_binary<arrow::BinaryViewArray>;
        case T::FIXED_SIZE_BINARY: return app_fixed_binary;
        case T::DICTIONARY:   return app_dictionary;
        case T::DATE32:       return app_date32;
        case T::DATE64:       return app_date64;
        case T::TIMESTAMP: {
            auto& ts = static_cast<const arrow::TimestampType&>(t);
            return ts.timezone().empty() ? timestamp_appender<false>(ts.unit())
                                         : timestamp_appender<true>(ts.unit());
        }
        case T::TIME32:
            return static_cast<const arrow::Time32Type&>(t).unit() == arrow::TimeUnit::SECOND
                ? app_time<arrow::Time32Array, 1> : app_time<arrow::Time32Array, 1000>;
        case T::TIME64:
            return static_cast<const arrow::Time64Type&>(t).unit() == arrow::TimeUnit::MICRO
                ? app_time<arrow::Time64Array, 1000000> : app_time<arrow::Time64Array, 1000000000>;
        case T::DURATION:     return app_int<arrow::DurationArray>;
        case T::DECIMAL128:   return app_decimal128;
        case T::DECIMAL256:   return app_decimal256;
        default:              return app_fallback;
    }
}

void append_cell(const arrow::Array& arr, int64_t row, std::string& out) {
    if (arr.IsNull(row)) { out += NULL_SYMBOL; return; }
    pick_appender(*arr.type())(arr, row, out);
}

static bool thread_safe_below_top(const arrow::DataType& t) {
    if (t.id() == arrow::Type::DICTIONARY) return false;
    if (t.id() == arrow::Type::EXTENSION)
        return thread_safe_below_top(*static_cast<const arrow::ExtensionType&>(t).storage_type());
    for (const auto& f : t.fields())
        if (!thread_safe_below_top(*f->type())) return false;
    return true;
}

bool format_thread_safe(const arrow::DataType& t) {
    if (t.id() == arrow::Type::DICTIONARY)
        return thread_safe_below_top(*static_cast<const arrow::DictionaryType&>(t).value_type());
    return thread_safe_below_top(t);
}

void prepare_parallel_format(const arrow::Array& arr) {
    if (arr.type_id() == arrow::Type::DICTIONARY)
        (void)static_cast<const arrow::DictionaryArray&>(arr).dictionary();
}

void finish_csv_field(std::string& buf, size_t start, char sep) {
    const size_t n = buf.size() - start;
    const char* p = buf.data() + start;
    if (n == 3 && std::memcmp(p, NULL_SYMBOL, 3) == 0) { buf.resize(start); return; }
    bool quote = false;
    size_t nul = n;
    for (size_t i = 0; i < n; ++i) {
        const char c = p[i];
        if (c == sep || c == '"' || c == '\n' || c == '\r') { quote = true; break; }
        if (c == '\0' && nul == n) nul = i;
    }
    if (!quote) {
        if (nul < n) buf.resize(start + nul);   // fputs stopped at a NUL byte
        return;
    }
    const std::string field(p, n);
    buf.resize(start);
    buf += '"';
    for (char c : field) {
        if (c == '"') buf += '"';
        buf += c;
    }
    buf += '"';
}

void json_append_string(std::string& out, std::string_view v) {
    out += '"';
    size_t run = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const unsigned char c = (unsigned char)v[i];
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        out.append(v.data() + run, i - run);
        run = i + 1;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default: {
                char u[8];
                std::snprintf(u, sizeof u, "\\u%04x", c);
                out += u;
            }
        }
    }
    out.append(v.data() + run, v.size() - run);
    out += '"';
}
