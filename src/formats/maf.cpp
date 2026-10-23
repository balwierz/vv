// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// UCSC Multiple Alignment Format (.maf starting with "##maf"; the mutation
// annotation format shares the extension and is read as a table): blocks of
// "a score=…" followed by "s src start size strand srcSize text" lines, one
// row per "s" line. Read forward in batches, as a genome-wide multiz file is
// gigabytes; "i", "e" and "q" lines are skipped. The "##maf" header and the
// comments before the first block are the preamble.

#include "internal.hpp"

namespace {

class MafSource : public TabularSource {
    std::string                            path_;
    std::shared_ptr<arrow::Schema>         schema_;
    std::vector<std::string>               header_;

    mutable std::unique_ptr<LineReader>    lr_;
    mutable std::string                    pending_;          // a line read past the header
    mutable bool                           has_pending_ = false;
    mutable int64_t                        block_ = 0;
    mutable double                         score_ = 0;
    mutable bool                           has_score_ = false;
    mutable int64_t                        line_no_ = 0;

    mutable std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
    mutable std::vector<int64_t>           batch_first_row_;
    mutable std::vector<int64_t>           batch_num_rows_;   // kept after eviction
    mutable int64_t                        rows_so_far_ = 0;
    mutable bool                           all_read_    = false;
    mutable bool                           retain_all_  = false;
    mutable bool                           evicted_any_ = false;
    mutable arrow::Status                  read_status_;      // sticky stream error

    static constexpr int BATCH_SIZE = 4096;

    bool next_line(std::string* line) const {
        if (has_pending_) { *line = std::move(pending_); has_pending_ = false; return true; }
        const bool nl = lr_->read_line(line);
        if (!nl && line->empty()) return false;
        ++line_no_;
        return true;
    }

    arrow::Status advance(int64_t row_cap = -1) const {
        if (all_read_) return arrow::Status::OK();
        arrow::Int64Builder block_b, start_b, size_b, src_size_b;
        arrow::DoubleBuilder score_b;
        arrow::StringBuilder src_b, strand_b, text_b;
        const int cap = (row_cap > 0 && row_cap < BATCH_SIZE) ? (int)row_cap : BATCH_SIZE;
        const int64_t byte_budget = fastx_byte_budget();
        int count = 0;
        int64_t bytes = 0;
        std::string line;
        while (count < cap && bytes < byte_budget) {
            if (!next_line(&line)) { all_read_ = true; break; }
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == 'a' && (line.size() == 1 || line[1] == ' ' || line[1] == '\t')) {
                ++block_;
                has_score_ = false;
                if (size_t p = line.find("score="); p != std::string::npos) {
                    char* end = nullptr;
                    score_ = std::strtod(line.c_str() + p + 6, &end);
                    has_score_ = end != line.c_str() + p + 6;
                }
                continue;
            }
            if (line[0] != 's' || line.size() < 2 || (line[1] != ' ' && line[1] != '\t')) continue;
            std::istringstream in(line.substr(1));
            std::string src, strand, text;
            int64_t start = 0, size = 0, src_size = 0;
            if (!(in >> src >> start >> size >> strand >> src_size >> text) ||
                (strand != "+" && strand != "-")) {
                all_read_ = true;
                return arrow::Status::Invalid("line ", line_no_, ": not a MAF \"s\" line: ",
                                              line.substr(0, 60));
            }
            ARROW_RETURN_NOT_OK(block_b.Append(std::max<int64_t>(block_, 1)));
            ARROW_RETURN_NOT_OK(has_score_ ? score_b.Append(score_) : score_b.AppendNull());
            ARROW_RETURN_NOT_OK(src_b.Append(valid_utf8(src)));
            ARROW_RETURN_NOT_OK(start_b.Append(start));
            ARROW_RETURN_NOT_OK(size_b.Append(size));
            ARROW_RETURN_NOT_OK(strand_b.Append(strand));
            ARROW_RETURN_NOT_OK(src_size_b.Append(src_size));
            ARROW_RETURN_NOT_OK(text_b.Append(valid_utf8(text)));
            bytes += (int64_t)(src.size() + text.size()) + 40;
            ++count;
        }
        if (count == 0) { all_read_ = true; return arrow::Status::OK(); }
        std::vector<std::shared_ptr<arrow::Array>> a(8);
        ARROW_RETURN_NOT_OK(block_b.Finish(&a[0]));
        ARROW_RETURN_NOT_OK(score_b.Finish(&a[1]));
        ARROW_RETURN_NOT_OK(src_b.Finish(&a[2]));
        ARROW_RETURN_NOT_OK(start_b.Finish(&a[3]));
        ARROW_RETURN_NOT_OK(size_b.Finish(&a[4]));
        ARROW_RETURN_NOT_OK(strand_b.Finish(&a[5]));
        ARROW_RETURN_NOT_OK(src_size_b.Finish(&a[6]));
        ARROW_RETURN_NOT_OK(text_b.Finish(&a[7]));
        stream_retain(batches_, batch_first_row_, batch_num_rows_, rows_so_far_, retain_all_, evicted_any_,
                      arrow::RecordBatch::Make(schema_, count, a));
        return arrow::Status::OK();
    }

public:
    static std::string open(const std::string& path, std::unique_ptr<TabularSource>* out) {
        auto self = std::make_unique<MafSource>();
        self->path_ = path;
        std::shared_ptr<arrow::io::InputStream> in;
        if (auto e = open_decoded_file(path, &in); !e.empty()) return e;
        self->lr_ = std::make_unique<LineReader>(std::move(in));
        std::string line;
        while (self->next_line(&line)) {
            if (!line.empty() && line[0] != '#') { self->pending_ = line; self->has_pending_ = true; break; }
            if (!line.empty()) self->header_.push_back(line);
        }
        if (self->header_.empty() || self->header_.front().rfind("##maf", 0) != 0)
            return "'" + path + "': a UCSC MAF file starts with \"##maf\"";
        self->schema_ = arrow::schema({
            arrow::field("block", arrow::int64()), arrow::field("score", arrow::float64()),
            arrow::field("src", arrow::utf8()), arrow::field("start", arrow::int64()),
            arrow::field("size", arrow::int64()), arrow::field("strand", arrow::utf8()),
            arrow::field("srcSize", arrow::int64()), arrow::field("text", arrow::utf8())});
        if (auto st = self->advance(); !st.ok()) self->read_status_ = st;
        *out = std::move(self);
        return "";
    }

