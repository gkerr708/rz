// rz: a terminal front end for a paper library, replacing Zotero's UI.
//
// Layout (the same as the Python/Textual version this replaces):
//
//    ⭘                Zotero TUI — 217 items · 201 with PDF
//   ╭──────────────────────────────────────────────────────────────╮
//   │  Search title, author, tag, abstract...                       │
//   ╰──────────────────────────────────────────────────────────────╯
//   ╭ papers, or authors (A) ──────────╮╭ the highlighted one ─────╮
//   │ * Smith & Jones (2001) Title…     ││  Title                    │
//   ╰──────────────────────────────────╯╰──────────────────────────╯
//    status line
//   ⏎ Open PDF / Back to Papers  q Quit  / Search  ? Help  A Authors ⇄ Papers
//
// How it fits together:
//   db.cpp      - the SQLite library: papers, PDFs, projects, author profiles
//   bibtex.cpp  - reading a pasted BibTeX entry, writing .bib files
//   edit.cpp    - the TOML files papers and authors are edited in ($EDITOR)
//   lists.cpp   - keywords.txt / projects.txt, the allowed tag/project names
//   rolodex.cpp - matching an author's name across papers
//   text.cpp    - styled, word-wrapped lines and shared drawing pieces
//   main.cpp    - this file: all the state, key handling and drawing
//
// As in crate: plain C++ state lives in `App`, draw() builds the screen from
// it and on_event() changes it. FTXUI redraws after every event.

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "bibtex.hpp"
#include "db.hpp"
#include "edit.hpp"
#include "lists.hpp"
#include "rolodex.hpp"
#include "text.hpp"
#include "util.hpp"

using namespace ftxui;
using Clock = std::chrono::steady_clock;

// =================================================================
// Papers: how they're shown, searched and sorted
// =================================================================
static fs::path zotero_dir() { return home_dir() / "Zotero"; }
static fs::path downloads_dir() { return home_dir() / "Downloads"; }

static const char* kNewItemTemplate =
    "% Paste a BibTeX entry below (e.g. Google Scholar's 'Cite' -> BibTeX),\n"
    "% or just fill in the fields by hand. Save and quit when done.\n"
    "\n"
    "@article{,\n"
    "  title = {},\n"
    "  author = {},\n"
    "  year = {},\n"
    "  journal = {},\n"
    "  doi = {},\n"
    "  url = {},\n"
    "}\n";

// "Last, First; Last, First" -> each entry, trimmed, blanks dropped.
static std::vector<std::string> author_list(const std::string& authors) {
    std::vector<std::string> out;
    for (const auto& a : split(authors, ";"))
        if (std::string t = trim(a); !t.empty()) out.push_back(t);
    return out;
}

static std::vector<std::string> tag_list(const std::string& tags) { return author_list(tags); }

static std::vector<std::string> last_names(const Item& it) {
    std::vector<std::string> out;
    for (const auto& a : author_list(it.authors)) out.push_back(trim(split(a, ",")[0]));
    return out;
}

// "Smith", "Smith & Jones", "Smith ... Wu"
static std::string condensed_authors(const Item& it) {
    std::vector<std::string> last = last_names(it);
    if (last.empty()) return "Unknown";
    if (last.size() == 1) return last[0];
    if (last.size() == 2) return last[0] + " & " + last[1];
    return last.front() + " ... " + last.back();
}

// Zotero stores dates as "<sql date> <original text>" (e.g. "1982-09-10
// 1982/09/10"): show only the original.
static std::string display_date(const std::string& value) {
    auto space = value.find(' ');
    if (value.empty() || space == std::string::npos) return value;
    static const std::regex sql_date(R"(\d{4}(-\d{2}){0,2})");
    if (std::regex_match(value.substr(0, space), sql_date)) return value.substr(space + 1);
    return value;
}

static std::string search_text(const Item& it) {
    std::vector<std::string> parts;
    for (const std::string* f : {&it.title, &it.authors, &it.tags, &it.publication_title, &it.type_name, &it.date,
                                 &it.abstract, &it.summary, &it.doi, &it.publisher})
        if (!f->empty()) parts.push_back(*f);
    return lower(join(parts, " "));
}

// Title matches count most, then authors and tags, then anything else.
static int match_score(const Item& it, const std::vector<std::string>& words) {
    std::string title = lower(it.title), authors = lower(it.authors), tags = lower(it.tags), rest = search_text(it);
    int score = 0;
    for (const auto& w : words) {
        if (contains(title, w)) score += 4;
        if (contains(authors, w)) score += 2;
        if (contains(tags, w)) score += 2;
        if (contains(rest, w)) score += 1;
    }
    return score;
}

static const std::vector<std::string> kPapersSorts = {"Date added", "A-Z", "Publication date"};
static const std::vector<std::string> kAuthorsSorts = {"Profiled first", "A-Z", "Most papers"};

static std::string sort_key_author(const Item& it) {
    if (it.authors.empty()) return "\xEF\xBF\xBF";  // U+FFFF: after everything
    return lower(trim(split(trim(split(it.authors, ";")[0]), ",")[0]));
}

// A file name that works on Linux, macOS and Windows.
static std::string safe_name(const std::string& text, size_t limit = 90) {
    std::string s;
    for (unsigned char c : text) s += (c < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) ? ' ' : c;
    s = trim_chars(collapse_ws(s), " .");
    if (cp_count(s) > limit) {
        s = cp_slice(s, 0, limit);
        if (auto sp = s.rfind(' '); sp != std::string::npos) s = s.substr(0, sp);
        while (!s.empty() && std::string_view(" ,;-").find(s.back()) != std::string_view::npos) s.pop_back();
    }
    return s;
}

// "Koeppel et al 2018 - Title", "Smith & Jones 2001 - Title"
static std::string share_filename(const Item& it) {
    std::vector<std::string> last;
    for (const auto& n : last_names(it)) if (!n.empty()) last.push_back(n);
    std::string who = last.empty() ? "Unknown"
                    : last.size() == 1 ? last[0]
                    : last.size() == 2 ? last[0] + " & " + last[1]
                                       : last[0] + " et al";
    std::smatch m;
    static const std::regex year_re(R"(\d{4})");
    std::string year = std::regex_search(it.date, m, year_re) ? m.str() : "n.d.";
    return safe_name(who + " " + year + " - " + (it.title.empty() ? "Untitled" : it.title), 120);
}

// Title split into exactly two lines of `width` characters (the second may be blank).
static std::pair<std::string, std::string> wrap_title(const std::string& title, size_t width = 65) {
    std::string t = collapse_ws(title);
    if (cp_count(t) <= width) return {t, ""};
    std::string rest = cp_slice(t, width);
    return {cp_slice(t, 0, width), cp_count(rest) > width ? cp_slice(rest, 0, width - 1) + "…" : rest};
}

static std::string plural(int n, const std::string& one, const std::string& many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The newest PDF in ~/Downloads ('def' at the PDF prompts).
static std::optional<fs::path> latest_download_pdf() {
    std::error_code ec;
    if (!fs::is_directory(downloads_dir(), ec)) return std::nullopt;
    std::optional<fs::path> best;
    fs::file_time_type best_time{};
    for (const auto& e : fs::directory_iterator(downloads_dir(), ec)) {
        if (e.path().extension() != ".pdf") continue;
        auto t = e.last_write_time(ec);
        if (!best || t > best_time) {
            best = e.path();
            best_time = t;
        }
    }
    return best;
}

// Every *.pdf directly inside a folder, sorted by name.
static std::vector<fs::path> pdfs_in(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".pdf") out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// A temporary file holding `content`, kept after we're done with it.
static fs::path temp_file(const std::string& prefix, const std::string& suffix, const std::string& content) {
    const char* tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/" + prefix + "XXXXXXXX" + suffix;
    int fd = mkstemps(path.data(), static_cast<int>(suffix.size()));
    if (fd < 0) throw std::runtime_error("Couldn't create a temporary file");
    close(fd);
    write_file(path, content);
    return path;
}

// Python's input(): ask on the plain terminal. Only inside App::suspended().
// nullopt on end of input (Ctrl-D).
static std::optional<std::string> ask(const std::string& question) {
    std::cout << question << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::cin.clear();
        std::cout << "\n";
        return std::nullopt;
    }
    return trim(answer);
}

// =================================================================
// Authors
// =================================================================
// "Last, First" -> "First Last"
static std::string display_name_from_entry(const std::string& raw) {
    auto comma = raw.find(',');
    if (comma == std::string::npos) return trim(raw);
    std::string last = trim(raw.substr(0, comma)), first = trim(raw.substr(comma + 1));
    return first.empty() ? last : trim(first + " " + last);
}

struct AuthorEntry {
    NameKey key;
    int record;              // index into App::authors
    std::vector<int> items;  // indices into App::items
};

// =================================================================
// A one-line text box (the search box and the pickers' filter boxes)
// =================================================================
struct LineEdit {
    std::string value;
    int cursor = 0;         // in characters
    bool selected = false;  // all selected: typing replaces it

    std::vector<std::string> glyphs() const { return Utf8ToGlyphs(value); }
    void set(std::string v) {
        value = std::move(v);
        cursor = static_cast<int>(glyphs().size());
        selected = false;
    }
    void select_all() {
        cursor = static_cast<int>(glyphs().size());
        selected = !value.empty();
    }

    // Returns true when the key was for the text box (even if nothing changed).
    bool on_event(const Event& e) {
        std::vector<std::string> g = glyphs();
        int n = static_cast<int>(g.size());
        cursor = std::clamp(cursor, 0, n);
        auto rebuild = [&] {
            value.clear();
            for (const auto& s : g) value += s;
        };
        auto take_selection = [&] {
            if (!selected) return false;
            g.clear();
            cursor = 0;
            selected = false;
            return true;
        };
        if (e.is_character()) {
            take_selection();
            g.insert(g.begin() + cursor, e.character());
            ++cursor;
        } else if (e == Event::Backspace) {
            if (!take_selection() && cursor > 0) g.erase(g.begin() + --cursor);
        } else if (e == Event::Delete || e == Event::CtrlD) {
            if (!take_selection() && cursor < n) g.erase(g.begin() + cursor);
        } else if (e == Event::ArrowLeft) {
            cursor = std::max(0, cursor - 1);
        } else if (e == Event::ArrowRight) {
            cursor = std::min(n, cursor + 1);
        } else if (e == Event::Home || e == Event::CtrlA) {
            cursor = 0;
        } else if (e == Event::End || e == Event::CtrlE) {
            cursor = n;
        } else if (e == Event::CtrlU) {  // delete to the start
            g.erase(g.begin(), g.begin() + cursor);
            cursor = 0;
        } else if (e == Event::CtrlK) {  // delete to the end
            g.erase(g.begin() + cursor, g.end());
        } else if (e == Event::CtrlW) {  // delete the word before the cursor
            int i = cursor;
            while (i > 0 && g[i - 1] == " ") --i;
            while (i > 0 && g[i - 1] != " ") --i;
            g.erase(g.begin() + i, g.begin() + cursor);
            cursor = i;
        } else {
            return false;
        }
        if (!e.is_character() && e != Event::Backspace && e != Event::Delete && e != Event::CtrlD) selected = false;
        rebuild();
        return true;
    }

