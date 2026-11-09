// Matching author names across papers, and the one-time seed data
// (rolodex.toml) used to pre-fill a new author's profile.
#pragma once

#include <map>
#include <string>

#include "db.hpp"

// (last name, first given-name token), both lowercase. Comparing only these
// means "John Frostad" and "John M. Frostad" match, while the last name still
// has to agree exactly.
NameKey name_key(const std::string& name);
// The same for one "Last, First" author entry from a paper.
NameKey author_key_from_entry(const std::string& entry);

// rolodex.toml's people, keyed by name_key(). Read-only: after an author is
// first seen, the authors table is what counts.
std::map<NameKey, SeedContact> load_seed_contacts();