    std::shared_ptr<arrow::Schema> schema() const override { return schema_; }
    int64_t total_rows() const override { return all_read_ ? rows_so_far_ : -1; }
    int     num_chunks() const override { return (int)batches_.size(); }
    ChunkMeta chunk_meta(int i) const override { return {batch_first_row_[i], batch_num_rows_[i]}; }
    void set_retain_all(bool b) override { retain_all_ = b; }
    bool evicted_any() const override { return evicted_any_; }
    int64_t chunk_bytes(int i) const override { return stream_chunk_bytes(batches_, i); }
    arrow::Status read_status() const override { return read_status_; }
    void ensure(int i) override {
        while (!all_read_ && (int)batches_.size() <= i) {
            auto st = advance();
            if (!st.ok()) { if (read_status_.ok()) read_status_ = st; break; }
        }
    }
    arrow::Status read_chunk(int i, const std::vector<int>& cols, std::shared_ptr<arrow::Table>* out) override {
        ensure(i);
        if (i >= (int)batches_.size()) return arrow::Status::IndexError("chunk ", i, " out of range");
        if (!batches_[i])
            return arrow::Status::CapacityError("chunk ", i, " was released by the streaming window");
        *out = batch_slice_to_table(*batches_[i], cols, schema_);
        return arrow::Status::OK();
    }
    const std::string& path() const override { return path_; }
    std::string footer() const override {
        return "Format: UCSC MAF  |  a row per \"s\" line; start 0-based on its strand";
    }
    std::vector<std::string> preamble_above() const override { return header_; }
    std::string sequence_alphabet() const override { return "nucleotide"; }
};

}  // namespace

std::string open_maf_source(const std::string& path, const Config& /*cfg*/, std::unique_ptr<TabularSource>* out) {
    return MafSource::open(path, out);
}