    // `width` columns: the text (scrolled so the cursor shows), or the placeholder.
    Element draw(int width, bool focused, const std::string& placeholder, Color fg, Color hint, Color bg) const {
        width = std::max(1, width);
        Elements cells;
        int used = 0;
        auto cursor_cell = [](const std::string& g) {
            return text(g) | color(pal::background) | bgcolor(pal::cursor);
        };
        if (value.empty()) {
            std::vector<std::string> ph = Utf8ToGlyphs(placeholder);
            for (int i = 0; i < static_cast<int>(ph.size()) && used < width; ++i) {
                cells.push_back(focused && i == 0 ? cursor_cell(ph[i]) : text(ph[i]) | color(hint));
                used += string_width(ph[i]);
            }
            if (focused && ph.empty()) {
                cells.push_back(cursor_cell(" "));
                ++used;
            }
        } else {
            std::vector<std::string> g = glyphs();
            int n = static_cast<int>(g.size());
            int c = std::clamp(cursor, 0, n);
            // Scroll so the cursor (and its cell after the text) stays visible.
            int start = 0, span = 0;
            for (int i = c; i >= 0; --i) {
                int w = i < n ? string_width(g[i]) : 1;
                if (span + w > width) break;
                span += w;
                start = i;
            }
            for (int i = start; i <= n && used < width; ++i) {
                std::string s = i < n ? g[i] : " ";
                if (i == n && !(focused && c == n)) break;
                Element cell = text(s) | color(fg);
                if (selected && i < n) cell = cell | bgcolor(Color::RGB(0x40, 0x3D, 0x52));
                if (focused && i == c) cell = cursor_cell(s);
                cells.push_back(cell);
                used += string_width(s);
            }
        }
        if (used < width) cells.push_back(text(std::string(width - used, ' ')));
        return hbox(std::move(cells)) | size(WIDTH, EQUAL, width) | bgcolor(bg);
    }
};

// =================================================================
// A scrolling list whose rows may wrap onto several lines
// =================================================================
struct ListView {
    int highlighted = -1;          // -1 = nothing
    int scroll = 0;                // first visible line
    bool reveal = true;            // scroll the highlighted row into view on the next draw
    int page = 10;                 // rows that fit, for PageUp/PageDown
    std::vector<int> line_rows;    // the row each visible line belongs to (-1 = blank)
    Box box;                       // where the lines landed on screen

    void reset(int n) {
        highlighted = n > 0 ? 0 : -1;
        scroll = 0;
        reveal = true;
    }
    void move_to(int i, int n) {
        highlighted = n > 0 ? std::clamp(i, 0, n - 1) : -1;
        reveal = true;
    }
    void move_by(int delta, int n) { move_to(highlighted < 0 ? 0 : highlighted + delta, n); }
    int row_at(int x, int y) const {
        if (!box.Contain(x, y)) return -1;
        size_t i = y - box.y_min;
        return i < line_rows.size() ? line_rows[i] : -1;
    }

    // Inside a border: 1 column of padding, the rows, a scrollbar when they
    // don't fit, 1 more column of padding (Textual's OptionList).
    Element draw(const std::vector<Line>& rows, int width, int height, bool focused) {
        height = std::max(1, height);
        int content = std::max(1, width - 2);
        std::vector<std::vector<Line>> wrapped;
        auto wrap_rows = [&] {
            wrapped.clear();
            for (const Line& r : rows) wrapped.push_back(wrap(r, content));
        };
        wrap_rows();
        std::vector<int> start(rows.size() + 1, 0);
        auto count = [&] {
            for (size_t i = 0; i < rows.size(); ++i) start[i + 1] = start[i] + static_cast<int>(wrapped[i].size());
            return start.back();
        };
        int total = count();
        bool bar = total > height;
        if (bar) {
            content = std::max(1, width - 4);
            wrap_rows();
            total = count();
        }

        if (reveal && highlighted >= 0 && highlighted < static_cast<int>(rows.size())) {
            if (start[highlighted + 1] > scroll + height) scroll = start[highlighted + 1] - height;
            if (start[highlighted] < scroll) scroll = start[highlighted];
        }
        reveal = false;
        scroll = std::clamp(scroll, 0, std::max(0, total - height));

        Elements lines;
        line_rows.clear();
        size_t r = 0;
        int fully_visible = 0;
        for (int li = scroll; li < scroll + height; ++li) {
            while (r < rows.size() && start[r + 1] <= li) ++r;
            if (r >= rows.size()) {
                lines.push_back(text(std::string(content, ' ')));
                line_rows.push_back(-1);
                continue;
            }
            if (li == start[r] && start[r + 1] <= scroll + height) ++fully_visible;
            Element e = render(wrapped[r][li - start[r]], content);
            if (static_cast<int>(r) == highlighted)
                e = e | bgcolor(focused ? pal::select_focus : pal::select_blur) | bold;
            lines.push_back(e);
            line_rows.push_back(static_cast<int>(r));
        }
        page = std::max(1, fully_visible);
        Elements parts{blank(1, height), vbox(std::move(lines)) | reflect(box)};
        if (bar) parts.push_back(scrollbar(height, total, scroll));
        parts.push_back(blank(1, height));
        return hbox(std::move(parts));
    }
};

// =================================================================
// Pickers: the filter picker (f, Shift+Tab) and tag / project pickers (t, m)
// =================================================================
struct Picker {
    enum Kind { Filter, Tag, Project } kind = Filter;
    struct Option {
        std::string id;  // "clear", "project:x", "keyword:x", "create:x", "pick:x"
        Line label;
    };
    LineEdit input;
    bool list_focused = false;
    std::vector<Option> options;
    ListView list;
    Box input_box;
    // Filter picker
    std::set<std::pair<std::string, std::string>> selected;
    std::vector<std::pair<std::string, std::string>> entries;  // (label, "project:x")
    // Tag / project picker
    int item_id = 0;
    std::set<std::string> known, current, pending;

    std::string noun() const { return kind == Tag ? "tag" : "project"; }

    static Line checkbox(bool checked) {
        return checked ? line("[x]", pal::green) : line("[ ]", pal::overlay0);
    }

    void render_options(const std::string& query, const std::string& preserve = "") {
        options.clear();
        int target = 0;
        if (kind == Filter) {
            if (!selected.empty()) options.push_back({"clear", line("x  Clear all filters")});
            std::string q = lower(trim(query));
            for (const auto& [label, id] : entries) {
                if (!q.empty() && !contains(lower(label), q)) continue;
                auto colon = id.find(':');
                std::string kind_name = id.substr(0, colon), name = id.substr(colon + 1);
                Line l = checkbox(selected.contains({kind_name, name}));
                l.push_back({" " + std::string(kind_name == "project" ? "PR" : "KW") + " ", pal::text});
                l.push_back({name, kind_name == "project" ? pal::sapphire : pal::mauve});
                options.push_back({id, l});
            }
            if (options.empty()) return list.reset(0);
            if (!preserve.empty()) {
                target = options.size() > 1 ? 1 : 0;
                for (size_t i = 0; i < options.size(); ++i) if (options[i].id == preserve) target = static_cast<int>(i);
            } else if (!selected.empty() && options.size() > 1) {
                target = 1;  // skip past "Clear all filters"
            }
        } else {
            std::string q = trim(query), ql = lower(q);
            bool exact = false;
            for (const auto& n : known) if (lower(n) == ql) exact = true;
            if (!q.empty() && !exact) options.push_back({"create:" + q, line("+ Create '" + q + "'", pal::green)});
            for (const auto& n : known) {
                if (!ql.empty() && !contains(lower(n), ql)) continue;
                bool checked = current.contains(n) != pending.contains(n);
                Line l = checkbox(checked);
                l.push_back({" " + n, pal::text});
                if (pending.contains(n)) l.push_back({" *", pal::yellow});
                options.push_back({"pick:" + n, l});
            }
            if (options.empty()) return list.reset(0);
            // Start on the first real match, so "ext" lands on "extensional-rheology".
            target = options.size() > 1 && options[0].id.starts_with("create:") ? 1 : 0;
            for (size_t i = 0; i < options.size(); ++i) if (!preserve.empty() && options[i].id == preserve) target = static_cast<int>(i);
        }
        list.reset(static_cast<int>(options.size()));
        list.move_to(target, static_cast<int>(options.size()));
    }

    // Space / Enter / Tab on an option: mark or unmark it.
    void toggle() {
        if (list.highlighted < 0 || list.highlighted >= static_cast<int>(options.size())) return;
        std::string id = options[list.highlighted].id;
        if (kind == Filter) {
            if (id == "clear") {
                selected.clear();
            } else if (auto colon = id.find(':'); colon != std::string::npos) {
                std::pair<std::string, std::string> key{id.substr(0, colon), id.substr(colon + 1)};
                if (!selected.erase(key)) selected.insert(key);
            }
            render_options(input.value, id == "clear" ? "" : id);
        } else {
            std::string name;
            if (id.starts_with("create:")) {
                name = id.substr(7);
                known.insert(name);
            } else if (id.starts_with("pick:")) {
                name = id.substr(5);
            } else {
                return;
            }
            if (!pending.erase(name)) pending.insert(name);
            render_options(input.value, "pick:" + name);
        }
    }

    void focus_search() {
        list_focused = false;
        input.select_all();  // typing the next search replaces the old one
    }
};

// =================================================================
// The app
// =================================================================
enum class Area { Search, List, Detail };
enum class Severity { Information, Warning, Error };

struct Toast {
    std::string message;
    Severity severity;
    Clock::time_point until;
};

struct App {
    ScreenInteractive& screen;
    explicit App(ScreenInteractive& s) : screen(s) {}

    // --- library ---
    std::vector<Item> items;           // every paper, newest first
    std::vector<int> filtered;         // papers shown: indices into `items`
    std::vector<Author> authors;       // every row of the authors table
    std::map<NameKey, int> authors_by_key;
    std::map<NameKey, NameKey> canonical;  // a name -> the record it's shown under
    std::vector<AuthorEntry> author_entries;
    std::vector<int> filtered_authors;  // authors shown: indices into author_entries
    std::vector<std::pair<std::string, std::string>> active_filters;  // ("project"|"keyword"|"author"|"queue", name)
    std::set<int> marked;               // item ids marked with Space, for S
    bool authors_mode = false;
    int papers_sort = 0, authors_sort = 0;

    // --- UI ---
    Area focus = Area::List;
    LineEdit search;
    ListView list;
    std::vector<Line> detail;  // the right panel, before wrapping
    int detail_scroll = 0, detail_page = 10, detail_max = 0;
    std::string status = "Loading library...";
    std::vector<Toast> toasts;
    std::atomic<bool> has_toasts = false;
    std::optional<Picker> picker;
    bool help_open = false;
    int help_scroll = 0, help_max = 0;
    int W = 140, H = 40;  // screen size, updated every draw
    Box search_box, detail_box;
    struct Hint {
        std::string key, desc;
        std::function<void()> action;
    };
    std::vector<Hint> hints;          // the footer's key hints, last draw
    std::array<Box, 8> hint_boxes{};  // ...and where they landed, for clicks

