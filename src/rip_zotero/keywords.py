from __future__ import annotations

from rip_zotero.db import APP_DIR

KEYWORDS_FILE = APP_DIR / "keywords.txt"

DEFAULT_KEYWORDS = """\
# One keyword per line. Lines starting with # are ignored.
# These are the only values allowed in an item's `tags` field (via 'e'),
# to keep tagging consistent and typo-free. Add new ones here first.
fluid-mechanics
extensional-rheology
"""


def ensure_keywords_file() -> None:
    KEYWORDS_FILE.parent.mkdir(parents=True, exist_ok=True)
    if not KEYWORDS_FILE.exists():
        KEYWORDS_FILE.write_text(DEFAULT_KEYWORDS)


def load_keywords() -> set[str]:
    ensure_keywords_file()
    keywords: set[str] = set()
    for line in KEYWORDS_FILE.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            keywords.add(line)
    return keywords


def unknown_tags(tags_str: str | None) -> list[str]:
    """Return the tags in tags_str that aren't in the controlled keyword list."""
    if not tags_str:
        return []
    known = load_keywords()
    tags = [t.strip() for t in tags_str.split(";") if t.strip()]
    return [t for t in tags if t not in known]
