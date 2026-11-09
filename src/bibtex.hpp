// Reading one BibTeX entry (for `a`) and writing entries (x, X, S).
#pragma once

#include <string>
#include <vector>

#include "db.hpp"

// One pasted entry -> item columns. "title" is "" when nothing was found.
Changes parse_bibtex(const std::string& text);

// Entries for these items, with unique keys like smith2001, smith2001a.
std::string items_to_bibtex(const std::vector<const Item*>& items);