    // =================================================================
    // Small helpers
    // =================================================================
    void set_status(std::string s) { status = std::move(s); }

    void notify(std::string message, Severity severity = Severity::Information, int seconds = 5) {
        toasts.push_back({std::move(message), severity, Clock::now() + std::chrono::seconds(seconds)});
        has_toasts = true;
    }

    void expire_toasts() {
        auto now = Clock::now();
        std::erase_if(toasts, [&](const Toast& t) { return t.until <= now; });
        has_toasts = !toasts.empty();
    }

    // Run `fn` with the terminal back to normal (Textual's suspend()), for
    // $EDITOR and the questions asked on the plain terminal.
    void suspended(const std::function<void()>& fn) { screen.WithRestoredIO(fn)(); }

    Item* highlighted_item() {
        if (authors_mode || list.highlighted < 0 || list.highlighted >= static_cast<int>(filtered.size())) return nullptr;
        return &items[filtered[list.highlighted]];
    }
    AuthorEntry* highlighted_author() {
        if (!authors_mode || list.highlighted < 0 || list.highlighted >= static_cast<int>(filtered_authors.size()))
            return nullptr;
        return &author_entries[filtered_authors[list.highlighted]];
    }
    Item* item_by_id(int id) {
        for (Item& it : items) if (it.id == id) return &it;
        return nullptr;
    }
    NameKey resolve(const NameKey& k) const {
        auto it = canonical.find(k);
        return it == canonical.end() ? k : it->second;
    }
    const Author* author_for(const NameKey& k) const {
        auto it = authors_by_key.find(k);
        return it == authors_by_key.end() ? nullptr : &authors[it->second];
    }

    bool require_papers_mode() {
        if (!authors_mode) return true;
        notify("Switch to papers view (Shift+A) first", Severity::Warning);
        return false;
    }

    // =================================================================
    // Loading
    // =================================================================
    void load_library() {
        init_db();
        items = load_items();
        sync_authors();
        if (authors_mode) {
            build_author_entries();
            apply_author_filter(search.value);
        } else {
            apply_filter(search.value);
        }
    }

    // Make sure every author on a paper has a profile row, then reload them.
    void sync_authors() {
        std::vector<std::pair<NameKey, std::string>> entries;
        std::set<NameKey> seen;
        for (const Item& it : items)
            for (const auto& raw : author_list(it.authors)) {
                NameKey key = author_key_from_entry(raw);
                if (seen.insert(key).second) entries.emplace_back(key, display_name_from_entry(raw));
            }
        ensure_authors(entries, load_seed_contacts());
        reload_authors();
    }

    void reload_authors() {
        authors = list_authors();
        authors_by_key.clear();
        std::map<long long, int> by_id;
        for (int i = 0; i < static_cast<int>(authors.size()); ++i) {
            authors_by_key[{authors[i].last_key, authors[i].first_key}] = i;
            by_id[authors[i].id] = i;
        }
        // Follow canonical_id links ("G. H. McKinley" -> "Gareth H. McKinley").
        canonical.clear();
        for (const Author& a : authors) {
            const Author* r = &a;
            std::set<long long> visited;
            while (r->canonical_id && visited.insert(r->id).second) {
                auto next = by_id.find(*r->canonical_id);
                if (next == by_id.end()) break;
                r = &authors[next->second];
            }
            canonical[{a.last_key, a.first_key}] = {r->last_key, r->first_key};
        }
    }

    void build_author_entries() {
        author_entries.clear();
        std::map<NameKey, int> index;
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
            for (const auto& raw : author_list(items[i].authors)) {
                NameKey key = resolve(author_key_from_entry(raw));
                auto rec = authors_by_key.find(key);
                if (rec == authors_by_key.end()) continue;
                auto [it, fresh] = index.try_emplace(key, static_cast<int>(author_entries.size()));
                if (fresh) author_entries.push_back({key, rec->second, {}});
                author_entries[it->second].items.push_back(i);
            }
    }

    // =================================================================
    // Papers view: filtering
    // =================================================================
    std::string filter_label() const {
        if (active_filters.empty()) return "";
        std::vector<std::string> parts;
        for (const auto& [kind, name] : active_filters) {
            if (kind == "queue") parts.push_back("Queue");
            else parts.push_back((kind == "project" ? "PR" : kind == "keyword" ? "KW" : kind == "author" ? "AU" : "?") +
                                 std::string(":") + name);
        }
        return join(parts, " + ") + " · ";
    }

    std::string subtitle() const {
        if (authors_mode) {
            int profiled = 0;
            for (const auto& e : author_entries) if (authors[e.record].has_profile()) ++profiled;
            return "Authors · " + std::to_string(author_entries.size()) + " people · " + std::to_string(profiled) +
                   " with profile info";
        }
        int with_pdf = 0;
        for (const Item& it : items) if (it.has_pdf()) ++with_pdf;
        return filter_label() + std::to_string(items.size()) + " items · " + std::to_string(with_pdf) + " with PDF";
    }

