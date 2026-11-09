#include "util.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <sstream>

extern char** environ;

// =================================================================
// UTF-8
// =================================================================
static std::u32string decode(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }  // stray byte
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

static void encode(char32_t cp, std::string& out) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static std::string encode(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) encode(c, out);
    return out;
}

std::string lower(std::string_view s) {
    bool ascii = true;
    for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    std::string out(s);
    if (ascii) {
        for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return out;
    }
    // Needs a UTF-8 LC_CTYPE (main() sets one) for towlower to know accents.
    std::u32string u = decode(s);
    for (char32_t& c : u) c = static_cast<char32_t>(std::towlower(static_cast<wint_t>(c)));
    return encode(u);
}

size_t cp_count(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string cp_slice(std::string_view s, size_t start, size_t n) {
    std::u32string u = decode(s);
    if (start >= u.size()) return "";
    return encode(u.substr(start, n));
}

// =================================================================
// Python-style string helpers
// =================================================================
static bool is_ws(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r') || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x1F; }

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    while (b > a && is_ws(s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

std::string trim_chars(std::string_view s, std::string_view chars) {
    size_t a = s.find_first_not_of(chars);
    if (a == std::string_view::npos) return "";
    size_t b = s.find_last_not_of(chars);
    return std::string(s.substr(a, b - a + 1));
}

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_ws(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !is_ws(s[j])) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::vector<std::string> split(std::string_view s, std::string_view sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            return out;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + sep.size();
    }
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string collapse_ws(std::string_view s) { return join(split_ws(s), " "); }

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

std::string truncate(std::string_view value, size_t width) {
    std::u32string u = decode(collapse_ws(value));
    if (u.size() > width) return encode(u.substr(0, width - 1)) + "…";
    return encode(u);
}

// textwrap's idea of a letter: \w but not a digit or underscore.
static bool is_letter(char32_t c) { return c >= 0x80 ? std::iswalpha(static_cast<wint_t>(c)) : std::isalpha(static_cast<int>(c)); }

// Split one whitespace-free word the way textwrap does with break_on_hyphens:
// "elasto-capillary" -> "elasto-", "capillary", but "FENE-P" stays whole.
static void hyphen_chunks(const std::u32string& w, std::vector<std::u32string>& out) {
    size_t start = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] != U'-' || i < 2) continue;
        bool before = (is_letter(w[i - 1]) && is_letter(w[i - 2])) ||
                      (i >= 3 && is_letter(w[i - 1]) && w[i - 2] == U'-' && is_letter(w[i - 3]));
        size_t j = i + 1;
        bool after = j < w.size() && is_letter(w[j]);
        if (after) {
            size_t k = j + 1;
            if (k < w.size() && w[k] == U'-') ++k;
            after = k < w.size() && is_letter(w[k]);
        }
        if (before && after) {
            out.push_back(w.substr(start, i + 1 - start));
            start = i + 1;
        }
    }
    out.push_back(w.substr(start));
}

std::vector<std::string> textwrap(std::string_view text, size_t width) {
    // Chunks, in reverse so the next one is at the back (as textwrap does).
    std::u32string u = decode(collapse_ws(text));
    std::vector<std::u32string> chunks;
    for (size_t i = 0; i < u.size();) {
        size_t j = i;
        if (u[i] == U' ') {
            while (j < u.size() && u[j] == U' ') ++j;
            chunks.push_back(u.substr(i, j - i));
        } else {
            while (j < u.size() && u[j] != U' ') ++j;
            hyphen_chunks(u.substr(i, j - i), chunks);
        }
        i = j;
    }
    std::reverse(chunks.begin(), chunks.end());
    auto blank = [](const std::u32string& c) { return c.find_first_not_of(U' ') == std::u32string::npos; };

    std::vector<std::string> lines;
    while (!chunks.empty()) {
        std::vector<std::u32string> cur;
        size_t cur_len = 0;
        if (blank(chunks.back()) && !lines.empty()) chunks.pop_back();
        while (!chunks.empty() && cur_len + chunks.back().size() <= width) {
            cur_len += chunks.back().size();
            cur.push_back(std::move(chunks.back()));
            chunks.pop_back();
        }
        if (!chunks.empty() && chunks.back().size() > width) {
            // A word longer than the line: break it, after a hyphen if there is one.
            size_t space_left = width > cur_len ? width - cur_len : 1;
            std::u32string& chunk = chunks.back();
            size_t end = space_left;
            size_t hyphen = chunk.rfind(U'-', space_left - 1);
            if (hyphen != std::u32string::npos && hyphen > 0 && hyphen < space_left &&
                chunk.substr(0, hyphen).find_first_not_of(U'-') != std::u32string::npos)
                end = hyphen + 1;
            cur.push_back(chunk.substr(0, end));
            chunk = chunk.substr(end);
        } else if (cur.empty() && !chunks.empty()) {
            cur.push_back(std::move(chunks.back()));
            chunks.pop_back();
        }
        if (!cur.empty() && blank(cur.back())) cur.pop_back();
        if (!cur.empty()) {
            std::u32string line;
            for (auto& c : cur) line += c;
            lines.push_back(encode(line));
        }
    }
    return lines;
}

// =================================================================
// Paths and files
// =================================================================
fs::path home_dir() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path("/");
}

