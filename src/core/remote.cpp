// remote.cpp — input named by a URL (https://, s3://, gs://, ...), read
// through htslib's hFILE layer. With an htslib built against libcurl, hFILE
// serves http(s), ftp, s3 and gs (credentials as htslib documents them); vv
// adds no network client of its own. The readers that open files through
// htslib (BAM/CRAM, BCF, tabix, FASTA/FASTQ, faidx) take a URL unchanged;
// the Arrow-based readers get an arrow::io::RandomAccessFile over the hFILE
// (open_input_file); formats whose libraries need a local path are copied to
// a temporary file first (open.cpp).
#include "internal.hpp"

#include <htslib/hfile.h>

#include <algorithm>
#include <mutex>

bool is_remote_url(const std::string& path) {
    // scheme "://" ..., the scheme at least two characters (so a Windows
    // drive letter is never one) and not file://.
    size_t i = 0;
    while (i < path.size() &&
           (std::isalnum((unsigned char)path[i]) || path[i] == '+' || path[i] == '-' || path[i] == '.'))
        ++i;
    if (i < 2 || !std::isalpha((unsigned char)path[0]) || path.compare(i, 3, "://") != 0)
        return false;
    std::string scheme = path.substr(0, i);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return scheme != "file";
}

std::string strip_url_query(const std::string& url) {
    if (!is_remote_url(url)) return url;
    const size_t cut = url.find_first_of("?#", url.find("://") + 3);
    return cut == std::string::npos ? url : url.substr(0, cut);
}

std::string url_display_name(const std::string& url) {
    std::string p = strip_url_query(url);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    p = p.substr(p.find_last_of('/') + 1);
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '%' && i + 2 < p.size() && std::isxdigit((unsigned char)p[i + 1]) &&
            std::isxdigit((unsigned char)p[i + 2])) {
            out += (char)std::stoi(p.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += p[i];
        }
    }
    return out;
}