    // The papers the active filters leave (before the search).
    std::vector<int> scope_items() const {
        std::vector<int> scope;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) scope.push_back(i);
        for (const auto& [kind, name] : active_filters) {
            std::function<bool(const Item&)> keep;
            std::set<int> ids;
            NameKey target;
            if (kind == "project") {
                ids = project_item_ids(name);
                keep = [&](const Item& it) { return ids.contains(it.id); };
            } else if (kind == "keyword") {
                keep = [&](const Item& it) {
                    auto tags = tag_list(it.tags);
                    return std::find(tags.begin(), tags.end(), name) != tags.end();
                };
            } else if (kind == "author") {
                target = resolve(name_key(name));
                keep = [&](const Item& it) {
                    for (const auto& a : author_list(it.authors))
                        if (resolve(author_key_from_entry(a)) == target) return true;
                    return false;
                };
            } else if (kind == "queue") {
                keep = [](const Item& it) { return it.to_read; };
            } else {
                continue;
            }
            std::erase_if(scope, [&](int i) { return !keep(items[i]); });
        }
        return scope;
    }

    void sort_items(std::vector<int>& v) const {
        auto by = [&](auto key, bool descending) {
            std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
                return descending ? key(items[a]) > key(items[b]) : key(items[a]) < key(items[b]);
            });
        };
        if (papers_sort == 1) by([](const Item& it) { return std::pair{sort_key_author(it), it.title}; }, false);
        else if (papers_sort == 2) by([](const Item& it) { return it.date; }, true);
        else by([](const Item& it) { return it.date_added; }, true);
    }

    void apply_filter(const std::string& query) {
        if (items.empty() && active_filters.empty()) {
            filtered.clear();
            set_status("No items yet — run `rz migrate` to import your library");
            rebuild_list();
            return;
        }
        std::vector<int> scope = scope_items();
        std::string q = trim(query);
        if (!q.empty()) {
            std::vector<std::string> words = split_ws(lower(q));
            std::vector<std::pair<int, int>> scored;  // (index, score)
            for (int i : scope) {
                std::string text = search_text(items[i]);
                if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return contains(text, w); }))
                    scored.emplace_back(i, match_score(items[i], words));
            }
            std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.second > b.second; });
            filtered.clear();
            for (auto& [i, _] : scored) filtered.push_back(i);
            set_status(filter_label() + std::to_string(filtered.size()) + " of " + std::to_string(scope.size()) +
                       " match");
        } else {
            filtered = scope;
            sort_items(filtered);
            int with_pdf = 0;
            for (int i : scope) if (items[i].has_pdf()) ++with_pdf;
            set_status(filter_label() + std::to_string(filtered.size()) + " items · " + std::to_string(with_pdf) +
                       " with PDF · Sort: " + kPapersSorts[papers_sort]);
        }
        rebuild_list();
    }

    // New contents for the list: back to the first row.
    void rebuild_list() {
        list.reset(static_cast<int>(authors_mode ? filtered_authors.size() : filtered.size()));
        show_detail();
    }

    // =================================================================
    // Authors view
    // =================================================================
    std::string author_search_text(const AuthorEntry& e) const {
        const Author& a = authors[e.record];
        std::vector<std::string> parts;
        for (const std::string* f : {&a.name, &a.email, &a.lab, &a.department, &a.university, &a.location,
                                     &a.research_areas, &a.notes, &a.known_for})
            if (!f->empty()) parts.push_back(lower(*f));
        return join(parts, " ");
    }

    void sort_authors(std::vector<int>& v) const {
        auto key = [&](int i) { return author_entries[i].key; };
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
            const AuthorEntry &x = author_entries[a], &y = author_entries[b];
            if (authors_sort == 2 && x.items.size() != y.items.size()) return x.items.size() > y.items.size();
            if (authors_sort == 0) {
                bool px = authors[x.record].has_profile(), py = authors[y.record].has_profile();
                if (px != py) return px;
            }
            return key(a) < key(b);
        });
    }

    void apply_author_filter(const std::string& query) {
        std::vector<std::string> words = split_ws(lower(trim(query)));
        std::vector<int> matched;
        for (int i = 0; i < static_cast<int>(author_entries.size()); ++i) {
            std::string text = author_search_text(author_entries[i]);
            if (std::all_of(words.begin(), words.end(), [&](const std::string& w) { return contains(text, w); }))
                matched.push_back(i);
        }
        sort_authors(matched);
        filtered_authors = matched;
        rebuild_list();
        int profiled = 0;
        for (int i : matched) if (authors[author_entries[i].record].has_profile()) ++profiled;
        set_status("Authors · " + std::to_string(matched.size()) + " of " + std::to_string(author_entries.size()) +
                   " · " + std::to_string(profiled) + " with profile info · Sort: " + kAuthorsSorts[authors_sort]);
    }

    void toggle_mode() {
        authors_mode = !authors_mode;
        if (authors_mode) {
            build_author_entries();
            apply_author_filter(search.value);
        } else {
            apply_filter(search.value);
        }
    }

    // c: the authors view, showing only this paper's authors.
    void show_paper_authors() {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        if (it->authors.empty()) {
            notify("No authors on this paper", Severity::Warning);
            return;
        }
        std::set<NameKey> keys;
        for (const auto& raw : author_list(it->authors)) keys.insert(resolve(author_key_from_entry(raw)));
        std::string short_title = truncate(it->title, 40);
        authors_mode = true;
        build_author_entries();
        filtered_authors.clear();
        for (int i = 0; i < static_cast<int>(author_entries.size()); ++i)
            if (keys.contains(author_entries[i].key)) filtered_authors.push_back(i);
        sort_authors(filtered_authors);
        rebuild_list();
        set_status("Authors of '" + short_title + "' · " + std::to_string(filtered_authors.size()));
    }

    void cycle_sort() {
        if (authors_mode) {
            authors_sort = (authors_sort + 1) % 3;
            apply_author_filter(search.value);
        } else {
            papers_sort = (papers_sort + 1) % 3;
            apply_filter(search.value);
        }
    }

    // Enter on an author: their papers.
    void filter_to_author(const AuthorEntry& e) {
        std::string name = authors[e.record].name;
        authors_mode = false;
        active_filters = {{"author", name}};
        search.set("");
        apply_filter("");
    }

    void open_website() {
        AuthorEntry* e = highlighted_author();
        if (!e) return;
        std::vector<std::string> urls;
        for (const auto& u : split(authors[e->record].urls, "; ")) if (!u.empty()) urls.push_back(u);
        if (urls.empty()) {
            notify("No website on file for this author", Severity::Warning);
            return;
        }
        std::string url = urls[0];
        static const std::regex scheme(R"(^[a-zA-Z][a-zA-Z0-9+.-]*://.*)");
        if (!std::regex_match(url, scheme)) url = "https://" + url;
        spawn_detached({"xdg-open", url});
        set_status("Opened " + url);
    }

    void edit_author() {
        AuthorEntry* e = highlighted_author();
        if (!e) return;
        NameKey key = e->key;
        Author record = authors[e->record];
        fs::path tmp = temp_file("rip-zotero-author-", ".toml", author_to_toml(record));
        suspended([&] { run_wait(editor_command(tmp)); });

        Changes fields;
        try {
            fields = parse_author_toml(read_file(tmp));
        } catch (const EditError& err) {
            notify(std::string(err.what()) + " Edits kept at " + tmp.string(), Severity::Error, 10);
            return;
        }
        std::string name;
        for (const auto& [f, v] : fields) if (f == "name") name = v;
        if (name.empty()) {
            notify("Name cannot be empty — edit not applied", Severity::Warning);
            return;
        }
        Changes changes;
        for (const auto& [f, v] : fields) if (v != *record.field(f)) changes.emplace_back(f, v);
        std::error_code ec;
        if (changes.empty()) {
            fs::remove(tmp, ec);
            set_status("No changes");
            return;
        }
        update_author_fields(record.id, changes);
        fs::remove(tmp, ec);

        reload_authors();
        build_author_entries();
        apply_author_filter(search.value);
        for (int i = 0; i < static_cast<int>(filtered_authors.size()); ++i)
            if (author_entries[filtered_authors[i]].key == key) {
                list.move_to(i, static_cast<int>(filtered_authors.size()));
                show_detail();
            }
        set_status("Saved profile: " + name);
    }

    // =================================================================
    // The detail panel
    // =================================================================
    // A "Label       value" row: value cut to `width`, or NaN when empty.
    static Line row(const std::string& label, const std::string& value, Color c = pal::text, size_t width = 55) {
        Line l{{label + std::string(std::max<int>(0, 11 - static_cast<int>(cp_count(label))), ' '), pal::overlay1},
               {" ", pal::plain}};
        if (value.empty()) l.push_back({"NaN", pal::overlay0});
        else l.push_back({truncate(value, width), c});
        return l;
    }

    // Profiles (with something filled in) of this paper's authors.
    std::vector<const Author*> author_records_for(const std::string& author_str) const {
        std::vector<const Author*> out;
        std::set<long long> seen;
        for (const auto& entry : author_list(author_str)) {
            const Author* a = author_for(resolve(author_key_from_entry(entry)));
            if (a && !seen.contains(a->id) && a->has_profile()) {
                out.push_back(a);
                seen.insert(a->id);
            }
        }
        return out;
    }

    std::vector<Line> build_detail(const Item& it) const {
        std::vector<Line> parts;
        auto [l1, l2] = it.title.empty() ? std::pair<std::string, std::string>{"NaN", ""} : wrap_title(it.title);
        Color title_color = it.title.empty() ? pal::overlay0 : pal::yellow;
        parts.push_back(line(l1, title_color, true));
        parts.push_back(l2.empty() ? Line{} : line(l2, title_color, true));
        parts.push_back({});

        if (it.to_read) parts.push_back({{"Queue      ", pal::overlay1}, {" ", pal::plain}, {"★ To read", pal::yellow}});
        parts.push_back(row("Date", display_date(it.date)));
        parts.push_back(row("Author", it.authors));
        for (const Author* a : author_records_for(it.authors)) {
            std::vector<std::string> bits;
            if (!(a->lab.empty() ? a->department : a->lab).empty()) bits.push_back(a->lab.empty() ? a->department : a->lab);
            if (!a->university.empty()) bits.push_back(a->university);
            std::string url;
            for (const auto& u : split(a->urls, "; ")) if (!u.empty() && url.empty()) url = u;
            Line l{{"Profile    ", pal::overlay1}, {" ", pal::plain}, {a->name, pal::sapphire, false, url}};
            if (!bits.empty()) l.push_back({" — " + join(bits, ", "), pal::plain});
            parts.push_back(l);
        }
        std::string journal = !it.publication_title.empty() ? it.publication_title
                            : !it.book_title.empty()        ? it.book_title
                                                            : it.proceedings_title;
        parts.push_back(row("Journal", journal));
        parts.push_back(row("Type", it.type_name));
        parts.push_back(row("Tags", it.tags, pal::mauve));
        parts.push_back(row("Projects", join(projects_for_item(it.id), "; "), pal::sapphire));

        auto pdfs = list_item_pdfs(it.id);
        if (pdfs.size() > 1) parts.push_back(row("PDF", "Yes (" + std::to_string(pdfs.size()) + " files)", pal::green));
        else if (it.has_pdf()) parts.push_back(row("PDF", "Yes", pal::green));
        else if (!it.pdf_path.empty()) parts.push_back(row("PDF", "Missing", pal::red));
        else parts.push_back(row("PDF", ""));

        // Free text: shown in full, unlike the cut-off rows above.
        for (auto [label, value] : {std::pair{"Summary", &it.summary}, std::pair{"Abstract", &it.abstract}}) {
            if (value->empty()) continue;
            parts.push_back({});
            parts.push_back(line(label, pal::overlay1));
            for (const auto& l : textwrap(*value, 65)) parts.push_back(line(l, pal::text));
        }
        return parts;
    }

    // [(author, papers together)] for an author's co-authors, most first.
    std::vector<std::pair<const Author*, int>> co_authors(const AuthorEntry& e) const {
        std::map<NameKey, int> counts;
        for (int i : e.items)
            for (const auto& raw : author_list(items[i].authors)) {
                NameKey k = resolve(author_key_from_entry(raw));
                if (k != e.key) ++counts[k];
            }
        std::vector<std::pair<const Author*, int>> out;
        for (const auto& [k, n] : counts) if (const Author* a = author_for(k)) out.emplace_back(a, n);
        std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first->name < b.first->name;
        });
        return out;
    }

    std::vector<Line> build_author_detail(const AuthorEntry& e) const {
        const Author& r = authors[e.record];
        std::vector<Line> parts{line(r.name, pal::yellow, true), {}};
        auto row70 = [&](const std::string& label, const std::string& value, Color c = pal::text) {
            parts.push_back(row(label, value, c, 70));
        };
        row70("Email", r.email);
        row70("Lab", r.lab.empty() ? r.department : r.lab);
        row70("Institution", r.university);
        row70("Location", r.location);
        if (!r.research_areas.empty()) row70("Areas", replace_all(r.research_areas, "; ", ", "), pal::mauve);
        std::vector<std::string> urls;
        for (const auto& u : split(r.urls, "; ")) if (!u.empty()) urls.push_back(u);
        if (!urls.empty())
            parts.push_back({{"Website    ", pal::overlay1}, {" ", pal::plain}, {urls[0], pal::sapphire, false, urls[0]}});
        if (!r.notes.empty()) row70("Notes", r.notes);
        if (!r.known_for.empty()) {
            parts.push_back({});
            parts.push_back(line("Known for", pal::overlay1));
            for (const auto& l : textwrap(r.known_for, 70)) parts.push_back(line(l, pal::text));
        }
        if (!r.has_profile()) parts.push_back(line("No profile info yet — press 'e' to fill it in", pal::overlay0));

        int count = static_cast<int>(e.items.size());
        parts.push_back({});
        parts.push_back(line("Papers (" + std::to_string(count) + ")", pal::overlay1));
        for (int k = 0; k < std::min(count, 10); ++k) {
            const Item& it = items[e.items[k]];
            parts.push_back(line("  - " + truncate(it.title.empty() ? "Untitled" : it.title, 70), pal::text));
        }
        if (count > 10) parts.push_back(line("  … and " + std::to_string(count - 10) + " more", pal::overlay0));

        auto co = co_authors(e);
        if (!co.empty()) {
            parts.push_back({});
            parts.push_back(line("Co-authors (" + std::to_string(co.size()) + ")", pal::overlay1));
            for (size_t k = 0; k < std::min<size_t>(co.size(), 10); ++k) {
                auto [a, n] = co[k];
                parts.push_back({{"  - ", pal::text},
                                 {a->name, a->has_profile() ? pal::blue : pal::text},
                                 {" (" + plural(n, "paper", "papers") + " together)", pal::overlay0}});
            }
            if (co.size() > 10) parts.push_back(line("  … and " + std::to_string(co.size() - 10) + " more", pal::overlay0));
        }
        return parts;
    }

    // Show the highlighted paper / author on the right, scrolled to the top.
    void show_detail() {
        detail.clear();
        if (Item* it = highlighted_item()) detail = build_detail(*it);
        else if (AuthorEntry* e = highlighted_author()) detail = build_author_detail(*e);
        detail_scroll = 0;
    }

    void show_item_detail(const Item& it) {
        detail = build_detail(it);
        detail_scroll = 0;
    }

    // =================================================================
    // Papers: opening, editing, adding, deleting
    // =================================================================
    void open_pdf(const fs::path& p) {
        std::error_code ec;
        if (fs::exists(p, ec)) spawn_detached({"zathura", p.string()});
    }

    // Enter on a paper: open its PDF, asking which when there are several.
    void activate_item() {
        Item* it = highlighted_item();
        if (!it) return;
        auto pdfs = list_item_pdfs(it->id);
        if (pdfs.size() > 1) return choose_and_open_pdf(*it, pdfs);
        if (it->has_pdf()) {
            open_pdf(it->pdf_path);
            set_status("Opened: " + fs::path(it->pdf_path).filename().string());
        } else {
            notify("No PDF attached for this item", Severity::Warning);
        }
    }

    void choose_and_open_pdf(const Item& it, const std::vector<std::pair<std::string, std::string>>& pdfs) {
        std::string choice;
        suspended([&] {
            std::cout << "Multiple PDFs for '" << it.title << "':\n";
            for (size_t i = 0; i < pdfs.size(); ++i)
                std::cout << "  " << i + 1 << ". "
                          << (pdfs[i].second.empty() ? fs::path(pdfs[i].first).filename().string() : pdfs[i].second)
                          << "\n";
            choice = ask("Choose number (blank to cancel): ").value_or("");
        });
        if (choice.empty()) return set_status("Cancelled");
        int n = 0;
        bool digits = std::all_of(choice.begin(), choice.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits && choice.size() < 6) n = std::stoi(choice);
        if (n < 1 || n > static_cast<int>(pdfs.size())) {
            notify("Invalid choice", Severity::Warning);
            return;
        }
        auto [path, label] = pdfs[n - 1];
        open_pdf(path);
        set_status("Opened: " + (label.empty() ? fs::path(path).filename().string() : label));
    }

    void edit_item() {
        if (authors_mode) return edit_author();
        Item* it = highlighted_item();
        if (!it) return;
        fs::path tmp = temp_file("rip-zotero-", ".toml", item_to_toml(*it));
        suspended([&] { run_wait(editor_command(tmp)); });

        Changes changes;
        try {
            changes = diff_fields(*it, parse_toml_fields(read_file(tmp)));
            if (changes.empty()) return set_status("No changes");
            for (const auto& [f, v] : changes) {
                if (f != "tags") continue;
                if (auto bad = unknown_tags(v); !bad.empty()) {
                    notify("Unknown keyword(s): " + join(bad, ", ") + " — add them via 'K' first. Edits kept at " +
                               tmp.string(),
                           Severity::Error, 10);
                    return;
                }
            }
            update_item_fields(it->id, changes);
        } catch (const EditError& err) {
            notify(std::string(err.what()) + " Edits kept at " + tmp.string(), Severity::Error, 10);
            return;
        }
        std::error_code ec;
        fs::remove(tmp, ec);
        std::vector<std::string> names;
        for (const auto& [f, v] : changes) {
            *it->field(f) = v;
            names.push_back(f);
        }
        show_item_detail(*it);
        set_status("Saved: " + join(names, ", "));
    }

    // Reload everything from disk, keeping a message in the status line.
    void reload(const std::string& message) {
        load_library();
        set_status(message);
    }

    void add_item() {
        if (!require_papers_mode()) return;
        fs::path tmp = temp_file("rip-zotero-new-", ".bib", kNewItemTemplate);
        std::string pdf_input;
        suspended([&] {
            run_wait(editor_command(tmp));
            pdf_input = ask("PDF file or directory (blank to skip, 'def' = latest ~/Downloads PDF, a directory "
                            "attaches every PDF inside): ")
                            .value_or("");
        });
        std::string text = read_file(tmp);
        std::error_code ec;
        fs::remove(tmp, ec);

        Changes fields = parse_bibtex(text);
        std::string title = fields[0].second;
        if (title.empty()) {
            notify("No BibTeX entry found — nothing added", Severity::Warning);
            return;
        }
        int id = create_item(fields);

        std::optional<fs::path> src;
        if (lower(pdf_input) == "def") {
            src = latest_download_pdf();
            if (!src) notify("No PDF found in ~/Downloads", Severity::Warning);
        } else if (!pdf_input.empty()) {
            src = expand_user(pdf_input);
        }
        if (src) {
            if (!fs::exists(*src, ec)) {
                notify("Path not found: " + src->string(), Severity::Warning);
            } else if (fs::is_directory(*src, ec)) {
                auto files = pdfs_in(*src);
                if (files.empty()) notify("No PDFs found in " + src->string(), Severity::Warning);
                for (int i = 0; i < static_cast<int>(files.size()); ++i) {
                    fs::path dest;
                    if (i == 0) {
                        dest = save_pdf(id, files[i]);
                        update_item_fields(id, {{"pdf_path", dest.string()}});
                    } else {
                        dest = save_extra_pdf(id, files[i], i);
                    }
                    add_item_pdf(id, dest.string(), files[i].stem().string(), i);
                }
            } else {
                fs::path dest = save_pdf(id, *src);
                update_item_fields(id, {{"pdf_path", dest.string()}});
            }
        }
        reload("Added: " + title);
    }

    void add_pdf() {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        std::string pdf_input, label;
        suspended([&] {
            auto answer = ask("PDF file or directory to attach to '" + it->title +
                              "' (blank to cancel, 'def' = latest ~/Downloads PDF, a directory attaches every PDF "
                              "inside): ");
            if (!answer) return;
            pdf_input = *answer;
            std::error_code ec;
            bool is_dir = !pdf_input.empty() && lower(pdf_input) != "def" && fs::is_directory(expand_user(pdf_input), ec);
            if (!pdf_input.empty() && !is_dir) {
                auto l = ask("Label for this PDF (blank = filename): ");
                if (!l) pdf_input.clear();
                else label = *l;
            }
        });
        if (pdf_input.empty()) return set_status("Cancelled");

        std::vector<std::pair<fs::path, std::string>> sources;
        std::error_code ec;
        if (lower(pdf_input) == "def") {
            auto src = latest_download_pdf();
            if (!src) return notify("No PDF found in ~/Downloads", Severity::Warning);
            sources.emplace_back(*src, label);
        } else {
            fs::path src = expand_user(pdf_input);
            if (!fs::exists(src, ec)) return notify("Path not found: " + src.string(), Severity::Warning);
            if (fs::is_directory(src, ec)) {
                auto files = pdfs_in(src);
                if (files.empty()) return notify("No PDFs found in " + src.string(), Severity::Warning);
                for (const auto& f : files) sources.emplace_back(f, f.stem().string());
            } else {
                sources.emplace_back(src, label);
            }
        }

        auto existing = list_item_pdfs(it->id);
        if (existing.empty() && it->has_pdf()) {
            // The first extra PDF: list the original one too, so both show up.
            add_item_pdf(it->id, it->pdf_path, "", 0);
            existing = list_item_pdfs(it->id);
        }
        int next = static_cast<int>(existing.size());
        std::vector<std::string> attached;
        std::optional<fs::path> first;
        for (const auto& [file, file_label] : sources) {
            if (!fs::exists(file, ec)) {
                notify("PDF not found: " + file.string(), Severity::Warning);
                continue;
            }
            fs::path dest = save_extra_pdf(it->id, file, next);
            add_item_pdf(it->id, dest.string(), file_label, next);
            attached.push_back(file_label.empty() ? dest.filename().string() : file_label);
            if (!first) first = dest;
            ++next;
        }
        if (attached.empty()) return set_status("Nothing attached");
        if (!it->has_pdf() && first) {
            update_item_fields(it->id, {{"pdf_path", first->string()}});
            it->pdf_path = first->string();
            it->check_pdf();
        }
        set_status(attached.size() == 1 ? "Attached: " + attached[0] + " -> " + it->title
                                        : "Attached " + std::to_string(attached.size()) + " PDFs -> " + it->title);
        show_item_detail(*it);
    }

    void delete_item() {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        bool confirmed = false;
        suspended([&] { confirmed = lower(ask("Delete '" + it->title + "'? [y/N] ").value_or("")) == "y"; });
        if (!confirmed) return set_status("Delete cancelled");
        std::string title = it->title;
        ::delete_item(it->id);
        reload("Deleted: " + title);
    }

    void toggle_read() {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        it->to_read = !it->to_read;
        update_item_fields(it->id, {{"to_read", it->to_read ? "1" : "0"}});
        show_item_detail(*it);
        set_status(std::string(it->to_read ? "Added to" : "Removed from") + " reading queue: " + it->title);
    }

    void toggle_queue_filter() {
        if (!require_papers_mode()) return;
        std::pair<std::string, std::string> q{"queue", ""};
        auto pos = std::find(active_filters.begin(), active_filters.end(), q);
        if (pos != active_filters.end()) active_filters.erase(pos);
        else active_filters.push_back(q);
        apply_filter(search.value);
    }

    // =================================================================
    // Tags and projects
    // =================================================================
    void open_name_picker(Picker::Kind kind) {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        Picker p;
        p.kind = kind;
        p.item_id = it->id;
        if (kind == Picker::Project) {
            p.known = load_projects();
            for (const auto& n : projects_for_item(it->id)) p.current.insert(n);
        } else {
            p.known = load_keywords();
            for (const auto& t : tag_list(it->tags)) p.current.insert(t);
        }
        p.render_options("");
        picker = std::move(p);
    }

    void apply_name_pick(Picker::Kind kind, int item_id, const std::set<std::string>& names) {
        Item* it = item_by_id(item_id);
        if (!it) return;
        std::string short_title = truncate(it->title, 40);
        if (names.empty()) return set_status("Cancelled");
        std::vector<std::string> added, removed;
        if (kind == Picker::Project) {
            for (const auto& raw : names) {
                std::string name = lower(trim(raw));
                if (name.empty()) continue;
                if (!load_projects().contains(name)) add_project(name);
                (toggle_project_item(name, it->id) ? added : removed).push_back(name);
            }
        } else {
            std::vector<std::string> current = tag_list(it->tags);
            for (const auto& raw : names) {
                std::string name = trim(raw);
                if (name.empty()) continue;
                if (!load_keywords().contains(name)) add_keyword(name);
                if (auto pos = std::find(current.begin(), current.end(), name); pos != current.end()) {
                    current.erase(pos);
                    removed.push_back(name);
                } else {
                    current.push_back(name);
                    added.push_back(name);
                }
            }
            it->tags = join(current, "; ");
            update_item_fields(it->id, {{"tags", it->tags}});
        }
        show_item_detail(*it);
        std::vector<std::string> parts;
        if (!added.empty()) parts.push_back("added " + join(added, ", "));
        if (!removed.empty()) parts.push_back("removed " + join(removed, ", "));
        set_status(std::string(kind == Picker::Project ? "Projects" : "Tags") + " for '" + short_title +
                   "': " + join(parts, "; "));
    }

    void open_filter(const std::string& initial_query) {
        if (!require_papers_mode()) return;
        Picker p;
        p.kind = Picker::Filter;
        for (const auto& f : active_filters) p.selected.insert(f);
        for (const auto& n : load_projects()) p.entries.emplace_back("Project: " + n, "project:" + n);
        for (const auto& n : load_keywords()) p.entries.emplace_back("Keyword: " + n, "keyword:" + n);
        p.input.set(initial_query);
        p.render_options(initial_query);
        picker = std::move(p);
    }

    // q / Esc in a picker: apply what's marked and close.
    void finish_picker() {
        Picker p = std::move(*picker);
        picker.reset();
        if (p.kind == Picker::Filter) {
            active_filters.assign(p.selected.begin(), p.selected.end());
            search.set("");
            apply_filter("");
        } else {
            apply_name_pick(p.kind, p.item_id, p.pending);
        }
    }

    // Enter in the search box: a keyword or project name becomes a filter.
    void submit_search() {
        std::string name = lower(trim(search.value));
        if (!authors_mode && !name.empty()) {
            std::optional<std::pair<std::string, std::string>> match;
            if (load_projects().contains(name)) match = {"project", name};
            else if (load_keywords().contains(name)) match = {"keyword", name};
            if (match) {
                active_filters = {*match};
                search.set("");
                apply_filter("");
            }
        }
        focus = Area::List;
    }

    // Esc / q: back to the list, dropping any filters.
    void focus_list() {
        focus = Area::List;
        if (active_filters.empty()) return;
        active_filters.clear();
        if (authors_mode) apply_author_filter(search.value);
        else apply_filter(search.value);
    }

    // Ctrl+R: back to how it starts, reloaded from disk.
    void reset() {
        authors_mode = false;
        active_filters.clear();
        marked.clear();
        papers_sort = authors_sort = 0;
        search.set("");
        focus = Area::List;
        load_library();
    }

    // K / M: edit keywords.txt / projects.txt, then tidy the papers to match.
    void edit_list(bool keywords) {
        if (keywords) ensure_keywords_file();
        else ensure_projects_file();
        std::set<std::string> before = keywords ? load_keywords() : load_projects();
        fs::path file = keywords ? keywords_file() : projects_file();
        suspended([&] { run_wait(editor_command(file)); });
        std::set<std::string> after = keywords ? load_keywords() : load_projects();

        std::vector<std::string> removed, added;
        std::set_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(removed));
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(added));
        std::string what = keywords ? "tag" : "project";
        if (removed.size() == 1 && added.size() == 1) {
            // One name swapped for another: probably a rename.
            bool yes = false;
            suspended([&] {
                yes = lower(ask("Rename " + what + " '" + removed[0] + "' -> '" + added[0] +
                                "' on every paper? [y/N] ")
                                .value_or("")) == "y";
            });
            if (yes) {
                int changed = keywords ? rename_tag(removed[0], added[0]) : rename_project(removed[0], added[0]);
                return reload("Renamed " + what + " '" + removed[0] + "' -> '" + added[0] + "' on " +
                              std::to_string(changed) + " item(s)");
            }
        }
        int n = keywords ? cleanup_tags(after) : cleanup_projects(after);
        if (!n) return set_status(keywords ? "Keyword list updated" : "Project list updated");
        reload(keywords ? "Keyword list updated · removed stale tags from " + std::to_string(n) + " item(s)"
                        : "Project list updated · removed " + std::to_string(n) + " project(s) no longer listed");
    }

    // =================================================================
    // Exporting and sharing
    // =================================================================
    std::vector<const Item*> filtered_items() const {
        std::vector<const Item*> out;
        for (int i : filtered) out.push_back(&items[i]);
        return out;
    }

    void export_bibtex() {
        if (!require_papers_mode()) return;
        if (filtered.empty()) return notify("Nothing to export in the current view", Severity::Warning);
        std::string out;
        suspended([&] {
            out = ask("Export " + std::to_string(filtered.size()) +
                      " item(s) to .bib file path (blank to cancel, go back): ")
                      .value_or("");
        });
        if (out.empty()) return set_status("Export cancelled");
        fs::path path = expand_user(out);
        if (!write_file(path, items_to_bibtex(filtered_items())))
            return notify("Couldn't write " + path.string(), Severity::Error);
        set_status("Exported " + std::to_string(filtered.size()) + " item(s) to " + path.string());
    }

    void copy_bibtex() {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        copy_to_clipboard(items_to_bibtex({it}));
        set_status("Copied BibTeX to clipboard: " + truncate(it->title, 40));
    }

    // The PDFs to open/copy/share for a paper: its list when it has
    // several, otherwise the main one.
    std::vector<std::pair<fs::path, std::string>> pdf_sources(const Item& it) const {
        auto pdfs = list_item_pdfs(it.id);
        std::vector<std::pair<fs::path, std::string>> out;
        if (pdfs.size() <= 1 && !it.pdf_path.empty()) {
            out.emplace_back(it.pdf_path, "");
        } else {
            for (const auto& [p, l] : pdfs) out.emplace_back(expand_user(p), l);
        }
        return out;
    }

    void copy_pdf_path() {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        std::vector<std::string> paths;
        for (const auto& [p, _] : pdf_sources(*it)) paths.push_back(p.string());
        if (paths.empty()) return notify("No PDF attached for this item", Severity::Warning);
        copy_to_clipboard(join(paths, "\n"));
        set_status(paths.size() == 1 ? "Copied PDF path: " + paths[0]
                                     : "Copied " + std::to_string(paths.size()) + " PDF paths (one per line)");
    }

    void mark_item() {
        if (!require_papers_mode()) return;
        Item* it = highlighted_item();
        if (!it) return;
        if (!marked.erase(it->id)) marked.insert(it->id);
        if (list.highlighted + 1 < static_cast<int>(filtered.size()))
            list.move_to(list.highlighted + 1, static_cast<int>(filtered.size())), show_detail();
        set_status(marked_status());
    }

    std::string marked_status() const {
        if (marked.empty()) return "No papers marked";
        return std::to_string(marked.size()) + " marked · S: share as folder of PDFs · U: unmark all";
    }

    void clear_marks() {
        marked.clear();
        if (!authors_mode) rebuild_list();
        set_status(marked_status());
    }

    // S: copy the marked papers' PDFs (or the highlighted one's) into a new
    // folder as "Author Year - Title.pdf", with a references.bib, and zip it.
    void share_marked() {
        if (!require_papers_mode()) return;
        std::vector<const Item*> chosen;
        if (!marked.empty()) {
            for (const Item& it : items) if (marked.contains(it.id)) chosen.push_back(&it);
        } else if (Item* it = highlighted_item()) {
            chosen.push_back(it);
        } else {
            return;
        }
        std::string fallback = "~/Downloads/papers-" + today_iso();
        std::string answer, zip_answer;
        suspended([&] {
            auto a = ask("Share " + std::to_string(chosen.size()) + " paper(s): folder to create [Enter = " + fallback +
                         ", q = cancel]: ");
            answer = a.value_or("q");
            if (a && lower(answer) != "q")
                zip_answer = lower(ask("Also make a .zip of it (opens natively on Windows/macOS)? [Y/n]: ").value_or(""));
        });
        if (lower(answer) == "q") return set_status("Share cancelled");

        fs::path out_dir = expand_user(answer.empty() ? fallback : answer);
        std::error_code ec;
        auto nonempty = [&](const fs::path& p) { return fs::exists(p, ec) && !fs::is_empty(p, ec); };
        if (nonempty(out_dir)) {
            fs::path base = out_dir;
            for (int n = 2; nonempty(out_dir); ++n)
                out_dir = base.parent_path() / (base.filename().string() + "-" + std::to_string(n));
        }
        fs::create_directories(out_dir);

        int copied = 0;
        std::vector<std::string> missing;
        std::set<std::string> used;
        for (const Item* it : chosen) {
            auto sources = pdf_sources(*it);
            std::erase_if(sources, [&](const auto& s) { return !fs::exists(s.first, ec); });
            if (sources.empty()) {
                missing.push_back(it->title.empty() ? "(untitled)" : it->title);
                continue;
            }
            std::string stem = share_filename(*it);
            for (size_t i = 0; i < sources.size(); ++i) {
                std::string label = safe_name(sources[i].second);
                std::string name = sources.size() == 1 ? stem
                                 : stem + " (" + (label.empty() ? std::to_string(i + 1) : label) + ")";
                std::string final_name = name;
                for (int k = 2; used.contains(lower(final_name)); ++k) final_name = name + " " + std::to_string(k);
                used.insert(lower(final_name));
                copy_file_with_times(sources[i].first, out_dir / (final_name + ".pdf"));
                ++copied;
            }
        }
        write_file(out_dir / "references.bib", items_to_bibtex(chosen));
        if (!missing.empty()) {
            std::string text = "No PDF in the library for:\n";
            for (const auto& t : missing) text += "- " + t + "\n";
            write_file(out_dir / "MISSING_PDFS.txt", text);
        }
        fs::path shared = out_dir;
        if (zip_answer != "n" && zip_answer != "no") {
            // Zipped next to the folder, with the folder inside it, so it
            // unpacks as one folder rather than loose files.
            fs::path zip = out_dir.string() + ".zip";
            fs::remove(zip, ec);
            int rc = run_wait({"zip", "-r", "-q", zip.string(), out_dir.filename().string()}, out_dir.parent_path());
            if (rc != 0) rc = run_wait({"bsdtar", "-a", "-cf", zip.string(), out_dir.filename().string()}, out_dir.parent_path());
            if (rc == 0) shared = zip;
            else notify("Couldn't make the .zip (is zip installed?) — the folder is ready", Severity::Warning);
        }
        copy_to_clipboard(shared.string());
        marked.clear();
        rebuild_list();
        std::string msg = "Shared " + std::to_string(copied) + " PDF(s) to " + shared.string() + " (path copied)";
        if (!missing.empty())
            msg += " · " + std::to_string(missing.size()) + " paper(s) had no PDF, see MISSING_PDFS.txt";
        set_status(msg);
    }

    // =================================================================
    // Keys
    // =================================================================
    bool on_event(Event e) {
        reap_children();
        expire_toasts();
        if (e == Event::Custom) return true;  // a redraw request (toast expired)
        try {
            if (e.is_mouse()) return on_mouse(e);
            if (help_open) return on_help_key(e);
            if (picker) return on_picker_key(e);
            // App-wide keys no widget uses.
            if (e == Event::CtrlR) return reset(), true;
            if (e == Event::CtrlS) return cycle_sort(), true;
            if (focus == Area::Search) return on_search_key(e);
            if (focus == Area::Detail && on_detail_key(e)) return true;
            if (focus == Area::List && on_list_key(e)) return true;
            return on_app_key(e);
        } catch (const std::exception& ex) {
            notify(ex.what(), Severity::Error, 10);
            return true;
        }
    }

    // Keys that work from the list and the detail panel alike.
    bool on_app_key(const Event& e) {
        std::string c = e.is_character() ? e.character() : "";
        if (c == "/") focus = Area::Search;
        else if (c == "?") help_open = true, help_scroll = 0;
        else if (c == "A") toggle_mode();
        else if (c == "q") screen.Exit();
        else if (e == Event::Escape) focus_list();
        else if (e == Event::Tab) focus = focus == Area::List ? Area::Detail : Area::Search;
        else if (e == Event::TabReverse) focus = focus == Area::Detail ? Area::List : Area::Search;
        else return false;
        return true;
    }

    bool on_search_key(const Event& e) {
        if (e == Event::Escape) focus_list();
        else if (e == Event::Return) submit_search();
        else if (e == Event::TabReverse) open_filter(search.value);
        else if (e == Event::Tab) focus = Area::List;
        else if (search.on_event(e)) {
            if (authors_mode) apply_author_filter(search.value);
            else apply_filter(search.value);
        } else {
            return false;
        }
        return true;
    }

    bool on_detail_key(const Event& e) {
        std::string c = e.is_character() ? e.character() : "";
        auto scroll_to = [&](int y) { detail_scroll = std::clamp(y, 0, detail_max); };
        if (c == "j" || e == Event::ArrowDown) scroll_to(detail_scroll + 1);
        else if (c == "k" || e == Event::ArrowUp) scroll_to(detail_scroll - 1);
        else if (c == " " || e == Event::CtrlD || e == Event::PageDown) scroll_to(detail_scroll + detail_page);
        else if (e == Event::CtrlU || e == Event::PageUp) scroll_to(detail_scroll - detail_page);
        else if (c == "g" || e == Event::Home) scroll_to(0);
        else if (c == "G" || e == Event::End) scroll_to(detail_max);
        else if (c == "h" || e == Event::Escape) focus = Area::List;  // filters stay
        else return false;
        return true;
    }

    bool on_list_key(const Event& e) {
        std::string c = e.is_character() ? e.character() : "";
        int n = static_cast<int>(authors_mode ? filtered_authors.size() : filtered.size());
        auto go = [&](int i) {
            int before = list.highlighted;
            list.move_to(i, n);
            if (list.highlighted != before) show_detail();
        };
        bool papers = !authors_mode;
        // Keys for one view only do nothing in the other.
        auto papers_only = [&](auto fn) { if (papers) fn(); };

        if (c == "j" || e == Event::ArrowDown) go(list.highlighted < 0 ? 0 : list.highlighted + 1);
        else if (c == "k" || e == Event::ArrowUp) go(list.highlighted < 0 ? 0 : list.highlighted - 1);
        else if (c == "g" || e == Event::Home) go(0);
        else if (c == "G" || e == Event::End) go(n - 1);
        else if (e == Event::PageDown) go(list.highlighted + list.page);
        else if (e == Event::PageUp) go(list.highlighted - list.page);
        else if (e == Event::Return) {
            if (AuthorEntry* a = highlighted_author()) filter_to_author(*a);
            else activate_item();
        }
        else if (c == "o") show_detail();
        else if (c == "l") focus = Area::Detail;
        else if (c == "e") edit_item();
        else if (c == "K") edit_list(true);
        else if (c == "M") edit_list(false);
        else if (c == "w") { if (authors_mode) open_website(); }
        else if (c == "a") papers_only([&] { add_item(); });
        else if (c == "p") papers_only([&] { add_pdf(); });
        else if (c == "d") papers_only([&] { delete_item(); });
        else if (c == "m") papers_only([&] { open_name_picker(Picker::Project); });
        else if (c == "t") papers_only([&] { open_name_picker(Picker::Tag); });
        else if (c == "c") papers_only([&] { show_paper_authors(); });
        else if (c == "x") papers_only([&] { copy_bibtex(); });
        else if (c == "y") papers_only([&] { copy_pdf_path(); });
        else if (c == "X") papers_only([&] { export_bibtex(); });
        else if (c == "f") papers_only([&] { open_filter(""); });
        else if (c == "r") papers_only([&] { toggle_read(); });
        else if (c == "R") papers_only([&] { toggle_queue_filter(); });
        else if (c == " ") papers_only([&] { mark_item(); });
        else if (c == "S") papers_only([&] { share_marked(); });
        else if (c == "U") papers_only([&] { clear_marks(); });
        else return false;
        return true;
    }

    // The help screen: j/k scroll, any other key closes it.
    bool on_help_key(const Event& e) {
        if (e == Event::Character('j')) help_scroll = std::min(help_scroll + 1, help_max);
        else if (e == Event::Character('k')) help_scroll = std::max(help_scroll - 1, 0);
        else help_open = false;
        return true;
    }

    bool on_picker_key(const Event& e) {
        Picker& p = *picker;
        int n = static_cast<int>(p.options.size());
        std::string c = e.is_character() ? e.character() : "";
        if (e == Event::Escape) finish_picker();
        else if (e == Event::Return) {
            if (p.list_focused) p.toggle();
            else p.list_focused = true;
        }
        else if (e == Event::Tab) p.toggle();
        else if (e == Event::TabReverse) p.list_focused = !p.list_focused;
        else if (e == Event::ArrowDown) p.list.move_by(1, n);
        else if (e == Event::ArrowUp) p.list.move_by(-1, n);
        else if (p.list_focused) {
            if (c == "j") p.list.move_by(1, n);
            else if (c == "k") p.list.move_by(-1, n);
            else if (c == "q") finish_picker();
            else if (c == " ") p.toggle();
            else if (c == "/") p.focus_search();
            else if (e == Event::Home) p.list.move_to(0, n);
            else if (e == Event::End) p.list.move_to(n - 1, n);
            else if (e == Event::PageDown) p.list.move_by(p.list.page, n);
            else if (e == Event::PageUp) p.list.move_by(-p.list.page, n);
        } else if (p.input.on_event(e)) {
            p.render_options(p.input.value);
        }
        return true;
    }

    // Mouse: click to focus / pick, wheel to scroll.
    bool on_mouse(Event e) {
        const Mouse& m = e.mouse();
        bool click = m.button == Mouse::Left && m.motion == Mouse::Pressed;
        int wheel = m.button == Mouse::WheelDown ? 3 : m.button == Mouse::WheelUp ? -3 : 0;
        if (!click && !wheel) return false;

        if (help_open) {
            if (wheel) help_scroll = std::clamp(help_scroll + wheel, 0, help_max);
            return true;
        }
        if (picker) {
            Picker& p = *picker;
            if (wheel) {
                p.list.scroll += wheel;
            } else if (int r = p.list.row_at(m.x, m.y); r >= 0) {
                // A click marks the option and applies straight away.
                p.list.move_to(r, static_cast<int>(p.options.size()));
                p.toggle();
                finish_picker();
            } else if (p.input_box.Contain(m.x, m.y)) {
                p.list_focused = false;
            }
            return true;
        }
        if (wheel) {
            if (list.box.Contain(m.x, m.y)) list.scroll += wheel;
            else if (detail_box.Contain(m.x, m.y)) detail_scroll = std::clamp(detail_scroll + wheel, 0, detail_max);
            return true;
        }
        for (size_t i = 0; i < hints.size() && i < hint_boxes.size(); ++i)
            if (hint_boxes[i].Contain(m.x, m.y)) return hints[i].action(), true;
        if (search_box.Contain(m.x, m.y)) {
            focus = Area::Search;
        } else if (int r = list.row_at(m.x, m.y); r >= 0) {
            // A click picks the row, like Enter (opens the PDF / the author's papers).
            focus = Area::List;
            int n = static_cast<int>(authors_mode ? filtered_authors.size() : filtered.size());
            list.move_to(r, n);
            show_detail();
            if (AuthorEntry* a = highlighted_author()) filter_to_author(*a);
            else activate_item();
        } else if (detail_box.Contain(m.x, m.y)) {
            focus = Area::Detail;
        }
        return true;
    }

    // =================================================================
    // Drawing
    // =================================================================
    Element draw_header() {
        std::string title = "Zotero TUI", sub = subtitle();
        int len = text_width(title) + 3 + text_width(sub);
        int start = std::max(3, (W - len) / 2 - 1);  // where Textual centres it
        return render({{" ⭘", pal::text}, {std::string(start - 2, ' ')}, {title, pal::text}, {" — ", pal::subtitle},
                       {sub, pal::subtitle}},
                      W);
    }

    Element draw_search() {
        bool focused = focus == Area::Search;
        Element field = search.draw(W - 8, focused, "Search title, author, tag, abstract...", pal::plain,
                                    pal::placeholder, pal::background);
        Element box = rounded(hbox({blank(2), field, blank(2)}), focused ? pal::border_focus : pal::border) |
                      reflect(search_box);
        return hbox({blank(1), box, blank(1)});
    }

    std::vector<Line> list_rows() const {
        std::vector<Line> rows;
        if (authors_mode) {
            for (int i : filtered_authors) {
                const AuthorEntry& e = author_entries[i];
                const Author& a = authors[e.record];
                Line l = a.has_profile() ? Line{{"*", pal::green}, {" "}, {a.name, pal::blue, true}}
                                         : Line{{"-", pal::overlay0}, {" "}, {a.name, pal::text}};
                l.push_back({" "});
                l.push_back({"(" + plural(static_cast<int>(e.items.size()), "paper", "papers") + ")", pal::overlay0});
                std::vector<std::string> bits;
                if (!a.university.empty()) bits.push_back(a.university);
                if (!a.location.empty()) bits.push_back(a.location);
                if (!bits.empty()) l.push_back({" " + join(bits, ", "), pal::text});
                rows.push_back(l);
            }
            return rows;
        }
        for (int i : filtered) {
            const Item& it = items[i];
            Line l;
            if (marked.contains(it.id)) l.push_back({"● ", pal::green, true});
            if (it.to_read) l.insert(l.end(), {{"★", pal::yellow}, {" "}});
            std::string year = it.date.empty() ? "n.d." : cp_slice(it.date, 0, 4);
            std::string author = condensed_authors(it), title = cp_slice(it.title, 0, 80);
            bool book = it.type_name == "book";
            if (it.has_pdf()) {
                l.insert(l.end(), {{"*", pal::green}, {" "}, {author, pal::blue, true}, {" "},
                                   {"(" + year + ")", pal::overlay0}, {" "}, {title, book ? pal::peach : pal::text}});
            } else if (book) {
                l.insert(l.end(), {{"- " + author + " (" + year + ") ", pal::overlay0}, {title, pal::peach}});
            } else {
                l.push_back({"- " + author + " (" + year + ") " + title, pal::overlay0});
            }
            rows.push_back(l);
        }
        return rows;
    }

    Element draw_list(int width, int height) {
        bool focused = focus == Area::List;
        Element inner = list.draw(list_rows(), width - 2, height - 2, focused);
        return rounded(inner, focused ? pal::border_focus : pal::border);
    }

    // Inside the border: 2 columns of padding each side and a blank line
    // above and below the text, then the scrollbar (Textual's Static padding).
    Element draw_detail(int width, int height) {
        bool focused = focus == Area::Detail;
        int inner_w = width - 2, inner_h = std::max(1, height - 2);
        int content = std::max(1, inner_w - 4);
        std::vector<Line> lines = wrap_all(detail, content);
        bool bar = static_cast<int>(lines.size()) + 2 > inner_h;
        if (bar) {
            content = std::max(1, inner_w - 6);
            lines = wrap_all(detail, content);
        }
        lines.insert(lines.begin(), Line{});
        lines.push_back({});
        int total = static_cast<int>(lines.size());
        detail_max = std::max(0, total - inner_h);
        detail_page = inner_h;
        detail_scroll = std::clamp(detail_scroll, 0, detail_max);
        Elements rows;
        for (int i = detail_scroll; i < detail_scroll + inner_h; ++i)
            rows.push_back(i < total ? render(lines[i], content) : text(std::string(content, ' ')));
        Elements parts{blank(2, inner_h), vbox(std::move(rows)), blank(2, inner_h)};
        if (bar) parts.push_back(scrollbar(inner_h, total, detail_scroll));
        return rounded(hbox(std::move(parts)) | reflect(detail_box), focused ? pal::border_focus : pal::border);
    }

    Element draw_status() { return render(line("  " + status, pal::muted), W); }

    Element draw_footer() {
        hints.clear();
        if (focus == Area::List) hints.push_back({"⏎", "Open PDF / Back to Papers", [this] { on_list_key(Event::Return); }});
        if (focus == Area::Detail) hints.push_back({"h", "Back to list", [this] { focus = Area::List; }});
        hints.push_back({"q", "Quit", [this] { screen.Exit(); }});
        hints.push_back({"/", "Search", [this] { focus = Area::Search; }});
        if (focus != Area::Search) {
            hints.push_back({"?", "Help", [this] { help_open = true, help_scroll = 0; }});
            hints.push_back({"A", "Authors ⇄ Papers", [this] { toggle_mode(); }});
        }
        Elements parts;
        for (size_t i = 0; i < hints.size(); ++i) {
            parts.push_back(hbox({text(" " + hints[i].key + " ") | color(pal::footer_key) | bold,
                                  text(hints[i].desc + " ") | color(pal::text)}) |
                            reflect(hint_boxes[i]));
        }
        parts.push_back(filler());
        return hbox(std::move(parts)) | size(WIDTH, EQUAL, W);
    }

    Element draw_toasts() {
        expire_toasts();
        if (toasts.empty()) return text("");
        int tw = std::min(60, W / 2);
        Elements stack;
        for (size_t i = 0; i < toasts.size(); ++i) {
            const Toast& t = toasts[i];
            Color edge = t.severity == Severity::Error ? pal::error : t.severity == Severity::Warning ? pal::warning : pal::success;
            std::vector<Line> lines = wrap(line(t.message, pal::text), tw - 3);
            lines.insert(lines.begin(), Line{});
            lines.push_back({});
            if (i) stack.push_back(blank(tw, 1));  // a gap between toasts
            for (const Line& l : lines)
                stack.push_back(hbox({text("▌") | color(edge), text(" "), render(l, tw - 3), text(" ")}) |
                                bgcolor(pal::toast_bg));
        }
        // Bottom right, above the footer.
        return vbox({filler(), hbox({filler(), vbox(std::move(stack))}), blank(W, 1)});
    }

    // A box `w` × `h`, centred on an otherwise blank screen (the modal screens).
    Element centred(Element box, int w, int h) {
        int left = std::max(0, (W - w) / 2), top = std::max(0, (H - h) / 2);
        return vbox({blank(W, top), hbox({blank(left, h), std::move(box) | size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h)}),
                     filler()});
    }

    Element draw_help() {
        std::vector<Line> content = help_lines();
        int w = std::min(86, W * 95 / 100);
        int max_h = std::max(5, H * 80 / 100);
        int inner_w = w - 2;
        int text_w = std::max(1, inner_w - 4);
        std::vector<Line> lines = wrap_all(content, text_w);
        int h = std::min(max_h, static_cast<int>(lines.size()) + 4);
        int view = h - 4;
        bool bar = static_cast<int>(lines.size()) > view;
        if (bar) {
            text_w = std::max(1, inner_w - 6);
            lines = wrap_all(content, text_w);
        }
        int total = static_cast<int>(lines.size());
        help_max = std::max(0, total - view);
        help_scroll = std::clamp(help_scroll, 0, help_max);
        Elements rows;
        for (int i = help_scroll; i < help_scroll + view; ++i)
            rows.push_back(i < total ? render(lines[i], text_w) : text(std::string(text_w, ' ')));
        Elements middle{blank(2, view), vbox(std::move(rows))};
        if (bar) middle.push_back(scrollbar(view, total, help_scroll));
        middle.push_back(blank(2, view));
        Element box = rounded(vbox({blank(inner_w, 1), hbox(std::move(middle)), blank(inner_w, 1)}), pal::border_focus);
        return centred(box, w, h);
    }

    Element draw_picker() {
        Picker& p = *picker;
        int w = std::min(50, W);
        int h = std::max(10, H * 80 / 100);
        int inner_w = w - 2, inner_h = h - 2;
        std::string hint_text = "Enter: jump to list · Space/Enter: mark · /: search again · q/Esc: apply";
        std::vector<Line> hint = wrap(line(hint_text, pal::muted), inner_w - 2);
        int list_h = std::max(3, inner_h - 3 - static_cast<int>(hint.size()));

        bool input_focused = !p.list_focused;
        std::string placeholder = p.kind == Picker::Filter ? "Type to filter…" : "Type to filter or create a " + p.noun() + "…";
        Color input_bg = input_focused ? pal::input_bg : pal::background;
        Element field = p.input.draw(inner_w - 6, input_focused, placeholder, pal::text,
                                     input_focused ? pal::placeholder_tinted : pal::placeholder, input_bg);
        Element input = tall(hbox({blank(2), field, blank(2)}), inner_w, 3,
                             input_focused ? pal::input_border : pal::border, input_bg) |
                        reflect(p.input_box);

        std::vector<Line> labels;
        for (const auto& o : p.options) labels.push_back(o.label);
        Element options = p.list.draw(labels, inner_w - 2, list_h - 2, p.list_focused);
        Element list_box = tall(options, inner_w, list_h, p.list_focused ? pal::input_border : pal::border,
                                pal::background);

        Elements hint_rows;
        for (const Line& l : hint) hint_rows.push_back(hbox({blank(1), render(l, inner_w - 2), blank(1)}));
        Element box = rounded(vbox({input, list_box, vbox(std::move(hint_rows))}) | size(HEIGHT, EQUAL, inner_h),
                              pal::border_focus);
        return centred(box, w, h);
    }

    Element draw() {
        auto term = Terminal::Size();
        W = std::max(40, term.dimx);
        H = std::max(12, term.dimy);
        Element screen_el;
        if (help_open) {
            hints.clear();
            screen_el = draw_help();
        } else if (picker) {
            hints.clear();
            screen_el = draw_picker();
        } else {
            int main_h = H - 7;
            int list_w = (W - 2) * 55 / 100;
            screen_el = vbox({
                draw_header(),
                blank(W, 1),
                draw_search(),
                hbox({blank(1, main_h), draw_list(list_w, main_h), draw_detail(W - 2 - list_w, main_h), blank(1, main_h)}),
                draw_status(),
                draw_footer(),
            });
        }
        return dbox({screen_el, draw_toasts()}) | size(WIDTH, EQUAL, W) | size(HEIGHT, EQUAL, H) |
               color(pal::plain) | bgcolor(pal::background);
    }

    // =================================================================
    // Help text
    // =================================================================
    static std::vector<Line> help_lines() {
        static const char* text = R"(#Navigation
  j / down        Move down
  k / up          Move up
  g               Jump to top
  G               Jump to bottom
  o               Show detail panel
  Escape          Focus list / cancel
  /               Focus search
  Shift+A         Toggle papers / authors view

#Authors view
  Type in search          Live-filter authors by name/lab/university/etc.
  Enter (on an author)    Filter papers list to the highlighted author
  e               Edit that author's profile (opens $EDITOR)
  w               Open that author's website in your browser
                  Each author's detail panel also lists their co-authors,
                  ranked by number of shared papers.

#Items
  e               Edit item (opens $EDITOR)
  a               Add item from BibTeX
                  (point the PDF prompt at a directory to attach every
                  PDF inside)
  p               Attach an additional PDF to an item
                  (point it at a directory to attach every PDF inside)
  d               Delete item
  m               Add/remove item from one or more projects — opens a
                  picker (type to filter, Enter to jump to the list,
                  j/k to move, Space/Enter to mark, / to search for
                  the next one, q/Esc to apply)
  t               Add/remove one or more tags on the item (same picker
                  as 'm')
  c               Jump to authors view, showing only this paper's
                  authors
  r               Add/remove item from the reading queue (★)
  R               Show only reading-queue items (toggle)

#Filtering & lists
  Type a keyword/project name in search, Enter  Apply as filter
  Shift+Tab (in search)  Open filter picker, pre-filtered by what
                  you typed — j/k to move, Space/Enter to stack
                  multiple, / to search again, q/Esc to apply
  f               Open filter picker from the list (all entries)
  K               Edit the keyword list — deleting a name unlinks it
                  from every paper; replacing exactly one name with
                  exactly one other offers to rename it everywhere
                  instead
  M               Edit the project list (same rename/removal
                  behavior as K, above)
  x               Copy the highlighted item's BibTeX to the clipboard
  Space           Mark/unmark the paper (marks stay while you search
                  and filter, so you can collect papers from several
                  searches); U unmarks all
  S               Share: copy the marked papers' PDFs (or the
                  highlighted one) into a new folder, renamed
                  "Author Year - Title.pdf", plus references.bib;
                  optionally also zipped (default yes) for email/Windows
  l               Focus the detail panel: j/k scroll, Space/Ctrl+D
                  page down, Ctrl+U page up, g/G top/bottom,
                  h/q/Esc back to the list
  y               Copy the highlighted item's PDF path to the clipboard
                  (all paths, one per line, if it has several PDFs)
  X               Export every item in the current filtered view to a
                  .bib file (prompts for a path; blank to cancel)

