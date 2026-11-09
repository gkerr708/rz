// Styled text and the drawing pieces shared by every panel.
//
// A Line is a run of Spans (text + colour + bold + link), the C++ stand-in
// for the Rich markup the Python version used. Lines are word-wrapped here
// rather than by FTXUI, so every panel wraps exactly like Textual did.
#pragma once

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

// The rose-pine-black theme, resolved to the exact colours Textual drew.
namespace pal {
using ftxui::Color;
// Text colours (the Python CAT palette)
inline const Color text = Color::RGB(0xE0, 0xDE, 0xF4);
inline const Color overlay0 = Color::RGB(0x38, 0x37, 0x3D);  // dim: years, papers without PDFs
inline const Color overlay1 = Color::RGB(0x4E, 0x4D, 0x55);  // detail labels
inline const Color yellow = Color::RGB(0xF6, 0xC1, 0x77);    // titles, headings, ★
inline const Color peach = Color::RGB(0xF6, 0xC1, 0x77);     // book titles
inline const Color green = Color::RGB(0x9C, 0xCF, 0xD8);     // * has PDF, [x]
inline const Color blue = Color::RGB(0x31, 0x74, 0x8F);      // authors
inline const Color sapphire = Color::RGB(0x31, 0x74, 0x8F);  // projects, links
inline const Color mauve = Color::RGB(0xC4, 0xA7, 0xE7);     // tags
inline const Color red = Color::RGB(0xEB, 0x6F, 0x92);       // missing PDF
// Chrome
inline const Color background = Color::RGB(0x00, 0x00, 0x00);
inline const Color plain = Color::RGB(0xDD, 0xDD, 0xDD);        // unstyled text
inline const Color muted = Color::RGB(0x99, 0x99, 0x99);        // status bar, hints
inline const Color subtitle = Color::RGB(0x93, 0x92, 0xA1);     // header subtitle
inline const Color placeholder = Color::RGB(0x60, 0x60, 0x60);
inline const Color placeholder_tinted = Color::RGB(0x67, 0x67, 0x68);
inline const Color border = Color::RGB(0xFF, 0xFF, 0xFF);       // blurred border
inline const Color border_focus = Color::RGB(0x9C, 0xCF, 0xD8);
inline const Color input_border = Color::RGB(0x52, 0x4F, 0x67);  // focused picker input
inline const Color input_bg = Color::RGB(0x0B, 0x0B, 0x0C);      // focused picker input
inline const Color cursor = Color::RGB(0xF4, 0xED, 0xE8);
inline const Color select_focus = Color::RGB(0x55, 0x55, 0x55);  // highlighted row, list focused
inline const Color select_blur = Color::RGB(0x33, 0x33, 0x33);
inline const Color scrollbar = Color::RGB(0x4E, 0x42, 0x5C);
inline const Color footer_key = Color::RGB(0xEB, 0xBC, 0xBA);
inline const Color toast_bg = Color::RGB(0x16, 0x16, 0x16);
inline const Color success = Color::RGB(0x9C, 0xCF, 0xD8);
inline const Color warning = Color::RGB(0xF5, 0xC1, 0x77);
inline const Color error = Color::RGB(0xEA, 0x6F, 0x92);
}  // namespace pal

struct Span {
    std::string text;
    ftxui::Color fg = pal::plain;
    bool bold = false;
    std::string link;  // URL, for a clickable hyperlink

    Span(std::string t = "", ftxui::Color c = pal::plain, bool b = false, std::string url = "")
        : text(std::move(t)), fg(c), bold(b), link(std::move(url)) {}
};
using Line = std::vector<Span>;

inline Line line(std::string text, ftxui::Color fg = pal::plain, bool bold = false) {
    return {Span{std::move(text), fg, bold, {}}};
}

int text_width(const std::string& s);  // terminal columns
int line_width(const Line& l);

// Word-wrap to `width` columns: breaks at spaces (dropping them), folds
// words longer than a line. An empty line stays one empty line.
std::vector<Line> wrap(const Line& l, int width);
std::vector<Line> wrap_all(const std::vector<Line>& lines, int width);

// One line, drawn exactly `width` columns wide (cut or padded).
ftxui::Element render(const Line& l, int width);

// Textual's 2-column scrollbar: `height` rows tall, showing `offset` of `total`.
ftxui::Element scrollbar(int height, int total, int offset);

// ╭─╮ rounded box around `inner` (already sized), in one colour.
ftxui::Element rounded(ftxui::Element inner, ftxui::Color c);

// Textual's "tall" border (▊▔▎ / ▊▁▎) around `inner`, `width` × `height`
// outside, filling the inside with `bg`.
ftxui::Element tall(ftxui::Element inner, int width, int height, ftxui::Color c, ftxui::Color bg);

// Blank rows/columns.
ftxui::Element blank(int width, int height = 1);
