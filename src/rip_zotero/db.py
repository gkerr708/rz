from __future__ import annotations
import shutil
import sqlite3
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from rip_zotero.main import ZoteroItem

APP_DIR = Path.home() / ".local" / "share" / "rip-zotero"
APP_DB = APP_DIR / "library.sqlite"
APP_STORAGE = APP_DIR / "storage"

# Column order mirrors ZoteroItem's fields (main.py) minus item_id/pdf_path/date_added,
# which are handled specially.
ITEM_COLUMNS = [
    "title",
    "authors",
    "tags",
    "type_name",
    "pdf_path",
    "doi",
    "isbn",
    "issn",
    "pmid",
    "pmcid",
    "abstract",
    "date",
    "url",
    "publication_title",
    "journal_abbrev",
    "volume",
    "issue",
    "pages",
    "publisher",
    "place",
    "edition",
    "series",
    "series_number",
    "num_pages",
    "language",
    "book_title",
    "conference_name",
    "proceedings_title",
    "short_title",
    "call_number",
    "archive_id",
    "repository",
    "library_catalog",
    "rights",
    "extra",
    "access_date",
]


def init_db(db_path: Path | None = None) -> None:
    db_path = db_path if db_path is not None else APP_DB
    db_path.parent.mkdir(parents=True, exist_ok=True)
    APP_STORAGE.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(str(db_path))
    try:
        cols_sql = ",\n".join(f"{c} TEXT" for c in ITEM_COLUMNS)
        con.execute(
            f"""
            CREATE TABLE IF NOT EXISTS items (
                id INTEGER PRIMARY KEY,
                {cols_sql},
                date_added TEXT NOT NULL
            )
            """
        )
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS manuscripts (
                id INTEGER PRIMARY KEY,
                name TEXT UNIQUE NOT NULL,
                created_at TEXT NOT NULL
            )
            """
        )
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS manuscript_items (
                manuscript_id INTEGER NOT NULL REFERENCES manuscripts(id) ON DELETE CASCADE,
                item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
                PRIMARY KEY (manuscript_id, item_id)
            )
            """
        )
        con.execute(
            """
            CREATE TABLE IF NOT EXISTS item_pdfs (
                id INTEGER PRIMARY KEY,
                item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
                path TEXT NOT NULL,
                label TEXT,
                sort_order INTEGER NOT NULL DEFAULT 0
            )
            """
        )
        con.commit()
    finally:
        con.close()


def list_item_pdfs(item_id: int, db_path: Path | None = None) -> list[tuple[str, str | None]]:
    """Return [(path, label), ...] for every PDF attached to item_id, in order."""
    db_path = db_path if db_path is not None else APP_DB
    if not db_path.exists():
        return []
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        rows = con.execute(
            "SELECT path, label FROM item_pdfs WHERE item_id = ? ORDER BY sort_order, id",
            (item_id,),
        ).fetchall()
    finally:
        con.close()
    return [(r[0], r[1]) for r in rows]


def add_item_pdf(
    item_id: int,
    path: str,
    label: str | None = None,
    sort_order: int = 0,
    db_path: Path | None = None,
) -> None:
    db_path = db_path if db_path is not None else APP_DB
    init_db(db_path)
    con = sqlite3.connect(str(db_path), timeout=1)
    try:
        con.execute("PRAGMA busy_timeout = 1000")
        with con:
            con.execute(
                "INSERT INTO item_pdfs (item_id, path, label, sort_order) VALUES (?, ?, ?, ?)",
                (item_id, path, label, sort_order),
            )
    finally:
        con.close()


def list_manuscripts(db_path: Path | None = None) -> list[str]:
    db_path = db_path if db_path is not None else APP_DB
    if not db_path.exists():
        return []
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        rows = con.execute("SELECT name FROM manuscripts ORDER BY name").fetchall()
    finally:
        con.close()
    return [r[0] for r in rows]


