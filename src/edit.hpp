// Editing a paper or an author profile in $EDITOR, as a small TOML file.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "db.hpp"

struct EditError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The paper fields `e` lets you change, in the order they're written.
extern const std::vector<std::string> kEditableFields;

std::string item_to_toml(const Item& item);
// field -> value for every editable field in the file. Throws EditError.
std::map<std::string, std::string> parse_toml_fields(const std::string& text);
// Fields whose value differs from the item's. A field left out of the file
// counts as cleared.
Changes diff_fields(const Item& item, const std::map<std::string, std::string>& parsed);

// Author profiles: urls and research_areas are TOML lists, stored "a; b".
extern const std::vector<std::string> kAuthorEditFields;
std::string author_to_toml(const Author& a);
// Only the fields present in the file. Throws EditError.
Changes parse_author_toml(const std::string& text);