static std::string url_scheme(const std::string& url) {
    std::string s = url.substr(0, url.find("://"));
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// The URL schemes htslib can open in this build, among the ones that name
// remote data (http, https, ftp, ftps, s3, gs and their +http(s) forms).
// Listing them loads htslib's plugins; a static libhts with a plugin loader
// then logs one warning per scheme from the plugins' own copy of htslib, so
// stderr is muted for the probe. Computed once.
static const std::vector<std::string>& htslib_remote_schemes() {
    static const std::vector<std::string> schemes = [] {
        std::vector<std::string> out;
#if HTS_VERSION >= 101200
        const char* list[128];
        int n = 128;
        const int level = hts_get_log_level();
        hts_set_log_level(HTS_LOG_OFF);
        const int muted = stderr_mute();
        const int total = hfile_list_schemes(nullptr, list, &n);
        stderr_unmute(muted);
        hts_set_log_level((htsLogLevel)level);
        for (int i = 0; i < std::min(total, n); ++i) {
            const std::string s = list[i];
            for (const char* want : {"http", "https", "ftp", "ftps", "s3", "s3+http", "s3+https",
                                     "gs", "gs+http", "gs+https"})
                if (s == want) out.push_back(s);
        }
#endif
        static const char* order[] = {"https", "http", "s3", "s3+https", "s3+http",
                                      "gs", "gs+https", "gs+http", "ftp", "ftps"};
        auto rank = [](const std::string& s) {
            for (size_t i = 0; i < sizeof order / sizeof *order; ++i)
                if (s == order[i]) return i;
            return sizeof order / sizeof *order;
        };
        std::sort(out.begin(), out.end(),
                  [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
        return out;
    }();
    return schemes;
}

std::string remote_schemes() {
    std::string s;
    for (const auto& x : htslib_remote_schemes()) {
        if (x.find('+') != std::string::npos) continue;   // s3+https etc.: same support as s3
        s += (s.empty() ? "" : " ") + x;
    }
    return s.empty() ? "none" : s;
}

bool remote_supported(const std::string& url) {
    const auto& v = htslib_remote_schemes();
    return std::find(v.begin(), v.end(), url_scheme(url)) != v.end();
}

std::string remote_unsupported_message(const std::string& url) {
    return "this build of vv cannot open " + url_scheme(url) +
           ":// URLs (its htslib has no libcurl); fetch the file first, e.g. "
           "`curl -sL URL | vv -`, or use a vv package built against a system "
           "htslib (conda, Homebrew, the distribution's)";
}

namespace {

// arrow::io::RandomAccessFile over an hFILE. hFILE keeps one file position
// and is not thread-safe, so every read takes the mutex and seeks first when
// another reader moved the position (Parquet's pre-buffering reads ranges
// from several threads).
class HFileRandomAccess final : public arrow::io::RandomAccessFile {
public:
    static arrow::Result<std::shared_ptr<HFileRandomAccess>> Open(const std::string& url) {
        errno = 0;
        hFILE* fp = hopen(url.c_str(), "r");
        if (!fp)
            return arrow::Status::IOError("cannot open the URL: ",
                                          errno ? std::strerror(errno) : "unknown error");
        auto self = std::shared_ptr<HFileRandomAccess>(new HFileRandomAccess(fp, url));
        return self;
    }
    ~HFileRandomAccess() override { (void)DoClose(); }

    arrow::Status Close() override { std::lock_guard<std::mutex> g(mu_); return DoClose(); }
    bool closed() const override { return fp_ == nullptr; }

    arrow::Result<int64_t> Tell() const override {
        std::lock_guard<std::mutex> g(mu_);
        if (!fp_) return arrow::Status::Invalid("file is closed");
        return pos_;
    }
    arrow::Status Seek(int64_t pos) override {
        std::lock_guard<std::mutex> g(mu_);
        if (!fp_) return arrow::Status::Invalid("file is closed");
        if (pos < 0) return arrow::Status::Invalid("negative seek");
        pos_ = pos;
        return arrow::Status::OK();
    }
    arrow::Result<int64_t> GetSize() override {
        std::lock_guard<std::mutex> g(mu_);
        if (!fp_) return arrow::Status::Invalid("file is closed");
        if (size_ < 0) {
            const off_t end = hseek(fp_, 0, SEEK_END);
            if (end < 0)
                return arrow::Status::IOError(url_, ": the server does not report the size "
                                              "or accept range requests");
            size_ = (int64_t)end;
            at_ = size_;
        }
        return size_;
    }
    arrow::Result<int64_t> Read(int64_t nbytes, void* out) override {
        std::lock_guard<std::mutex> g(mu_);
        ARROW_ASSIGN_OR_RAISE(int64_t n, DoReadAt(pos_, nbytes, out));
        pos_ += n;
        return n;
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(nbytes));
        ARROW_ASSIGN_OR_RAISE(int64_t n, Read(nbytes, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(n, /*shrink_to_fit=*/false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }
    arrow::Result<int64_t> ReadAt(int64_t position, int64_t nbytes, void* out) override {
        std::lock_guard<std::mutex> g(mu_);
        return DoReadAt(position, nbytes, out);
    }
    arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t position, int64_t nbytes) override {
        ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateResizableBuffer(nbytes));
        ARROW_ASSIGN_OR_RAISE(int64_t n, ReadAt(position, nbytes, buf->mutable_data()));
        ARROW_RETURN_NOT_OK(buf->Resize(n, /*shrink_to_fit=*/false));
        return std::shared_ptr<arrow::Buffer>(std::move(buf));
    }

private:
    HFileRandomAccess(hFILE* fp, std::string url) : fp_(fp), url_(std::move(url)) {}

    arrow::Status DoClose() {
        if (!fp_) return arrow::Status::OK();
        const int r = hclose(fp_);
        fp_ = nullptr;
        return r == 0 ? arrow::Status::OK() : arrow::Status::IOError("closing ", url_);
    }
    // The caller holds mu_.
    arrow::Result<int64_t> DoReadAt(int64_t position, int64_t nbytes, void* out) {
        if (!fp_) return arrow::Status::Invalid("file is closed");
        if (position != at_) {
            if (hseek(fp_, (off_t)position, SEEK_SET) < 0)
                return arrow::Status::IOError(url_, ": cannot seek to byte ", position,
                                              " (the server does not accept range requests?)");
            at_ = position;
        }
        int64_t got = 0;
        char* p = static_cast<char*>(out);
        while (got < nbytes) {
            const ssize_t r = hread(fp_, p + got, (size_t)(nbytes - got));
            if (r < 0) return arrow::Status::IOError("reading ", url_, ": ", std::strerror(errno));
            if (r == 0) break;
            got += r;
        }
        at_ += got;
        return got;
    }

    hFILE*             fp_;
    std::string        url_;
    mutable std::mutex mu_;
    int64_t            pos_  = 0;    // the stream position (Read / Seek / Tell)
    int64_t            at_   = 0;    // where the hFILE is
    int64_t            size_ = -1;
};

}  // namespace

arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>> open_input_file(const std::string& path) {
    if (is_remote_url(path)) {
        if (!remote_supported(path)) return arrow::Status::IOError(remote_unsupported_message(path));
        ARROW_ASSIGN_OR_RAISE(auto f, HFileRandomAccess::Open(path));
        return std::static_pointer_cast<arrow::io::RandomAccessFile>(f);
    }
    ARROW_ASSIGN_OR_RAISE(auto f, arrow::io::ReadableFile::Open(path));
    return std::static_pointer_cast<arrow::io::RandomAccessFile>(f);
}

bool read_head(const std::string& path, void* buf, size_t n, size_t* got) {
    *got = 0;
    auto f = open_input_file(path);
    if (!f.ok()) return false;
    auto r = (*f)->ReadAt(0, (int64_t)n, buf);
    if (!r.ok()) return false;
    *got = (size_t)*r;
    return true;
}
