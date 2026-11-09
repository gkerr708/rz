#include "text.hpp"

#include <algorithm>
#include <cmath>

#include <ftxui/screen/string.hpp>

using namespace ftxui;

int text_width(const std::string& s) { return string_width(s); }

int line_width(const Line& l) {
    int w = 0;
    for (const Span& s : l) w += string_width(s.text);
    return w;
}

namespace {
struct Glyph {
    std::string g;
    int w;
    int span;
};

// Glyph runs back into spans: neighbours from the same span merge.
Line to_line(const std::vector<Glyph>& gs, const Line& src) {
    Line out;
    int last = -1;
    for (const Glyph& g : gs) {
        if (g.span != last) {
            Span s = src[g.span];
            s.text.clear();
            out.push_back(std::move(s));
            last = g.span;
        }
        out.back().text += g.g;
    }
    return out;
}
}  // namespace

std::vector<Line> wrap(const Line& l, int width) {
    width = std::max(1, width);
    std::vector<Glyph> glyphs;
    for (int i = 0; i < static_cast<int>(l.size()); ++i)
        for (std::string& g : Utf8ToGlyphs(l[i].text)) {
            int w = string_width(g);
            glyphs.push_back({std::move(g), w, i});
        }

    std::vector<Line> out;
    std::vector<Glyph> cur;
    int cur_w = 0;
    bool wrapped = false;  // past the first line: leading spaces are dropped
    auto flush = [&] {
        while (!cur.empty() && cur.back().g == " ") {
            cur_w -= cur.back().w;
            cur.pop_back();
        }
        out.push_back(to_line(cur, l));
        cur.clear();
        cur_w = 0;
        wrapped = true;
    };

    for (size_t i = 0; i < glyphs.size();) {
        size_t j = i;
        bool space = glyphs[i].g == " ";
        int w = 0;
        while (j < glyphs.size() && (glyphs[j].g == " ") == space) w += glyphs[j++].w;

        if (space) {
            if (!(wrapped && cur.empty())) {
                cur.insert(cur.end(), glyphs.begin() + i, glyphs.begin() + j);
                cur_w += w;
            }
        } else {
            if (cur_w + w > width && !cur.empty()) flush();
            // A word longer than a whole line is folded across lines.
            for (size_t k = i; k < j; ++k) {
                if (cur_w + glyphs[k].w > width && !cur.empty()) flush();
                cur.push_back(glyphs[k]);
                cur_w += glyphs[k].w;
            }
        }
        i = j;
    }
    if (!cur.empty() || out.empty()) {
        // Spaces hanging past the edge are dropped, like at any other break.
        while (cur_w > width && !cur.empty() && cur.back().g == " ") {
            cur_w -= cur.back().w;
            cur.pop_back();
        }
        out.push_back(to_line(cur, l));
    }
    return out;
}

std::vector<Line> wrap_all(const std::vector<Line>& lines, int width) {
    std::vector<Line> out;
    for (const Line& l : lines) {
        auto w = wrap(l, width);
        out.insert(out.end(), std::make_move_iterator(w.begin()), std::make_move_iterator(w.end()));
    }
    return out;
}

Element render(const Line& l, int width) {
    Elements parts;
    int used = 0;
    for (const Span& s : l) {
        if (s.text.empty()) continue;
        Element e = text(s.text) | color(s.fg);
        if (s.bold) e = e | bold;
        if (!s.link.empty()) e = e | hyperlink(s.link);
        parts.push_back(std::move(e));
        used += string_width(s.text);
    }
    if (used < width) parts.push_back(text(std::string(width - used, ' ')));
    return hbox(std::move(parts)) | size(WIDTH, EQUAL, width);
}

Element scrollbar(int height, int total, int offset) {
    static const char* bars[] = {"▁", "▁", "▂", "▃", "▄", "▅", "▆", "▇", " "};
    // Same arithmetic as Textual's ScrollBarRender, in eighths of a row.
    double ratio = static_cast<double>(total) / height;
    double thumb = std::max(1.0, height / ratio);
    double pos_ratio = total > height ? static_cast<double>(offset) / (total - height) : 0;
    double position = (height - thumb) * pos_ratio;
    int start = static_cast<int>(position * 8);
    int end = start + static_cast<int>(std::ceil(thumb * 8));
    int start_index = std::max(0, start) / 8, start_bar = std::max(0, start) % 8;
    int end_index = std::max(0, end) / 8, end_bar = std::max(0, end) % 8;

    Elements cells;
    for (int i = 0; i < height; ++i) {
        Element cell;
        if (i == start_index && start_bar != 0) {
            // Thumb starts part-way down this cell.
            std::string c = bars[8 - start_bar];
            cell = text(c + c) | color(pal::scrollbar) | bgcolor(pal::background);
        } else if (i == end_index && end_bar != 0) {
            // Thumb ends part-way down this cell.
            std::string c = bars[8 - end_bar];
            cell = text(c + c) | color(pal::background) | bgcolor(pal::scrollbar);
        } else if (i >= start_index && i < end_index) {
            cell = text("  ") | bgcolor(pal::scrollbar);
        } else {
            cell = text("  ") | bgcolor(pal::background);
        }
        cells.push_back(cell);
    }
    return vbox(std::move(cells)) | size(WIDTH, EQUAL, 2);
}

Element rounded(Element inner, Color c) { return std::move(inner) | borderStyled(ROUNDED, c); }

static std::string repeat(const char* s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

Element tall(Element inner, int width, int height, Color c, Color bg) {
    int iw = std::max(0, width - 2), ih = std::max(0, height - 2);
    auto left = [&] { return text("▊") | color(pal::background) | bgcolor(c); };
    auto right = [&] { return text("▎") | color(c) | bgcolor(pal::background); };
    Elements lefts, rights;
    for (int i = 0; i < ih; ++i) {
        lefts.push_back(left());
        rights.push_back(right());
    }
    return vbox({
        hbox({left(), text(repeat("▔", iw)) | color(c) | bgcolor(bg), right()}),
        hbox({vbox(std::move(lefts)), std::move(inner) | size(WIDTH, EQUAL, iw) | size(HEIGHT, EQUAL, ih) | bgcolor(bg),
              vbox(std::move(rights))}),
        hbox({left(), text(repeat("▁", iw)) | color(c) | bgcolor(bg), right()}),
    });
}

Element blank(int width, int height) {
    return text("") | size(WIDTH, EQUAL, std::max(0, width)) | size(HEIGHT, EQUAL, std::max(0, height));
}