def toggle_manuscript_item(
    manuscript_name: str, item_id: int, db_path: Path | None = None
) -> bool:
    """Add item_id to manuscript_name if absent, else remove it.

    Returns True if the item was added, False if it was removed.
    """
    db_path = db_path if db_path is not None else APP_DB
    init_db(db_path)
    con = sqlite3.connect(str(db_path), timeout=1)
    try:
        con.execute("PRAGMA busy_timeout = 1000")
        con.execute("PRAGMA foreign_keys = ON")
        with con:
            row = con.execute(
                "SELECT id FROM manuscripts WHERE name = ?", (manuscript_name,)
            ).fetchone()
            if row is None:
                manuscript_id = con.execute(
                    "INSERT INTO manuscripts (name, created_at) VALUES (?, datetime('now'))",
                    (manuscript_name,),
                ).lastrowid
            else:
                manuscript_id = row[0]

            exists = con.execute(
                "SELECT 1 FROM manuscript_items WHERE manuscript_id = ? AND item_id = ?",
                (manuscript_id, item_id),
            ).fetchone()
            if exists:
                con.execute(
                    "DELETE FROM manuscript_items WHERE manuscript_id = ? AND item_id = ?",
                    (manuscript_id, item_id),
                )
                return False
            else:
                con.execute(
                    "INSERT INTO manuscript_items (manuscript_id, item_id) VALUES (?, ?)",
                    (manuscript_id, item_id),
                )
                return True
    finally:
        con.close()


def manuscript_item_ids(
    manuscript_name: str, db_path: Path | None = None
) -> set[int]:
    db_path = db_path if db_path is not None else APP_DB
    if not db_path.exists():
        return set()
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        rows = con.execute(
            """
            SELECT mi.item_id
            FROM manuscript_items mi
            JOIN manuscripts m ON mi.manuscript_id = m.id
            WHERE m.name = ?
            """,
            (manuscript_name,),
        ).fetchall()
    finally:
        con.close()
    return {r[0] for r in rows}


def manuscripts_for_item(item_id: int, db_path: Path | None = None) -> list[str]:
    db_path = db_path if db_path is not None else APP_DB
    if not db_path.exists():
        return []
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        rows = con.execute(
            """
            SELECT m.name
            FROM manuscript_items mi
            JOIN manuscripts m ON mi.manuscript_id = m.id
            WHERE mi.item_id = ?
            ORDER BY m.name
            """,
            (item_id,),
        ).fetchall()
    finally:
        con.close()
    return [r[0] for r in rows]


def load_items(db_path: Path | None = None) -> list["ZoteroItem"]:
    from rip_zotero.main import ZoteroItem

    db_path = db_path if db_path is not None else APP_DB
    if not db_path.exists():
        return []
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    try:
        rows = con.execute(
            "SELECT * FROM items ORDER BY date_added DESC"
        ).fetchall()
    finally:
        con.close()

    items = []
    for row in rows:
        kwargs = {c: row[c] for c in ITEM_COLUMNS}
        pdf_path = kwargs.pop("pdf_path")
        items.append(
            ZoteroItem(
                item_id=row["id"],
                pdf_path=Path(pdf_path) if pdf_path else None,
                date_added=row["date_added"],
                **kwargs,
            )
        )
    return items


def create_item(fields: dict[str, str | None], db_path: Path | None = None) -> int:
    db_path = db_path if db_path is not None else APP_DB
    init_db(db_path)
    con = sqlite3.connect(str(db_path), timeout=1)
    try:
        con.execute("PRAGMA busy_timeout = 1000")
        cols = [c for c in ITEM_COLUMNS if c in fields]
        placeholders = ", ".join("?" for _ in cols)
        col_list = ", ".join(cols)
        with con:
            cur = con.execute(
                f"""
                INSERT INTO items ({col_list}, date_added)
                VALUES ({placeholders}, datetime('now'))
                """,
                [fields[c] for c in cols],
            )
            return cur.lastrowid
    finally:
        con.close()


def update_item_fields(
    item_id: int, changes: dict[str, str | None], db_path: Path | None = None
) -> None:
    if not changes:
        return
    db_path = db_path if db_path is not None else APP_DB
    con = sqlite3.connect(str(db_path), timeout=1)
    try:
        con.execute("PRAGMA busy_timeout = 1000")
        set_clause = ", ".join(f"{col} = ?" for col in changes)
        with con:
            con.execute(
                f"UPDATE items SET {set_clause} WHERE id = ?",
                [*changes.values(), item_id],
            )
    finally:
        con.close()


