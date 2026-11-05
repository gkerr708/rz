from __future__ import annotations
import tomllib
from typing import TYPE_CHECKING

from rip_zotero.db import update_item_fields

if TYPE_CHECKING:
    from rip_zotero.main import ZoteroItem

EDITABLE_FIELDS: list[str] = [
    "title",
    "date",
    "doi",
    "isbn",
    "issn",
    "pmid",
    "pmcid",
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
    "summary",
    "abstract",
    "tags",
]


class ZoteroWriteError(Exception):
    pass


def _toml_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def item_to_toml(item: "ZoteroItem") -> str:
    lines = [
        f"# Editing: {item.title}",
        "# Save and quit to apply changes. Leave a field blank to clear it.",
        "# Non-listed fields (type, PDF path) are not editable here.",
        "",
    ]
    for attr in EDITABLE_FIELDS:
        value = getattr(item, attr) or ""
        lines.append(f'{attr} = "{_toml_escape(value)}"')
    return "\n".join(lines) + "\n"


def parse_toml_fields(text: str) -> dict[str, str]:
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ZoteroWriteError(f"Invalid TOML: {e}") from e
    parsed: dict[str, str] = {}
    for attr in EDITABLE_FIELDS:
        if attr in data:
            parsed[attr] = str(data[attr]).strip()
    return parsed


def diff_fields(item: "ZoteroItem", parsed: dict[str, str]) -> dict[str, str | None]:
    changes: dict[str, str | None] = {}
    for attr in EDITABLE_FIELDS:
        new_value = parsed.get(attr, "")
        old_value = getattr(item, attr) or ""
        if new_value != old_value:
            changes[attr] = new_value or None
    return changes


def save_item_fields(item_id: int, changes: dict[str, str | None]) -> None:
    update_item_fields(item_id, changes)
