from __future__ import annotations

from rip_zotero.db import APP_DIR

MANUSCRIPTS_FILE = APP_DIR / "manuscripts.txt"

DEFAULT_MANUSCRIPTS = """\
# One manuscript name per line. Lines starting with # are ignored.
# These are the only names allowed when adding a paper to a manuscript
# (via 'm') or filtering to one (via ':name' in search), to keep names
# consistent and typo-free. Add new ones here first.
"""


def ensure_manuscripts_file() -> None:
    MANUSCRIPTS_FILE.parent.mkdir(parents=True, exist_ok=True)
    if not MANUSCRIPTS_FILE.exists():
        MANUSCRIPTS_FILE.write_text(DEFAULT_MANUSCRIPTS)


def load_manuscripts() -> set[str]:
    ensure_manuscripts_file()
    names: set[str] = set()
    for line in MANUSCRIPTS_FILE.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            names.add(line.lower())
    return names
