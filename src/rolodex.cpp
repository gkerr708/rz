#include "rolodex.hpp"

#include <toml++/toml.hpp>

NameKey name_key(const std::string& name) {
    std::vector<std::string> tokens = split_ws(name);
    if (tokens.empty()) return {"", ""};
    return {lower(tokens.back()), lower(tokens.front())};
}

NameKey author_key_from_entry(const std::string& raw) {
    std::string entry = trim(raw);
    auto comma = entry.find(',');
    if (comma == std::string::npos) return name_key(entry);
    std::vector<std::string> first = split_ws(entry.substr(comma + 1));
    return {lower(trim(entry.substr(0, comma))), first.empty() ? "" : lower(first.front())};
}

std::map<NameKey, SeedContact> load_seed_contacts() {
    std::map<NameKey, SeedContact> seeds;
    fs::path file = RZ_ROLODEX_SEED;
    if (!fs::exists(file)) return seeds;
    toml::table data;
    try {
        data = toml::parse_file(file.string());
    } catch (const toml::parse_error&) {
        return seeds;
    }
    const toml::array* groups = data["group"].as_array();
    if (!groups) return seeds;
    auto str = [](const toml::table& t, const char* key) { return trim(t[key].value_or(std::string())); };
    auto list = [](const toml::table& t, const char* key) {
        std::vector<std::string> out;
        if (const toml::array* a = t[key].as_array())
            for (const auto& v : *a)
                if (auto s = v.value<std::string>(); s && !s->empty()) out.push_back(*s);
        return out;
    };
    for (const auto& node : *groups) {
        const toml::table* t = node.as_table();
        if (!t) continue;
        std::string name = str(*t, "name");
        if (name.empty() || lower(name) == "template") continue;
        SeedContact c;
        c.name = name;
        c.email = str(*t, "email");
        c.lab = str(*t, "lab");
        c.university = str(*t, "university");
        c.department = str(*t, "department");
        c.location = str(*t, "location");
        c.notes = str(*t, "notes");
        c.urls = list(*t, "urls");
        c.research_areas = list(*t, "research_areas");
        seeds[name_key(name)] = std::move(c);
    }
    return seeds;
}