def cleanup_tags(valid_keywords: set[str], db_path: Path | None = None) -> int:
    """Strip any tag not in valid_keywords from every item. Returns items changed."""
    db_path = db_path if db_path is not None else APP_DB
    con = sqlite3.connect(str(db_path), timeout=1)
    changed = 0
    try:
        con.execute("PRAGMA busy_timeout = 1000")
        with con:
            rows = con.execute(
                "SELECT id, tags FROM items WHERE tags IS NOT NULL AND tags != ''"
            ).fetchall()
            for item_id, tags in rows:
                current = [t.strip() for t in tags.split(";") if t.strip()]
                kept = [t for t in current if t in valid_keywords]
                if kept != current:
                    con.execute(
                        "UPDATE items SET tags = ? WHERE id = ?",
                        ("; ".join(kept) or None, item_id),
                    )
                    changed += 1
    finally:
        con.close()
    return changed


def delete_item(item_id: int, db_path: Path | None = None) -> None:
    db_path = db_path if db_path is not None else APP_DB
    con = sqlite3.connect(str(db_path), timeout=1)
    try:
        con.execute("PRAGMA busy_timeout = 1000")
        with con:
            row = con.execute(
                "SELECT pdf_path FROM items WHERE id = ?", (item_id,)
            ).fetchone()
            extra_pdfs = con.execute(
                "SELECT path FROM item_pdfs WHERE item_id = ?", (item_id,)
            ).fetchall()
            con.execute("DELETE FROM item_pdfs WHERE item_id = ?", (item_id,))
            con.execute(
                "DELETE FROM manuscript_items WHERE item_id = ?", (item_id,)
            )
            con.execute("DELETE FROM items WHERE id = ?", (item_id,))
    finally:
        con.close()

    if row and row[0]:
        Path(row[0]).unlink(missing_ok=True)
    for (path,) in extra_pdfs:
        Path(path).unlink(missing_ok=True)


def save_pdf(item_id: int, data: bytes, storage_dir: Path | None = None) -> Path:
    storage_dir = storage_dir if storage_dir is not None else APP_STORAGE
    storage_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = storage_dir / f"{item_id}.pdf"
    pdf_path.write_bytes(data)
    return pdf_path


def save_extra_pdf(
    item_id: int, data: bytes, index: int, storage_dir: Path | None = None
) -> Path:
    """Save an additional PDF for an item that already has one (e.g. a book
    split into per-chapter files). `index` should be unique per item so
    filenames don't collide with the primary PDF or each other."""
    storage_dir = storage_dir if storage_dir is not None else APP_STORAGE
    storage_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = storage_dir / f"{item_id}_{index}.pdf"
    pdf_path.write_bytes(data)
    return pdf_path


def migrate_from_zotero(
    zotero_db: Path,
    zotero_storage: Path,
    db_path: Path | None = None,
    storage_dir: Path | None = None,
) -> int:
    db_path = db_path if db_path is not None else APP_DB
    storage_dir = storage_dir if storage_dir is not None else APP_STORAGE

    import rip_zotero.main

    original_db = rip_zotero.main.ZOTERO_DB
    original_storage = rip_zotero.main.ZOTERO_STORAGE
    rip_zotero.main.ZOTERO_DB = zotero_db
    rip_zotero.main.ZOTERO_STORAGE = zotero_storage
    try:
        items = rip_zotero.main.load_library()
    finally:
        rip_zotero.main.ZOTERO_DB = original_db
        rip_zotero.main.ZOTERO_STORAGE = original_storage

    init_db(db_path)
    storage_dir.mkdir(parents=True, exist_ok=True)

    con = sqlite3.connect(str(db_path))
    try:
        cols = ITEM_COLUMNS + ["date_added"]
        placeholders = ", ".join("?" for _ in cols)
        col_list = ", ".join(cols)
        with con:
            for item in items:
                item_id = int(item.item_id)
                new_pdf_path = None
                if item.has_pdf:
                    dest = storage_dir / f"{item_id}.pdf"
                    shutil.copy2(item.pdf_path, dest)
                    new_pdf_path = str(dest)

                values = []
                for c in ITEM_COLUMNS:
                    if c == "pdf_path":
                        values.append(new_pdf_path)
                    else:
                        values.append(getattr(item, c))
                values.append(item.date_added or "")

                con.execute(
                    f"INSERT OR REPLACE INTO items (id, {col_list}) "
                    f"VALUES (?, {placeholders})",
                    [item_id, *values],
                )
    finally:
        con.close()

    return len(items)
