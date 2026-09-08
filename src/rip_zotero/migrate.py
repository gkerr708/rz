from __future__ import annotations
import sys

from rip_zotero.db import APP_DB, APP_STORAGE, migrate_from_zotero
from rip_zotero.main import ZOTERO_DB, ZOTERO_STORAGE


def main() -> None:
    if not ZOTERO_DB.exists():
        print(f"Zotero database not found at {ZOTERO_DB}", file=sys.stderr)
        raise SystemExit(1)
    print(f"Migrating from {ZOTERO_DB} -> {APP_DB} ...")
    count = migrate_from_zotero(ZOTERO_DB, ZOTERO_STORAGE)
    print(f"Migrated {count} items to {APP_DB}")
    print(f"PDFs copied to {APP_STORAGE}")


if __name__ == "__main__":
    main()
