// The controlled vocabularies, one name per line, '#' for comments:
//   keywords.txt - the only tags allowed on a paper
//   projects.txt - the only project names allowed (stored lowercase)
// Both live in ~/.local/share/rip-zotero and are edited with K / M.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "util.hpp"

fs::path keywords_file();
void ensure_keywords_file();
std::set<std::string> load_keywords();
void add_keyword(const std::string& name);
// Tags in "a; b; c" that aren't in keywords.txt.
std::vector<std::string> unknown_tags(const std::string& tags);

fs::path projects_file();
void ensure_projects_file();
std::set<std::string> load_projects();
void add_project(const std::string& name);
