#include "edit.hpp"

#include <cstdio>
#include <sstream>

#include <toml++/toml.hpp>

const std::vector<std::string> kEditableFields = {
    "title", "date", "doi", "isbn", "issn", "pmid", "pmcid", "url", "publication_title", "journal_abbrev",
    "volume", "issue", "pages", "publisher", "place", "edition", "series", "series_number", "num_pages",
    "language", "book_title", "conference_name", "proceedings_title", "short_title", "call_number",
    "archive_id", "repository", "library_catalog", "rights", "extra", "access_date", "summary", "abstract",
    "tags",
};

const std::vector<std::string> kAuthorEditFields = {
    "name", "email", "lab", "department", "university", "location", "urls", "research_areas", "notes",
    "known_for",
};

static bool is_list_field(const std::string& f) { return f == "urls" || f == "research_areas"; }

// A TOML basic string's contents.
static std::string escape(const std::string& v) {
    std::string out;
    for (unsigned char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += c;
        else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04X", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out;
}

static toml::table parse(const std::string& text) {
    try {
        return toml::parse(text);
    } catch (const toml::parse_error& e) {
        const auto& at = e.source().begin;
        throw EditError("Invalid TOML: " + std::string(e.description()) + " (at line " + std::to_string(at.line) +
                        ", column " + std::to_string(at.column) + ")");
    }
}

// A value as text, whatever type was typed (Python's str()).
static std::string as_text(const toml::node& n) {
    if (auto s = n.value_exact<std::string>()) return *s;
    if (auto b = n.value_exact<bool>()) return *b ? "True" : "False";
    if (auto i = n.value_exact<int64_t>()) return std::to_string(*i);
    std::ostringstream ss;
    if (const auto* f = n.as_floating_point()) ss << f->get();
    else n.visit([&](const auto& v) { ss << v; });
    return ss.str();
}

std::string item_to_toml(const Item& item) {
    std::string out = "# Editing: " + item.title + "\n" +
                      "# Save and quit to apply changes. Leave a field blank to clear it.\n"
                      "# Non-listed fields (type, PDF path) are not editable here.\n\n";
    for (const auto& f : kEditableFields) out += f + " = \"" + escape(*item.field(f)) + "\"\n";
    return out;
}

std::map<std::string, std::string> parse_toml_fields(const std::string& text) {
    toml::table data = parse(text);
    std::map<std::string, std::string> parsed;
    for (const auto& f : kEditableFields)
        if (const toml::node* n = data.get(f)) parsed[f] = trim(as_text(*n));
    return parsed;
}

Changes diff_fields(const Item& item, const std::map<std::string, std::string>& parsed) {
    Changes changes;
    for (const auto& f : kEditableFields) {
        auto it = parsed.find(f);
        std::string value = it == parsed.end() ? "" : it->second;
        if (value != *item.field(f)) changes.emplace_back(f, value);
    }
    return changes;
}

std::string author_to_toml(const Author& a) {
    std::string out = "# Editing profile for: " + a.name + "\n" +
                      "# Save and quit to apply changes. Leave a field blank to clear it.\n\n";
    for (const auto& f : kAuthorEditFields) {
        const std::string& value = *a.field(f);
        if (is_list_field(f)) {
            std::vector<std::string> quoted;
            for (const auto& v : split(value, "; "))
                if (!v.empty()) quoted.push_back("\"" + escape(v) + "\"");
            out += f + " = [" + join(quoted, ", ") + "]\n";
        } else {
            out += f + " = \"" + escape(value) + "\"\n";
        }
    }
    return out;
}

Changes parse_author_toml(const std::string& text) {
    toml::table data = parse(text);
    Changes fields;
    for (const auto& f : kAuthorEditFields) {
        const toml::node* n = data.get(f);
        if (!n) continue;
        if (is_list_field(f)) {
            std::vector<std::string> items;
            if (const toml::array* arr = n->as_array()) {
                for (const auto& v : *arr)
                    if (std::string s = trim(as_text(v)); !s.empty()) items.push_back(s);
            } else if (std::string s = trim(as_text(*n)); !s.empty()) {
                items.push_back(s);  // a single string instead of a list
            }
            fields.emplace_back(f, join(items, "; "));
        } else {
            fields.emplace_back(f, trim(as_text(*n)));
        }
    }
    return fields;
}
