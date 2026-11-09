#include "bibtex.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

static const std::map<std::string, std::string> kEntryTypes = {
    {"article", "journalArticle"},  {"book", "book"},           {"inbook", "bookSection"},
    {"incollection", "bookSection"}, {"inproceedings", "conferencePaper"},
    {"conference", "conferencePaper"}, {"phdthesis", "thesis"}, {"mastersthesis", "thesis"},
    {"techreport", "report"},       {"misc", "webpage"},        {"unpublished", "manuscript"},
};

static const std::map<std::string, std::string> kReverseEntryTypes = {
    {"journalArticle", "article"}, {"book", "book"},       {"bookSection", "incollection"},
    {"conferencePaper", "inproceedings"}, {"thesis", "phdthesis"}, {"report", "techreport"},
    {"webpage", "misc"},           {"manuscript", "unpublished"},
};

// text[start] == '{': the contents up to the matching '}'.
static std::string extract_braced(const std::string& text, size_t start) {
    int depth = 0;
    for (size_t i = start; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}' && --depth == 0) return text.substr(start + 1, i - start - 1);
    }
    return text.substr(start + 1);
}

static std::string squash(const std::string& s) {
    static const std::regex ws(R"(\s+)");
    return trim(std::regex_replace(s, ws, " "));
}

// Every `key = value` in the text, later ones winning.
static std::map<std::string, std::string> raw_fields(const std::string& text) {
    std::map<std::string, std::string> fields;
    static const std::regex key_re(R"((\w+)\s*=\s*)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), key_re); it != std::sregex_iterator(); ++it) {
        std::string key = lower((*it)[1].str());
        size_t pos = it->position() + it->length();
        if (pos >= text.size()) continue;
        std::string value;
        if (text[pos] == '{') {
            value = extract_braced(text, pos);
        } else if (text[pos] == '"') {
            size_t end = text.find('"', pos + 1);
            value = end == std::string::npos ? "" : text.substr(pos + 1, end - pos - 1);
        } else {
            size_t end = text.find_first_of(",\n}", pos);
            value = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        }
        fields[key] = squash(value);
    }
    return fields;
}

Changes parse_bibtex(const std::string& text) {
    std::smatch m;
    static const std::regex type_re(R"(@(\w+)\s*\{)");
    std::string entry_type = std::regex_search(text, m, type_re) ? lower(m[1].str()) : "";
    auto fields = raw_fields(text);
    auto get = [&](const std::string& k) {
        auto it = fields.find(k);
        return it == fields.end() ? std::string() : it->second;
    };

    std::string authors;
    if (std::string a = get("author"); !a.empty()) {
        static const std::regex and_re(R"(\s+and\s+)");
        std::vector<std::string> parts;
        for (auto it = std::sregex_token_iterator(a.begin(), a.end(), and_re, -1); it != std::sregex_token_iterator(); ++it)
            if (std::string p = trim(it->str()); !p.empty()) parts.push_back(p);
        authors = join(parts, "; ");
    }
    auto type = kEntryTypes.find(entry_type);
    std::string journal = get("journal");
    return {
        {"title", get("title")},
        {"authors", authors},
        {"date", get("year")},
        {"doi", get("doi")},
        {"url", get("url")},
        {"publication_title", journal.empty() ? get("booktitle") : journal},
        {"volume", get("volume")},
        {"issue", get("number")},
        {"pages", get("pages")},
        {"publisher", get("publisher")},
        {"abstract", get("abstract")},
        {"type_name", type == kEntryTypes.end() ? "document" : type->second},
    };
}

static std::string escape(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '\\' || c == '{' || c == '}') out += '\\';
        out += c;
    }
    return out;
}

// "smith2001": first author's last name (letters and digits only) + year.
static std::string key_base(const Item& item) {
    std::string last = "unknown";
    if (!item.authors.empty()) {
        std::string first_author = split(item.authors, ";")[0];
        std::string raw = lower(trim(split(first_author, ",")[0]));
        std::string clean;
        for (char c : raw) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) clean += c;
        if (!clean.empty()) last = clean;
    }
    std::string year = cp_slice(item.date, 0, 4);
    bool digits = !year.empty() && std::all_of(year.begin(), year.end(), [](char c) { return c >= '0' && c <= '9'; });
    return last + (digits ? year : "nd");
}

static std::string entry(const Item& item, const std::string& key) {
    auto type = kReverseEntryTypes.find(item.type_name);
    std::string out = "@" + (type == kReverseEntryTypes.end() ? std::string("article") : type->second) + "{" + key + ",";
    auto add = [&](const char* field, const std::string& value) {
        if (!value.empty()) out += "\n  " + std::string(field) + " = {" + escape(value) + "},";
    };
    add("title", item.title);
    if (!item.authors.empty()) {
        std::vector<std::string> names;
        for (const auto& a : split(item.authors, ";")) names.push_back(trim(a));
        add("author", join(names, " and "));
    }
    add("year", cp_slice(item.date, 0, 4));
    add("journal", item.publication_title);
    add("volume", item.volume);
    add("number", item.issue);
    add("pages", item.pages);
    add("publisher", item.publisher);
    add("doi", item.doi);
    add("url", item.url);
    return out + "\n}";
}

std::string items_to_bibtex(const std::vector<const Item*>& items) {
    std::set<std::string> used;
    std::vector<std::string> entries;
    for (const Item* item : items) {
        std::string base = key_base(*item), key = base;
        for (char suffix = 'a'; used.contains(key); ++suffix) key = base + suffix;
        used.insert(key);
        entries.push_back(entry(*item, key));
    }
    return join(entries, "\n\n") + "\n";
}
