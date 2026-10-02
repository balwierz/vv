// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Piotr Balwierz
//
// Compressed input: the codec a stream starts with (gzip, zstd, bzip2, xz, by
// magic bytes) and a decoded stream over it. gzip / zstd use Arrow's codecs;
// bzip2 and xz have their own InputStream adapters over libbz2 / liblzma
// (Arrow has no xz codec, and its bzip2 codec depends on how Arrow was built).

#include "internal.hpp"

#if VV_HAVE_LZMA
#include <lzma.h>
#endif
#if VV_HAVE_BZ2
#include <bzlib.h>
#endif

StreamCodec sniff_codec(const uint8_t* m, size_t n) {
    if (n >= 2 && m[0] == 0x1f && m[1] == 0x8b) return StreamCodec::Gzip;
    if (n >= 4 && m[0] == 0x28 && m[1] == 0xb5 && m[2] == 0x2f && m[3] == 0xfd) return StreamCodec::Zstd;
    if (n >= 6 && std::memcmp(m, "\xfd" "7zXZ\0", 6) == 0) return StreamCodec::Xz;
    // "BZh" + block size digit, then a block (magic 0x314159265359, π) or, in
    // an empty stream, the end-of-stream magic 0x177245385090 (√π).
    if (n >= 10 && m[0] == 'B' && m[1] == 'Z' && m[2] == 'h' && m[3] >= '1' && m[3] <= '9' &&
        (std::memcmp(m + 4, "\x31\x41\x59\x26\x53\x59", 6) == 0 ||
         std::memcmp(m + 4, "\x17\x72\x45\x38\x50\x90", 6) == 0)) return StreamCodec::Bz2;
    return StreamCodec::None;
}

StreamCodec sniff_file_codec(const std::shared_ptr<arrow::io::ReadableFile>& rf) {
    uint8_t m[10];
    auto got = rf->ReadAt(0, sizeof(m), m);
    const size_t n = got.ok() ? (size_t)*got : 0;
    (void)rf->Seek(0);
    return sniff_codec(m, n);
}

const char* codec_label(StreamCodec c) {
    switch (c) {
        case StreamCodec::Gzip: return "gzip";
        case StreamCodec::Zstd: return "zstd";
        case StreamCodec::Bz2:  return "bzip2";
        case StreamCodec::Xz:   return "xz";
        default:                return "";
    }
}

bool has_compression_suffix(const std::string& path) {
    for (const char* z : {".gz", ".bgz", ".zst", ".zstd", ".xz", ".bz2"})
        if (fends_ci(path, z)) return true;
    return false;
}

std::string strip_compression_suffix(const std::string& path) {
    for (const char* z : {".gz", ".bgz", ".zst", ".zstd", ".xz", ".bz2"})
        if (fends_ci(path, z)) return path.substr(0, path.size() - std::strlen(z));
    return path;
}

