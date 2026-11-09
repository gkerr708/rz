#include "db.hpp"

#include <sqlite3.h>

#include <algorithm>

// =================================================================
// Locations
// =================================================================
fs::path app_dir() { return home_dir() / ".local" / "share" / "rip-zotero"; }
fs::path app_db() { return app_dir() / "library.sqlite"; }
fs::path app_storage() { return app_dir() / "storage"; }

const std::vector<std::string> kItemColumns = {
    "title", "authors", "tags", "type_name", "pdf_path", "doi", "isbn", "issn", "pmid", "pmcid",
    "abstract", "date", "url", "publication_title", "journal_abbrev", "volume", "issue", "pages",
    "publisher", "place", "edition", "series", "series_number", "num_pages", "language", "book_title",
    "conference_name", "proceedings_title", "short_title", "call_number", "archive_id", "repository",
    "library_catalog", "rights", "extra", "access_date", "summary",
};

// =================================================================
// Item / Author fields by name
// =================================================================
namespace {
using ItemMember = std::string Item::*;
const std::vector<std::pair<std::string_view, ItemMember>> kItemMembers = {
    {"title", &Item::title}, {"authors", &Item::authors}, {"tags", &Item::tags},
    {"type_name", &Item::type_name}, {"pdf_path", &Item::pdf_path}, {"doi", &Item::doi},
    {"isbn", &Item::isbn}, {"issn", &Item::issn}, {"pmid", &Item::pmid}, {"pmcid", &Item::pmcid},
    {"abstract", &Item::abstract}, {"date", &Item::date}, {"url", &Item::url},
    {"publication_title", &Item::publication_title}, {"journal_abbrev", &Item::journal_abbrev},
    {"volume", &Item::volume}, {"issue", &Item::issue}, {"pages", &Item::pages},
    {"publisher", &Item::publisher}, {"place", &Item::place}, {"edition", &Item::edition},
    {"series", &Item::series}, {"series_number", &Item::series_number}, {"num_pages", &Item::num_pages},
    {"language", &Item::language}, {"book_title", &Item::book_title},
    {"conference_name", &Item::conference_name}, {"proceedings_title", &Item::proceedings_title},
    {"short_title", &Item::short_title}, {"call_number", &Item::call_number},
    {"archive_id", &Item::archive_id}, {"repository", &Item::repository},
    {"library_catalog", &Item::library_catalog}, {"rights", &Item::rights}, {"extra", &Item::extra},
    {"access_date", &Item::access_date}, {"summary", &Item::summary}, {"date_added", &Item::date_added},
};

using AuthorMember = std::string Author::*;
const std::vector<std::pair<std::string_view, AuthorMember>> kAuthorMembers = {
    {"last_key", &Author::last_key}, {"first_key", &Author::first_key}, {"name", &Author::name},
    {"email", &Author::email}, {"lab", &Author::lab}, {"department", &Author::department},
    {"university", &Author::university}, {"location", &Author::location}, {"urls", &Author::urls},
    {"research_areas", &Author::research_areas}, {"notes", &Author::notes}, {"known_for", &Author::known_for},
};
}  // namespace

std::string* Item::field(std::string_view name) {
    for (auto& [n, m] : kItemMembers) if (n == name) return &(this->*m);
    return nullptr;
}
const std::string* Item::field(std::string_view name) const { return const_cast<Item*>(this)->field(name); }

void Item::check_pdf() {
    std::error_code ec;
    pdf_exists = !pdf_path.empty() && fs::exists(pdf_path, ec);
}

std::string* Author::field(std::string_view name) {
    for (auto& [n, m] : kAuthorMembers) if (n == name) return &(this->*m);
    return nullptr;
}
const std::string* Author::field(std::string_view name) const { return const_cast<Author*>(this)->field(name); }

bool Author::has_profile() const {
    for (const char* f : {"email", "lab", "department", "university", "location", "urls", "research_areas",
                          "notes", "known_for"})
        if (!field(f)->empty()) return true;
    return false;
}

// =================================================================
// A thin layer over the sqlite3 C API
// =================================================================
namespace {
struct Conn {
    sqlite3* db = nullptr;