fs::path expand_user(const std::string& path) {
    if (path == "~") return home_dir();
    if (path.starts_with("~/")) return home_dir() / path.substr(2);
    return path;
}

std::string today_iso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
}

void copy_file_with_times(const fs::path& from, const fs::path& to) {
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    std::error_code ec;
    fs::last_write_time(to, fs::last_write_time(from), ec);
    fs::permissions(to, fs::status(from).permissions(), ec);
}

// =================================================================
// Other programs
// =================================================================
namespace {
// argv as the char* array exec wants, kept alive with the strings.
struct Argv {
    std::vector<std::string> strings;
    std::vector<char*> ptrs;
    explicit Argv(const std::vector<std::string>& a) : strings(a) {
        for (auto& s : strings) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }
};
}  // namespace

int run_wait(const std::vector<std::string>& argv, const fs::path& cwd) {
    if (argv.empty()) return -1;
    Argv a(argv);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    pid_t pid;
    int rc = posix_spawnp(&pid, a.ptrs[0], &fa, nullptr, a.ptrs.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;
    Argv a(argv);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);  // own session: survives us
    pid_t pid;
    posix_spawnp(&pid, a.ptrs[0], &fa, &attr, a.ptrs.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
}

void reap_children() {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
}

static std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

void copy_to_clipboard(const std::string& text) {
    if (std::getenv("WAYLAND_DISPLAY")) {
        int fds[2];
        if (pipe(fds) == 0) {
            Argv a({"wl-copy"});
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
            posix_spawn_file_actions_addclose(&fa, fds[1]);
            posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
            pid_t pid;
            int rc = posix_spawnp(&pid, "wl-copy", &fa, nullptr, a.ptrs.data(), environ);
            posix_spawn_file_actions_destroy(&fa);
            close(fds[0]);
            if (rc == 0) {
                size_t off = 0;
                while (off < text.size()) {
                    ssize_t n = write(fds[1], text.data() + off, text.size() - off);
                    if (n <= 0) break;
                    off += n;
                }
                close(fds[1]);
                int status = 0;
                waitpid(pid, &status, 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return;
            } else {
                close(fds[1]);
            }
        }
    }
    // OSC 52: the terminal puts it on the clipboard (works over ssh too).
    std::string seq = "\033]52;c;" + base64(text) + "\a";
    std::fwrite(seq.data(), 1, seq.size(), stdout);
    std::fflush(stdout);
}

std::vector<std::string> editor_command(const fs::path& file) {
    const char* e = std::getenv("EDITOR");
    std::vector<std::string> argv = split_ws(e && *e ? e : "nvim");
    if (argv.empty()) argv = {"nvim"};
    argv.push_back(file.string());
    return argv;
}