#Other
  Ctrl+S          Cycle sort order (papers: Date added -> A-Z ->
                  Publication date; authors: Profiled first -> A-Z ->
                  Most papers). Current sort shows in the status bar.
  Ctrl+R          Reset to the start view (clear search, filters,
                  sort; back to papers) and reload from disk
  ?               This help screen
  q               Quit

Press any key to close.
)";
        std::vector<Line> out;
        for (const auto& l : split(text, "\n")) {
            if (l.starts_with("#")) out.push_back(line(l.substr(1), pal::yellow, true));
            else out.push_back(line(l, pal::plain));
        }
        return out;
    }
};

// =================================================================
// main
// =================================================================
static int migrate() {
    fs::path zdb = zotero_dir() / "zotero.sqlite", storage = zotero_dir() / "storage";
    if (!fs::exists(zdb)) {
        std::cerr << "Zotero database not found at " << zdb.string() << "\n";
        return 1;
    }
    std::cout << "Migrating from " << zdb.string() << " -> " << app_db().string() << " ...\n";
    int count = migrate_from_zotero(zdb, storage);
    std::cout << "Migrated " << count << " items to " << app_db().string() << "\n";
    std::cout << "PDFs copied to " << app_storage().string() << "\n";
    return 0;
}

int main(int argc, char** argv) {
    // A UTF-8 locale, so lowercasing knows about accented letters.
    if (!std::setlocale(LC_CTYPE, "C.UTF-8")) std::setlocale(LC_CTYPE, "");

    std::string arg = argc > 1 ? argv[1] : "";
    if (arg == "migrate") {
        try {
            return migrate();
        } catch (const std::exception& e) {
            std::cerr << "rz: " << e.what() << "\n";
            return 1;
        }
    }
    if (!arg.empty()) {
        std::cerr << "usage: rz            browse the library\n"
                     "       rz migrate    import ~/Zotero (zotero.sqlite + storage/)\n";
        return arg == "-h" || arg == "--help" ? 0 : 2;
    }

    auto screen = ScreenInteractive::Fullscreen();
    App app(screen);
    try {
        app.load_library();
    } catch (const std::exception& e) {
        std::cerr << "rz: " << e.what() << "\n";
        return 1;
    }

    // Wakes the screen while a notification is showing, so it disappears on time.
    std::jthread ticker([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (app.has_toasts) screen.PostEvent(Event::Custom);
        }
    });

    auto ui = Renderer([&] { return app.draw(); });
    ui |= CatchEvent([&](Event e) { return app.on_event(e); });
    screen.Loop(ui);
    return 0;
}
