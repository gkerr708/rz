// Small helpers shared by everything: Python-style string functions that
// count characters (not bytes), paths, and running other programs.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// --- strings (UTF-8 aware where it matters) ---
std::string lower(std::string_view s);                        // Unicode lowercase
std::string trim(std::string_view s);                         // Python s.strip()
std::string trim_chars(std::string_view s, std::string_view chars);  // s.strip(chars)
std::vector<std::string> split_ws(std::string_view s);        // Python s.split()
std::vector<std::string> split(std::string_view s, std::string_view sep);  // s.split(sep)
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string collapse_ws(std::string_view s);                  // " ".join(s.split())
std::string replace_all(std::string s, std::string_view from, std::string_view to);
bool contains(std::string_view hay, std::string_view needle);

size_t cp_count(std::string_view s);                          // len() in characters
std::string cp_slice(std::string_view s, size_t start, size_t n = std::string::npos);

// "long text…" cut to `width` characters, whitespace collapsed.
std::string truncate(std::string_view value, size_t width = 55);
// Python's textwrap.wrap(text, width): whole words, breaking after hyphens.
std::vector<std::string> textwrap(std::string_view text, size_t width);

// --- paths ---
fs::path home_dir();
fs::path expand_user(const std::string& path);  // ~/x -> /home/me/x
std::string today_iso();                        // 2026-11-09
std::string read_file(const fs::path& p);
bool write_file(const fs::path& p, const std::string& text);
// shutil.copy2: copy contents, permissions and modification time.
void copy_file_with_times(const fs::path& from, const fs::path& to);

// --- other programs ---
// Run and wait, sharing this terminal (for $EDITOR). Returns the exit code.
int run_wait(const std::vector<std::string>& argv, const fs::path& cwd = {});
// Start a GUI program (zathura, the browser) that outlives us, output hidden.
void spawn_detached(const std::vector<std::string>& argv);
// Collect finished detached programs so they don't linger as zombies.
void reap_children();
// wl-copy when on Wayland, otherwise the terminal's OSC 52 clipboard.
void copy_to_clipboard(const std::string& text);
// $EDITOR split into words, defaulting to nvim.
std::vector<std::string> editor_command(const fs::path& file);
