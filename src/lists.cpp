#include "lists.hpp"

#include "db.hpp"

static const char* kDefaultKeywords =
    "# One keyword per line. Lines starting with # are ignored.\n"
    "# These are the only values allowed in an item's `tags` field (via 'e'),\n"
    "# to keep tagging consistent and typo-free. Add new ones here first.\n"
    "fluid-mechanics\n"
    "extensional-rheology\n";

static const char* kDefaultProjects =
    "# One project name per line. Lines starting with # are ignored.\n"
    "# These are the only names allowed when adding a paper to a project\n"
    "# (via 'm') or filtering to one (via ':name' in search), to keep names\n"
    "# consistent and typo-free. Add new ones here first.\n";

// Non-comment, non-blank lines of a list file.
static std::set<std::string> read_names(const fs::path& file, bool lowercase) {
    std::set<std::string> names;
    for (const auto& raw : split(read_file(file), "\n")) {
        std::string line = trim(raw);
        if (line.empty() || line.starts_with("#")) continue;
        names.insert(lowercase ? lower(line) : line);
    }
    return names;
}

// Append a name on its own line (adding the missing newline first if needed).
static void append_name(const fs::path& file, const std::string& name) {
    std::string text = read_file(file);
    if (!text.empty() && text.back() != '\n') text += '\n';
    write_file(file, text + name + "\n");
}

fs::path keywords_file() { return app_dir() / "keywords.txt"; }

void ensure_keywords_file() {
    fs::create_directories(app_dir());
    if (!fs::exists(keywords_file())) write_file(keywords_file(), kDefaultKeywords);
}

std::set<std::string> load_keywords() {
    ensure_keywords_file();
    return read_names(keywords_file(), false);
}

void add_keyword(const std::string& name) {
    if (load_keywords().contains(name)) return;
    append_name(keywords_file(), name);
}

std::vector<std::string> unknown_tags(const std::string& tags) {
    std::vector<std::string> bad;
    if (tags.empty()) return bad;
    std::set<std::string> known = load_keywords();
    for (const auto& t : split(tags, ";")) {
        std::string s = trim(t);
        if (!s.empty() && !known.contains(s)) bad.push_back(s);
    }
    return bad;
}

fs::path projects_file() { return app_dir() / "projects.txt"; }

void ensure_projects_file() {
    fs::create_directories(app_dir());
    if (fs::exists(projects_file())) return;
    fs::path old = app_dir() / "manuscripts.txt";  // the file's old name
    write_file(projects_file(), fs::exists(old) ? read_file(old) : kDefaultProjects);
}

std::set<std::string> load_projects() {
    ensure_projects_file();
    return read_names(projects_file(), true);
}

void add_project(const std::string& name) {
    if (load_projects().contains(name)) return;
    append_name(projects_file(), name);
}