namespace {

// A decoder over another stream: subclasses implement step(), which turns
// pending input (in_, in_pos_) into output, refilling from the source.
class DecodeStream : public arrow::io::InputStream {
public:
    explicit DecodeStream(std::shared_ptr<arrow::io::InputStream> src) : src_(std::move(src)) {}
    arrow::Status Close() override { closed_ = true; return src_->Close(); }
    bool closed() const override { return closed_; }
    arrow::Result<int64_t> Tell() const override { return pos_; }
    arrow::Result<int64_t> Read(int64_t nbytes, void* out) override {
        int64_t done = 0;
        auto* dst = static_cast<uint8_t*>(out);
        while (done < nbytes && !eof_) {
            if (in_pos_ == in_len_ && !src_eof_) {
                ARROW_ASSIGN_OR_RAISE(int64_t got, src_->Read((int64_t)in_.size(), in_.data()));
                in_len_ = (size_t)got; in_pos_ = 0;
                if (got == 0) src_eof_ = true;
            }
            size_t produced = 0;
            ARROW_RETURN_NOT_OK(step(dst + done, (size_t)(nbytes - done), &produced));
            done += (int64_t)produced;
        }
        pos_ += done;
        return done;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(nbytes));
        ARROW_ASSIGN_OR_RAISE(int64_t n, Read(nbytes, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(n, /*shrink_to_fit=*/false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }

protected:
    // Decode into dst (room for `room` bytes); set *produced; set eof_ at the
    // end of the data. Called with in_pos_ == in_len_ only once src_eof_.
    virtual arrow::Status step(uint8_t* dst, size_t room, size_t* produced) = 0;

    std::shared_ptr<arrow::io::InputStream> src_;
    std::vector<uint8_t> in_ = std::vector<uint8_t>(1 << 16);
    size_t in_pos_ = 0, in_len_ = 0;
    bool src_eof_ = false, eof_ = false, closed_ = false;
    int64_t pos_ = 0;
};

#if VV_HAVE_LZMA
class XzStream : public DecodeStream {
public:
    using DecodeStream::DecodeStream;
    ~XzStream() override { lzma_end(&z_); }
    arrow::Status init() {
        // CONCATENATED: several .xz streams back to back (pixz, `cat a.xz b.xz`).
        if (lzma_stream_decoder(&z_, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
            return arrow::Status::IOError("xz: cannot start the decoder");
        return arrow::Status::OK();
    }
protected:
    arrow::Status step(uint8_t* dst, size_t room, size_t* produced) override {
        z_.next_in = in_.data() + in_pos_; z_.avail_in = in_len_ - in_pos_;
        z_.next_out = dst; z_.avail_out = room;
        const lzma_ret r = lzma_code(&z_, src_eof_ ? LZMA_FINISH : LZMA_RUN);
        in_pos_ = in_len_ - z_.avail_in;
        *produced = room - z_.avail_out;
        if (r == LZMA_STREAM_END) { eof_ = true; return arrow::Status::OK(); }
        if (r == LZMA_OK) {
            if (src_eof_ && *produced == 0 && z_.avail_in == 0)
                return arrow::Status::IOError("xz: the file ends inside a stream (truncated?)");
            return arrow::Status::OK();
        }
        if (r == LZMA_BUF_ERROR && src_eof_)
            return arrow::Status::IOError("xz: the file ends inside a stream (truncated?)");
        return arrow::Status::IOError(r == LZMA_FORMAT_ERROR ? "xz: not an .xz stream"
                                      : r == LZMA_DATA_ERROR ? "xz: corrupt data"
                                      : r == LZMA_MEM_ERROR  ? "xz: out of memory"
                                                             : "xz: decoding failed");
    }
private:
    lzma_stream z_ = LZMA_STREAM_INIT;
};
#endif

#if VV_HAVE_BZ2
class Bz2Stream : public DecodeStream {
public:
    using DecodeStream::DecodeStream;
    ~Bz2Stream() override { if (live_) BZ2_bzDecompressEnd(&z_); }
    arrow::Status init() {
        std::memset(&z_, 0, sizeof(z_));
        if (BZ2_bzDecompressInit(&z_, 0, 0) != BZ_OK)
            return arrow::Status::IOError("bzip2: cannot start the decoder");
        live_ = true;
        return arrow::Status::OK();
    }
protected:
    arrow::Status step(uint8_t* dst, size_t room, size_t* produced) override {
        *produced = 0;
        if (stream_done_) {
            // Another bzip2 stream follows (pbzip2, `cat a.bz2 b.bz2`), or the end.
            if (in_pos_ == in_len_) { if (src_eof_) eof_ = true; return arrow::Status::OK(); }
            BZ2_bzDecompressEnd(&z_); live_ = false;
            ARROW_RETURN_NOT_OK(init());
            stream_done_ = false;
        }
        z_.next_in = reinterpret_cast<char*>(in_.data() + in_pos_);
        z_.avail_in = (unsigned)(in_len_ - in_pos_);
        z_.next_out = reinterpret_cast<char*>(dst);
        z_.avail_out = (unsigned)std::min<size_t>(room, 1u << 30);
        const unsigned room_given = z_.avail_out;
        const int r = BZ2_bzDecompress(&z_);
        in_pos_ = in_len_ - z_.avail_in;
        *produced = room_given - z_.avail_out;
        if (r == BZ_STREAM_END) { stream_done_ = true; return arrow::Status::OK(); }
        if (r == BZ_OK) {
            if (src_eof_ && *produced == 0 && z_.avail_in == 0)
                return arrow::Status::IOError("bzip2: the file ends inside a stream (truncated?)");
            return arrow::Status::OK();
        }
        return arrow::Status::IOError(r == BZ_DATA_ERROR_MAGIC ? "bzip2: not a .bz2 stream"
                                      : r == BZ_DATA_ERROR     ? "bzip2: corrupt data"
                                      : r == BZ_MEM_ERROR      ? "bzip2: out of memory"
                                                               : "bzip2: decoding failed");
    }
private:
    bz_stream z_{};
    bool live_ = false, stream_done_ = false;
};
#endif

}  // namespace

std::string decode_stream(StreamCodec c, std::shared_ptr<arrow::io::InputStream> in,
                          std::shared_ptr<arrow::io::InputStream>* out) {
    switch (c) {
        case StreamCodec::None:
            *out = std::move(in);
            return "";
        case StreamCodec::Gzip:
        case StreamCodec::Zstd: {
            auto codec = arrow::util::Codec::Create(c == StreamCodec::Gzip ? arrow::Compression::GZIP
                                                                           : arrow::Compression::ZSTD);
            if (!codec.ok()) return codec.status().ToString();
            auto ci = arrow::io::CompressedInputStream::Make(codec->get(), std::move(in));
            if (!ci.ok()) return ci.status().ToString();
            *out = *ci;
            return "";
        }
        case StreamCodec::Xz: {
#if VV_HAVE_LZMA
            auto s = std::make_shared<XzStream>(std::move(in));
            if (auto st = s->init(); !st.ok()) return st.message();
            *out = std::move(s);
            return "";
#else
            return "this vv was built without xz support (liblzma)";
#endif
        }
        case StreamCodec::Bz2: {
#if VV_HAVE_BZ2
            auto s = std::make_shared<Bz2Stream>(std::move(in));
            if (auto st = s->init(); !st.ok()) return st.message();
            *out = std::move(s);
            return "";
#else
            return "this vv was built without bzip2 support (libbz2)";
#endif
        }
    }
    return "unknown compression";
}

std::string open_decoded_file(const std::string& path, std::shared_ptr<arrow::io::InputStream>* out,
                              StreamCodec* codec) {
    auto rf = arrow::io::ReadableFile::Open(path);
    if (!rf.ok()) return "Cannot open '" + path + "': " + rf.status().ToString();
    const StreamCodec c = sniff_file_codec(*rf);
    if (codec) *codec = c;
    if (auto e = decode_stream(c, *rf, out); !e.empty()) return "'" + path + "': " + e;
    return "";
}
