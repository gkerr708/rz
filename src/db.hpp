// The library database: ~/.local/share/rip-zotero/library.sqlite, plus the
// PDFs copied into ~/.local/share/rip-zotero/storage.
//
// Text fields use "" for "no value" (NULL in the database), so an empty
// field and a missing one mean the same thing, as in the Python version.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

struct DbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

fs::path app_dir();      // ~/.local/share/rip-zotero
fs::path app_db();       // .../library.sqlite
fs::path app_storage();  // .../storage

// The text columns of `items`, in table order.
extern const std::vector<std::string> kItemColumns;

struct Item {
    int id = 0;
    std::string title, authors, tags, type_name, pdf_path, doi, isbn, issn, pmid, pmcid, abstract, date, url,
        publication_title, journal_abbrev, volume, issue, pages, publisher, place, edition, series, series_number,
        num_pages, language, book_title, conference_name, proceedings_title, short_title, call_number, archive_id,
        repository, library_catalog, rights, extra, access_date, summary;
    std::string date_added;
    bool to_read = false;
    bool pdf_exists = false;  // pdf_path points at a real file (checked on load)

    bool has_pdf() const { return pdf_exists; }
    void check_pdf();  // refresh pdf_exists after pdf_path changes
    // A column by name ("title", "doi", ...); nullptr if there's no such column.
    std::string* field(std::string_view name);
    const std::string* field(std::string_view name) const;
};

// Changes to write: column -> new value ("" stores NULL), in order.
using Changes = std::vector<std::pair<std::string, std::string>>;

struct Author {
    long long id = 0;
    std::string last_key, first_key, name, email, lab, department, university, location, urls, research_areas,
        notes, known_for;
    std::optional<long long> canonical_id;

    std::string* field(std::string_view name);
    const std::string* field(std::string_view name) const;
    bool has_profile() const;  // any of email, lab, ..., known_for filled in
};

using NameKey = std::pair<std::string, std::string>;  // (last name, first given name), lowercase

struct SeedContact {
    std::string name, email, lab, university, department, location, notes;
    std::vector<std::string> urls, research_areas;
};

void init_db();

std::vector<Item> load_items();
int create_item(const Changes& fields);
void update_item_fields(int item_id, const Changes& changes);
void delete_item(int item_id);

// Every PDF attached to an item, in order: (path, label).
std::vector<std::pair<std::string, std::string>> list_item_pdfs(int item_id);
void add_item_pdf(int item_id, const std::string& path, const std::string& label, int sort_order);
fs::path save_pdf(int item_id, const fs::path& source);
fs::path save_extra_pdf(int item_id, const fs::path& source, int index);

std::vector<std::string> list_projects();
bool toggle_project_item(const std::string& project, int item_id);  // true = added
std::set<int> project_item_ids(const std::string& project);
std::vector<std::string> projects_for_item(int item_id);
int cleanup_projects(const std::set<std::string>& valid);
int rename_project(const std::string& old_name, const std::string& new_name);

int cleanup_tags(const std::set<std::string>& valid);
int rename_tag(const std::string& old_name, const std::string& new_name);

void ensure_authors(const std::vector<std::pair<NameKey, std::string>>& entries,
                    const std::map<NameKey, SeedContact>& seed);
std::vector<Author> list_authors();
void update_author_fields(long long author_id, const Changes& changes);

// rz migrate: copy a Zotero library (zotero.sqlite + storage/) into ours.
int migrate_from_zotero(const fs::path& zotero_db, const fs::path& zotero_storage);