    // Read-only connections are for queries; writing ones create the file.
    Conn(const fs::path& path, bool read_only) {
        int flags = read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
            std::string err = db ? sqlite3_errmsg(db) : "out of memory";
            sqlite3_close(db);
            throw DbError("Couldn't open " + path.string() + ": " + err);
        }
        sqlite3_busy_timeout(db, 1000);
    }
    ~Conn() { sqlite3_close(db); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw DbError(msg);
        }
    }
    // For the migrations that fail harmlessly when already applied.
    void try_exec(const std::string& sql) { sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr); }
    long long last_id() const { return sqlite3_last_insert_rowid(db); }
};

struct Stmt {
    sqlite3_stmt* st = nullptr;
    sqlite3* db;
    int next = 1;  // next parameter to bind

    Stmt(Conn& c, const std::string& sql) : db(c.db) {
        if (sqlite3_prepare_v2(c.db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
            throw DbError(sqlite3_errmsg(c.db));
    }
    ~Stmt() { sqlite3_finalize(st); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    // "" binds NULL, like Python's `value or None`.
    Stmt& text(const std::string& v) {
        if (v.empty()) sqlite3_bind_null(st, next++);
        else sqlite3_bind_text(st, next++, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& raw_text(const std::string& v) {  // always text, even ""
        sqlite3_bind_text(st, next++, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& integer(long long v) {
        sqlite3_bind_int64(st, next++, v);
        return *this;
    }
    // True while there's a row to read.
    bool step() {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(sqlite3_errmsg(db));
    }
    void run() { while (step()) {} }
    void reset() {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        next = 1;
    }
    std::string col(int i) const {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    long long col_int(int i) const { return sqlite3_column_int64(st, i); }
    bool is_null(int i) const { return sqlite3_column_type(st, i) == SQLITE_NULL; }
};

// BEGIN ... COMMIT, rolled back if anything throws (Python's `with con:`).
struct Txn {
    Conn& c;
    bool done = false;
    explicit Txn(Conn& conn) : c(conn) { c.exec("BEGIN"); }
    void commit() {
        c.exec("COMMIT");
        done = true;
    }
    ~Txn() {
        if (!done) c.try_exec("ROLLBACK");
    }
};

bool db_exists() {
    std::error_code ec;
    return fs::exists(app_db(), ec);
}

// A writing connection, after making sure the tables exist.
Conn open_rw(bool init = false) {
    if (init) init_db();
    return Conn(app_db(), false);
}
}  // namespace

// =================================================================
// Schema
// =================================================================
void init_db() {
    fs::create_directories(app_dir());
    fs::create_directories(app_storage());
    Conn con(app_db(), false);
    std::string cols;
    for (const auto& c : kItemColumns) cols += c + " TEXT,\n";
    con.exec("CREATE TABLE IF NOT EXISTS items (\n id INTEGER PRIMARY KEY,\n" + cols + " date_added TEXT NOT NULL\n)");
    // Renamed from manuscripts/manuscript_items -> projects/project_items.
    // Migrate any existing tables/columns in place so old data survives.
    con.try_exec("ALTER TABLE manuscripts RENAME TO projects");
    con.try_exec("ALTER TABLE manuscript_items RENAME TO project_items");
    con.try_exec("ALTER TABLE project_items RENAME COLUMN manuscript_id TO project_id");
    con.exec(R"(CREATE TABLE IF NOT EXISTS projects (
        id INTEGER PRIMARY KEY,
        name TEXT UNIQUE NOT NULL,
        created_at TEXT NOT NULL
    ))");
    con.exec(R"(CREATE TABLE IF NOT EXISTS project_items (
        project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
        item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
        PRIMARY KEY (project_id, item_id)
    ))");
    con.exec(R"(CREATE TABLE IF NOT EXISTS item_pdfs (
        id INTEGER PRIMARY KEY,
        item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
        path TEXT NOT NULL,
        label TEXT,
        sort_order INTEGER NOT NULL DEFAULT 0
    ))");
    con.exec(R"(CREATE TABLE IF NOT EXISTS authors (
        id INTEGER PRIMARY KEY,
        last_key TEXT NOT NULL,
        first_key TEXT NOT NULL,
        name TEXT NOT NULL,
        email TEXT,
        lab TEXT,
        department TEXT,
        university TEXT,
        location TEXT,
        urls TEXT,
        research_areas TEXT,
        notes TEXT,
        UNIQUE(last_key, first_key)
    ))");
    // Columns added later; these fail harmlessly once they exist.
    con.try_exec("ALTER TABLE items ADD COLUMN to_read INTEGER NOT NULL DEFAULT 0");
    con.try_exec("ALTER TABLE items ADD COLUMN summary TEXT");
    con.try_exec("ALTER TABLE authors ADD COLUMN canonical_id INTEGER");
    con.try_exec("ALTER TABLE authors ADD COLUMN known_for TEXT");
}

// =================================================================
// Items
// =================================================================
std::vector<Item> load_items() {
    std::vector<Item> items;
    if (!db_exists()) return items;
    Conn con(app_db(), true);
    std::string sql = "SELECT id, date_added, to_read";
    for (const auto& c : kItemColumns) sql += ", " + c;
    Stmt st(con, sql + " FROM items ORDER BY date_added DESC");
    while (st.step()) {
        Item it;
        it.id = static_cast<int>(st.col_int(0));
        it.date_added = st.col(1);
        it.to_read = st.col_int(2) != 0;
        for (size_t i = 0; i < kItemColumns.size(); ++i) *it.field(kItemColumns[i]) = st.col(3 + i);
        it.check_pdf();
        items.push_back(std::move(it));
    }
    return items;
}

int create_item(const Changes& fields) {
    Conn con = open_rw(true);
    std::vector<const std::pair<std::string, std::string>*> cols;
    for (const auto& c : kItemColumns)
        for (const auto& f : fields)
            if (f.first == c) cols.push_back(&f);
    std::string names, marks;
    for (auto* f : cols) {
        names += f->first + ", ";
        marks += "?, ";
    }
    Txn txn(con);
    Stmt st(con, "INSERT INTO items (" + names + "date_added) VALUES (" + marks + "datetime('now'))");
    for (auto* f : cols) st.text(f->second);
    st.run();
    int id = static_cast<int>(con.last_id());
    txn.commit();
    return id;
}

// Only known column names are accepted, so the SQL can't be bent.
static std::string set_clause(const Changes& changes, const std::vector<std::string>& allowed) {
    std::string sql;
    for (const auto& [col, _] : changes) {
        if (std::find(allowed.begin(), allowed.end(), col) == allowed.end()) throw DbError("Unknown column " + col);
        sql += (sql.empty() ? "" : ", ") + col + " = ?";
    }
    return sql;
}

void update_item_fields(int item_id, const Changes& changes) {
    if (changes.empty()) return;
    std::vector<std::string> allowed = kItemColumns;
    allowed.push_back("to_read");
    Conn con = open_rw();
    Txn txn(con);
    Stmt st(con, "UPDATE items SET " + set_clause(changes, allowed) + " WHERE id = ?");
    for (const auto& [col, value] : changes) {
        if (col == "to_read") st.integer(value == "1" ? 1 : 0);
        else st.text(value);
    }
    st.integer(item_id).run();
    txn.commit();
}

void delete_item(int item_id) {
    std::string main_pdf;
    std::vector<std::string> extra;
    {
        Conn con = open_rw();
        Txn txn(con);
        Stmt row(con, "SELECT pdf_path FROM items WHERE id = ?");
        row.integer(item_id);
        if (row.step()) main_pdf = row.col(0);
        Stmt pdfs(con, "SELECT path FROM item_pdfs WHERE item_id = ?");
        pdfs.integer(item_id);
        while (pdfs.step()) extra.push_back(pdfs.col(0));
        for (const char* sql : {"DELETE FROM item_pdfs WHERE item_id = ?", "DELETE FROM project_items WHERE item_id = ?",
                                "DELETE FROM items WHERE id = ?"}) {
            Stmt del(con, sql);
            del.integer(item_id).run();
        }
        txn.commit();
    }
    std::error_code ec;
    if (!main_pdf.empty()) fs::remove(main_pdf, ec);
    for (const auto& p : extra) fs::remove(p, ec);
}

// =================================================================
// PDFs
// =================================================================
std::vector<std::pair<std::string, std::string>> list_item_pdfs(int item_id) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!db_exists()) return out;
    Conn con(app_db(), true);
    Stmt st(con, "SELECT path, label FROM item_pdfs WHERE item_id = ? ORDER BY sort_order, id");
    st.integer(item_id);
    while (st.step()) out.emplace_back(st.col(0), st.col(1));
    return out;
}

void add_item_pdf(int item_id, const std::string& path, const std::string& label, int sort_order) {
    Conn con = open_rw(true);
    Txn txn(con);
    Stmt st(con, "INSERT INTO item_pdfs (item_id, path, label, sort_order) VALUES (?, ?, ?, ?)");
    st.integer(item_id).raw_text(path).text(label).integer(sort_order).run();
    txn.commit();
}

fs::path save_pdf(int item_id, const fs::path& source) {
    fs::create_directories(app_storage());
    fs::path dest = app_storage() / (std::to_string(item_id) + ".pdf");
    fs::copy_file(source, dest, fs::copy_options::overwrite_existing);
    return dest;
}

fs::path save_extra_pdf(int item_id, const fs::path& source, int index) {
    // `index` is unique per item, so names never collide with the main PDF.
    fs::create_directories(app_storage());
    fs::path dest = app_storage() / (std::to_string(item_id) + "_" + std::to_string(index) + ".pdf");
    fs::copy_file(source, dest, fs::copy_options::overwrite_existing);
    return dest;
}

// =================================================================
// Projects
// =================================================================
std::vector<std::string> list_projects() {
    std::vector<std::string> out;
    if (!db_exists()) return out;
    Conn con(app_db(), true);
    Stmt st(con, "SELECT name FROM projects ORDER BY name");
    while (st.step()) out.push_back(st.col(0));
    return out;
}

bool toggle_project_item(const std::string& project, int item_id) {
    Conn con = open_rw(true);
    con.exec("PRAGMA foreign_keys = ON");
    Txn txn(con);
    long long project_id;
    {
        Stmt find(con, "SELECT id FROM projects WHERE name = ?");
        find.raw_text(project);
        if (find.step()) {
            project_id = find.col_int(0);
        } else {
            Stmt ins(con, "INSERT INTO projects (name, created_at) VALUES (?, datetime('now'))");
            ins.raw_text(project).run();
            project_id = con.last_id();
        }
    }
    Stmt exists(con, "SELECT 1 FROM project_items WHERE project_id = ? AND item_id = ?");
    exists.integer(project_id).integer(item_id);
    bool had = exists.step();
    Stmt change(con, had ? "DELETE FROM project_items WHERE project_id = ? AND item_id = ?"
                         : "INSERT INTO project_items (project_id, item_id) VALUES (?, ?)");
    change.integer(project_id).integer(item_id).run();
    txn.commit();
    return !had;
}

std::set<int> project_item_ids(const std::string& project) {
    std::set<int> out;
    if (!db_exists()) return out;
    Conn con(app_db(), true);
    Stmt st(con, "SELECT pi.item_id FROM project_items pi JOIN projects p ON pi.project_id = p.id WHERE p.name = ?");
    st.raw_text(project);
    while (st.step()) out.insert(static_cast<int>(st.col_int(0)));
    return out;
}

std::vector<std::string> projects_for_item(int item_id) {
    std::vector<std::string> out;
    if (!db_exists()) return out;
    Conn con(app_db(), true);
    Stmt st(con,
            "SELECT p.name FROM project_items pi JOIN projects p ON pi.project_id = p.id "
            "WHERE pi.item_id = ? ORDER BY p.name");
    st.integer(item_id);
    while (st.step()) out.push_back(st.col(0));
    return out;
}

int cleanup_projects(const std::set<std::string>& valid) {
    // Delete projects (and, by cascade, their paper links) no longer listed.
    Conn con = open_rw();
    con.exec("PRAGMA foreign_keys = ON");
    Txn txn(con);
    std::vector<long long> doomed;
    Stmt st(con, "SELECT id, name FROM projects");
    while (st.step()) if (!valid.contains(st.col(1))) doomed.push_back(st.col_int(0));
    for (long long id : doomed) {
        Stmt del(con, "DELETE FROM projects WHERE id = ?");
        del.integer(id).run();
    }
    txn.commit();
    return static_cast<int>(doomed.size());
}

int rename_project(const std::string& old_name, const std::string& new_name) {
    // If new_name already exists, merge old_name's papers into it instead.
    Conn con = open_rw();
    con.exec("PRAGMA foreign_keys = ON");
    Txn txn(con);
    auto id_of = [&](const std::string& name) -> std::optional<long long> {
        Stmt st(con, "SELECT id FROM projects WHERE name = ?");
        st.raw_text(name);
        if (st.step()) return st.col_int(0);
        return std::nullopt;
    };
    auto old_id = id_of(old_name);
    if (!old_id) return 0;
    auto new_id = id_of(new_name);
    if (!new_id) {
        Stmt up(con, "UPDATE projects SET name = ? WHERE id = ?");
        up.raw_text(new_name).integer(*old_id).run();
        txn.commit();
        return 1;
    }
    std::vector<long long> items;
    {
        Stmt st(con, "SELECT item_id FROM project_items WHERE project_id = ?");
        st.integer(*old_id);
        while (st.step()) items.push_back(st.col_int(0));
    }
    for (long long item : items) {
        Stmt ins(con, "INSERT OR IGNORE INTO project_items (project_id, item_id) VALUES (?, ?)");
        ins.integer(*new_id).integer(item).run();
    }
    Stmt del(con, "DELETE FROM projects WHERE id = ?");
    del.integer(*old_id).run();
    txn.commit();
    return static_cast<int>(items.size());
}

// =================================================================
// Tags
// =================================================================
static std::vector<std::string> split_tags(const std::string& tags) {
    std::vector<std::string> out;
    for (const auto& t : split(tags, ";")) {
        std::string s = trim(t);
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

// Rewrite every item's tags through `fix`, saving the ones that changed.
template <typename Fix>
static int rewrite_tags(Fix fix) {
    Conn con = open_rw();
    Txn txn(con);
    std::vector<std::pair<long long, std::string>> rows;
    {
        Stmt st(con, "SELECT id, tags FROM items WHERE tags IS NOT NULL AND tags != ''");
        while (st.step()) rows.emplace_back(st.col_int(0), st.col(1));
    }
    int changed = 0;
    for (const auto& [id, tags] : rows) {
        std::vector<std::string> current = split_tags(tags);
        std::optional<std::vector<std::string>> updated = fix(current);
        if (!updated) continue;
        Stmt up(con, "UPDATE items SET tags = ? WHERE id = ?");
        up.text(join(*updated, "; ")).integer(id).run();
        ++changed;
    }
    txn.commit();
    return changed;
}

int cleanup_tags(const std::set<std::string>& valid) {
    return rewrite_tags([&](const std::vector<std::string>& current) -> std::optional<std::vector<std::string>> {
        std::vector<std::string> kept;
        for (const auto& t : current) if (valid.contains(t)) kept.push_back(t);
        if (kept == current) return std::nullopt;
        return kept;
    });
}

int rename_tag(const std::string& old_name, const std::string& new_name) {
    return rewrite_tags([&](const std::vector<std::string>& current) -> std::optional<std::vector<std::string>> {
        if (std::find(current.begin(), current.end(), old_name) == current.end()) return std::nullopt;
        std::vector<std::string> deduped;
        for (const auto& t : current) {
            std::string n = t == old_name ? new_name : t;
            if (std::find(deduped.begin(), deduped.end(), n) == deduped.end()) deduped.push_back(n);
        }
        return deduped;
    });
}

// =================================================================
// Authors
// =================================================================
void ensure_authors(const std::vector<std::pair<NameKey, std::string>>& entries,
                    const std::map<NameKey, SeedContact>& seed) {
    // New authors get a row, pre-filled from the seed file if it knows
    // them; existing rows are never touched, so edits aren't overwritten.
    Conn con = open_rw(true);
    Txn txn(con);
    Stmt exists(con, "SELECT 1 FROM authors WHERE last_key = ? AND first_key = ?");
    Stmt ins(con,
             "INSERT INTO authors (last_key, first_key, name, email, lab, department, university, location, "
             "urls, research_areas, notes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    for (const auto& [key, display] : entries) {
        exists.reset();
        exists.raw_text(key.first).raw_text(key.second);
        if (exists.step()) continue;
        auto it = seed.find(key);
        SeedContact info = it == seed.end() ? SeedContact{} : it->second;
        ins.reset();
        ins.raw_text(key.first).raw_text(key.second).raw_text(info.name.empty() ? display : info.name);
        ins.text(info.email).text(info.lab).text(info.department).text(info.university).text(info.location);
        ins.text(join(info.urls, "; ")).text(join(info.research_areas, "; ")).text(info.notes);
        ins.run();
    }
    txn.commit();
}

std::vector<Author> list_authors() {
    std::vector<Author> out;
    if (!db_exists()) return out;
    Conn con(app_db(), true);
    std::string sql = "SELECT id, canonical_id";
    for (auto& [n, _] : kAuthorMembers) sql += ", " + std::string(n);
    Stmt st(con, sql + " FROM authors");
    while (st.step()) {
        Author a;
        a.id = st.col_int(0);
        if (!st.is_null(1)) a.canonical_id = st.col_int(1);
        for (size_t i = 0; i < kAuthorMembers.size(); ++i) a.*(kAuthorMembers[i].second) = st.col(2 + i);
        out.push_back(std::move(a));
    }
    return out;
}

void update_author_fields(long long author_id, const Changes& changes) {
    if (changes.empty()) return;
    std::vector<std::string> allowed;
    for (auto& [n, _] : kAuthorMembers) allowed.emplace_back(n);
    Conn con = open_rw();
    Txn txn(con);
    Stmt st(con, "UPDATE authors SET " + set_clause(changes, allowed) + " WHERE id = ?");
    for (const auto& [col, value] : changes) {
        // name is NOT NULL, so it's stored even when blank.
        if (col == "name") st.raw_text(value);
        else st.text(value);
    }
    st.integer(author_id).run();
    txn.commit();
}

// =================================================================
// Importing a Zotero library
// =================================================================
int migrate_from_zotero(const fs::path& zotero_db, const fs::path& zotero_storage) {
    // Zotero's own field names -> our columns.
    static const std::map<std::string, std::string> zotero_fields = {
        {"title", "title"}, {"DOI", "doi"}, {"ISBN", "isbn"}, {"ISSN", "issn"}, {"PMID", "pmid"},
        {"PMCID", "pmcid"}, {"abstractNote", "abstract"}, {"date", "date"}, {"url", "url"},
        {"publicationTitle", "publication_title"}, {"journalAbbreviation", "journal_abbrev"},
        {"volume", "volume"}, {"issue", "issue"}, {"pages", "pages"}, {"publisher", "publisher"},
        {"place", "place"}, {"edition", "edition"}, {"series", "series"}, {"seriesNumber", "series_number"},
        {"numPages", "num_pages"}, {"language", "language"}, {"bookTitle", "book_title"},
        {"conferenceName", "conference_name"}, {"proceedingsTitle", "proceedings_title"},
        {"shortTitle", "short_title"}, {"callNumber", "call_number"}, {"archiveID", "archive_id"},
        {"repository", "repository"}, {"libraryCatalog", "library_catalog"}, {"rights", "rights"},
        {"extra", "extra"}, {"accessDate", "access_date"},
    };

    std::map<long long, Item> by_id;
    std::set<long long> has_data;
    {
        Conn z(zotero_db, true);
        // One row per (item, field); the first value of each field wins.
        Stmt data(z, R"(SELECT i.itemID, f.fieldName, idv.value
            FROM items i
            JOIN itemData id ON i.itemID = id.itemID
            JOIN fields f ON id.fieldID = f.fieldID
            JOIN itemDataValues idv ON id.valueID = idv.valueID
            WHERE i.itemTypeID != 3)");
        std::set<std::pair<long long, std::string>> seen;
        while (data.step()) {
            long long id = data.col_int(0);
            has_data.insert(id);
            Item& it = by_id[id];
            it.id = static_cast<int>(id);
            auto f = zotero_fields.find(data.col(1));
            if (f == zotero_fields.end() || !seen.insert({id, f->second}).second) continue;
            *it.field(f->second) = data.col(2);
        }
        auto per_item = [&](const std::string& sql, auto apply) {
            Stmt st(z, sql);
            while (st.step()) {
                auto it = by_id.find(st.col_int(0));
                if (it != by_id.end()) apply(it->second, st);
            }
        };
        per_item(R"(SELECT i.itemID, GROUP_CONCAT(c.lastName || ', ' || c.firstName, '; ')
            FROM items i JOIN itemCreators ic ON i.itemID = ic.itemID
            JOIN creators c ON ic.creatorID = c.creatorID
            WHERE i.itemTypeID != 3 GROUP BY i.itemID)",
                 [](Item& it, Stmt& st) { it.authors = st.col(1); });
        per_item(R"(SELECT i.itemID, GROUP_CONCAT(t.name, '; ')
            FROM items i JOIN itemTags it ON i.itemID = it.itemID
            JOIN tags t ON it.tagID = t.tagID
            WHERE i.itemTypeID != 3 GROUP BY i.itemID)",
                 [](Item& it, Stmt& st) { it.tags = st.col(1); });
        per_item(R"(SELECT i.itemID, it.typeName FROM items i
            JOIN itemTypes it ON i.itemTypeID = it.itemTypeID WHERE i.itemTypeID != 3)",
                 [](Item& it, Stmt& st) { it.type_name = st.col(1); });
        // An item can have several PDFs (a book split into chapters): keep the first.
        std::set<long long> got_pdf;
        per_item(R"(SELECT ia.parentItemID, i.key, ia.path FROM itemAttachments ia
            JOIN items i ON ia.itemID = i.itemID
            WHERE ia.contentType = 'application/pdf' AND ia.path IS NOT NULL)",
                 [&](Item& it, Stmt& st) {
                     if (!got_pdf.insert(it.id).second) return;
                     std::string path = st.col(2);
                     it.pdf_path = path.starts_with("storage:")
                                       ? (zotero_storage / st.col(1) / path.substr(8)).string()
                                       : path;
                 });
        per_item("SELECT itemID, dateAdded FROM items WHERE itemTypeID != 3",
                 [](Item& it, Stmt& st) { it.date_added = st.col(1); });
    }

    std::vector<Item> items;
    for (auto& [id, it] : by_id)
        if (has_data.contains(id) && !it.title.empty()) items.push_back(std::move(it));

    init_db();
    fs::create_directories(app_storage());
    Conn con = open_rw();
    Txn txn(con);
    std::string names = "id", marks = "?";
    for (const auto& c : kItemColumns) {
        names += ", " + c;
        marks += ", ?";
    }
    Stmt ins(con, "INSERT OR REPLACE INTO items (" + names + ", date_added) VALUES (" + marks + ", ?)");
    for (Item& it : items) {
        it.check_pdf();
        std::string new_pdf;
        if (it.has_pdf()) {
            fs::path dest = app_storage() / (std::to_string(it.id) + ".pdf");
            copy_file_with_times(it.pdf_path, dest);
            new_pdf = dest.string();
        }
        ins.reset();
        ins.integer(it.id);
        for (const auto& c : kItemColumns) ins.text(c == "pdf_path" ? new_pdf : *it.field(c));
        ins.raw_text(it.date_added);
        ins.run();
    }
    txn.commit();
    return static_cast<int>(items.size());
}
