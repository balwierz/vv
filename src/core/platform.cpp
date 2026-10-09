// platform.cpp — the system calls whose POSIX and Windows forms differ:
// terminal size, globbing, temporary files, the environment, pipe detection,
// file mapping (vvjson::map_file) and the Windows console's modes. Everything
// else in vv uses the C / C++ standard library or the readers' libraries.
#include "internal.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <random>
#else
#include <fnmatch.h>
#include <sys/ioctl.h>
#endif

bool term_size(int fd, int* cols, int* rows) {
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    CONSOLE_SCREEN_BUFFER_INFO bi;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(h, &bi)) return false;
    *cols = bi.srWindow.Right - bi.srWindow.Left + 1;
    if (rows) *rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
    return *cols > 0;
#else
    struct winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return false;
    *cols = ws.ws_col;
    if (rows) *rows = ws.ws_row;
    return true;
#endif
}

#ifdef _WIN32
// fnmatch(3) with no flags, for a platform without it: `*`, `?`, bracket
// expressions (`[a-z]`, `[!x]` / `[^x]`, a leading `]` as a member) and `\`
// escapes. Backtracks to the last `*` only, which is enough for one star
// class (no `**` semantics are needed).
static bool bracket_match(const char*& p, char c) {
    const char* q = p + 1;                       // past '['
    bool neg = *q == '!' || *q == '^';
    if (neg) ++q;
    bool hit = false;
    bool first = true;
    while (*q && (first || *q != ']')) {
        first = false;
        char lo = *q == '\\' && q[1] ? *++q : *q;
        char hi = lo;
        if (q[1] == '-' && q[2] && q[2] != ']') {
            q += 2;
            hi = *q == '\\' && q[1] ? *++q : *q;
        }
        if (lo <= c && c <= hi) hit = true;
        ++q;
    }
    if (*q != ']') return c == '[';              // no closing ']': a literal '['
    p = q;                                       // at ']'
    return hit != neg;
}
#endif

bool glob_match(const char* pattern, const char* name) {
#ifdef _WIN32
    const char* p = pattern;
    const char* s = name;
    const char* star_p = nullptr;
    const char* star_s = nullptr;
    while (*s) {
        if (*p == '*') { star_p = ++p; star_s = s; continue; }
        bool ok;
        const char* np = p;
        if (*p == '?') ok = true;
        else if (*p == '[') {
            if (bracket_match(np, *s)) ok = true;
            else { ok = false; if (*np != ']') np = p; }   // literal '[' failed
        } else if (*p == '\\' && p[1]) { ++np; ok = *np == *s; }
        else ok = *p && *p == *s;
        if (ok) { p = np + 1; ++s; continue; }
        if (!star_p) return false;
        p = star_p;
        s = ++star_s;
    }
    while (*p == '*') ++p;
    return *p == '\0';
#else
    return fnmatch(pattern, name, 0) == 0;
#endif
}

int make_temp_file(std::string* tmpl, int suffix_len) {
#ifdef _WIN32
    const size_t end = tmpl->size() - (size_t)suffix_len;
    if (end < 6 || tmpl->compare(end - 6, 6, "XXXXXX") != 0) { errno = EINVAL; return -1; }
    static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (size_t i = end - 6; i < end; ++i) (*tmpl)[i] = kChars[gen() % 36];
        int fd = _open(tmpl->c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                       _S_IREAD | _S_IWRITE);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
#else
    return suffix_len > 0 ? mkstemps(tmpl->data(), suffix_len) : mkstemp(tmpl->data());
#endif
}

std::string temp_dir() {
    const char* d = std::getenv("TMPDIR");
    std::string s = d && *d ? d : "";
#ifdef _WIN32
    if (s.empty()) {
        std::error_code ec;
        s = std::filesystem::temp_directory_path(ec).generic_string();
    }
#endif
    if (s.empty()) s = "/tmp";
#ifdef _WIN32
    std::replace(s.begin(), s.end(), '\\', '/');   // one separator in the paths vv prints
#endif
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

void set_env(const char* name, const char* value, bool overwrite) {
#ifdef _WIN32
    if (!overwrite && std::getenv(name)) return;
    _putenv_s(name, value);
#else
    setenv(name, value, overwrite ? 1 : 0);
#endif
}

bool path_is_pipe(const std::string& path) {
#ifdef _WIN32
    (void)path;   // named pipes (\\.\pipe\…) are not read by name
    return false;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode) || S_ISSOCK(st.st_mode);
#endif
}

void init_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    for (DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = GetStdHandle(which);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

#ifdef _WIN32
namespace vvjson {

std::string map_file(const std::string& path, void** view, size_t* size) {
    *view = nullptr;
    *size = 0;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "Cannot open '" + path + "'";
    LARGE_INTEGER n;
    if (!GetFileSizeEx(f, &n)) { CloseHandle(f); return "Cannot stat '" + path + "'"; }
    if (n.QuadPart == 0) { CloseHandle(f); return ""; }
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);   // the mapping keeps the file open
    if (!m) return "Cannot map '" + path + "'";
    void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);   // the view keeps the mapping alive
    if (!v) return "Cannot map '" + path + "'";
    *view = v;
    *size = (size_t)n.QuadPart;
    return "";
}

void unmap_file(void* view) {
    if (view) UnmapViewOfFile(view);
}

}  // namespace vvjson
#endif
