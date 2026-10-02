// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// UCSC wiggle (.wig): fixedStep / variableStep sections (and bedGraph-style
// lines) expanded on the fly into bedGraph rows — chrom, start, end, value,
// 0-based half-open — and read by the delimited reader as a bedGraph.

#include "internal.hpp"

namespace {

// The wiggle text in, bedGraph lines out (an InputStream for the CSV reader).
class WigStream : public arrow::io::InputStream {
public:
    explicit WigStream(std::shared_ptr<arrow::io::InputStream> src) : in_(std::move(src)) {}
    arrow::Status Close() override { closed_ = true; return arrow::Status::OK(); }
    bool closed() const override { return closed_; }
    arrow::Result<int64_t> Tell() const override { return pos_; }
    arrow::Result<int64_t> Read(int64_t nbytes, void* out) override {
        if (!err_.ok()) return err_;                 // a reader may ask again
        auto* dst = static_cast<char*>(out);
        int64_t done = 0;
        while (done < nbytes) {
            if (out_pos_ == out_.size()) {
                if (eof_) break;
                out_.clear(); out_pos_ = 0;
                err_ = fill();
                if (!err_.ok()) return err_;
                if (out_.empty() && eof_) break;
            }
            const size_t take = std::min<size_t>((size_t)(nbytes - done), out_.size() - out_pos_);
            std::memcpy(dst + done, out_.data() + out_pos_, take);
            out_pos_ += take; done += (int64_t)take;
        }
        pos_ += done;
        return done;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(nbytes));
        ARROW_ASSIGN_OR_RAISE(int64_t n, Read(nbytes, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(n, false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }

private:
    enum class Mode { None, Fixed, Variable };
    LineReader  in_;
    std::string line_, out_;
    size_t      out_pos_ = 0;
    bool        eof_ = false, closed_ = false;
    arrow::Status err_;
    int64_t     pos_ = 0, line_no_ = 0;
    Mode        mode_ = Mode::None;
    std::string chrom_;
    int64_t     next_ = 0, step_ = 1, span_ = 1;

    arrow::Status bad(const std::string& why) const {
        return arrow::Status::IOError("WIG line " + std::to_string(line_no_) + ": " + why);
    }
    static bool to_int(const std::string& s, int64_t* v) {
        char* e = nullptr;
        *v = std::strtoll(s.c_str(), &e, 10);
        return !s.empty() && e && *e == '\0';
    }
    // key=value words of a fixedStep / variableStep line.
    arrow::Status declaration(const std::vector<std::string>& w, bool fixed) {
        std::string chrom;
        int64_t start = -1, step = 1, span = 1;
        for (size_t i = 1; i < w.size(); ++i) {
            const size_t eq = w[i].find('=');
            if (eq == std::string::npos) return bad("expected key=value, got '" + w[i] + "'");
            const std::string k = w[i].substr(0, eq), v = w[i].substr(eq + 1);
            int64_t n = 0;
            if (k == "chrom") chrom = v;
            else if (k == "start" || k == "step" || k == "span") {
                if (!to_int(v, &n) || n < (k == "start" ? 1 : 1)) return bad("bad " + k + "=" + v);
                (k == "start" ? start : k == "step" ? step : span) = n;
            }
        }
        if (chrom.empty()) return bad("no chrom= in " + w[0]);
        if (fixed && start < 1) return bad("fixedStep needs start= (1-based)");
        chrom_ = chrom; step_ = step; span_ = span;
        mode_ = fixed ? Mode::Fixed : Mode::Variable;
        next_ = fixed ? start - 1 : 0;
        return arrow::Status::OK();
    }
    void emit(const std::string& chrom, int64_t s, int64_t e, const std::string& value) {
        out_ += chrom; out_ += '\t';
        out_ += std::to_string(s); out_ += '\t';
        out_ += std::to_string(e); out_ += '\t';
        out_ += value; out_ += '\n';
    }
    // Translate input lines until there is output to hand on (or the end).
    arrow::Status fill() {
        while (out_.size() < (1u << 16) && !eof_) {
            const bool nl = in_.read_line(&line_);
            if (!nl && line_.empty()) { eof_ = true; break; }
            ++line_no_;
            if (!line_.empty() && line_.back() == '\r') line_.pop_back();
            std::vector<std::string> w;
            for (size_t i = 0; i < line_.size();) {
                while (i < line_.size() && (line_[i] == ' ' || line_[i] == '\t')) ++i;
                size_t j = i;
                while (j < line_.size() && line_[j] != ' ' && line_[j] != '\t') ++j;
                if (j > i) w.push_back(line_.substr(i, j - i));
                i = j;
            }
            if (w.empty() || w[0][0] == '#' || w[0] == "track" || w[0] == "browser") continue;
            if (w[0] == "variableStep") { ARROW_RETURN_NOT_OK(declaration(w, false)); continue; }
            if (w[0] == "fixedStep")    { ARROW_RETURN_NOT_OK(declaration(w, true));  continue; }
            if (mode_ == Mode::Fixed && w.size() == 1) {
                emit(chrom_, next_, next_ + span_, w[0]);
                next_ += step_;
            } else if (mode_ == Mode::Variable && w.size() == 2) {
                int64_t p = 0;
                if (!to_int(w[0], &p) || p < 1) return bad("bad position '" + w[0] + "'");
                emit(chrom_, p - 1, p - 1 + span_, w[1]);
            } else if (w.size() == 4) {            // a bedGraph line (0-based start)
                emit(w[0], std::strtoll(w[1].c_str(), nullptr, 10),
                     std::strtoll(w[2].c_str(), nullptr, 10), w[3]);
            } else {
                return bad(mode_ == Mode::None ? "data before a fixedStep / variableStep line"
                                               : "unexpected field count");
            }
        }
        return arrow::Status::OK();
    }
};

}  // namespace

std::string open_wig_source(const std::string& path, const Config& cfg,
                            std::unique_ptr<TabularSource>* out) {
    std::shared_ptr<arrow::io::InputStream> in;
    if (auto e = open_decoded_file(path, &in); !e.empty()) return e;
    auto wig = std::make_shared<WigStream>(std::move(in));
    if (auto e = open_delimited_stream(wig, path, DelimKind::BED, /*is_gz=*/true, cfg.region, out,
                                       '\t', HeaderMode::Off);
        !e.empty())
        return e.rfind("IOError: ", 0) == 0 ? e.substr(9) : e;
    delimited_apply_bed_variant(**out, BedVariant::BedGraph);
    delimited_apply_column_names(**out, {}, "WIG (as bedGraph intervals)");
    return "";
}
