from __future__ import annotations
import re
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from rip_zotero.main import ZoteroItem

ENTRY_TYPE_MAP = {
    "article": "journalArticle",
    "book": "book",
    "inbook": "bookSection",
    "incollection": "bookSection",
    "inproceedings": "conferencePaper",
    "conference": "conferencePaper",
    "phdthesis": "thesis",
    "mastersthesis": "thesis",
    "techreport": "report",
    "misc": "webpage",
    "unpublished": "manuscript",
}

REVERSE_ENTRY_TYPE_MAP = {
    "journalArticle": "article",
    "book": "book",
    "bookSection": "incollection",
    "conferencePaper": "inproceedings",
    "thesis": "phdthesis",
    "report": "techreport",
    "webpage": "misc",
    "manuscript": "unpublished",
}


def _extract_braced(text: str, start: int) -> tuple[str, int]:
    """text[start] == '{'; returns (contents, index just past matching '}')."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1 : i], i + 1
    return text[start + 1 :], len(text)


def _raw_fields(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for m in re.finditer(r"(\w+)\s*=\s*", text):
        key = m.group(1).lower()
        pos = m.end()
        if pos >= len(text):
            continue
        if text[pos] == "{":
            value, _ = _extract_braced(text, pos)
        elif text[pos] == '"':
            end = text.find('"', pos + 1)
            value = text[pos + 1 : end] if end != -1 else ""
        else:
            m2 = re.search(r"[,\n}]", text[pos:])
            value = text[pos : pos + m2.start()] if m2 else text[pos:]
        fields[key] = re.sub(r"\s+", " ", value).strip()
    return fields


def parse_bibtex(text: str) -> dict[str, str | None]:
    """Parse a single BibTeX entry into ZoteroItem-shaped fields.

    Returns an empty title on failure so callers can detect "nothing found".
    """
    type_match = re.search(r"@(\w+)\s*\{", text)
    entry_type = type_match.group(1).lower() if type_match else None

    fields = _raw_fields(text)

    authors = fields.get("author")
    authors_str = None
    if authors:
        parts = [p.strip() for p in re.split(r"\s+and\s+", authors) if p.strip()]
        authors_str = "; ".join(parts)

    return {
        "title": fields.get("title") or "",
        "authors": authors_str,
        "date": fields.get("year"),
        "doi": fields.get("doi"),
        "url": fields.get("url"),
        "publication_title": fields.get("journal") or fields.get("booktitle"),
        "volume": fields.get("volume"),
        "issue": fields.get("number"),
        "pages": fields.get("pages"),
        "publisher": fields.get("publisher"),
        "abstract": fields.get("abstract"),
        "type_name": ENTRY_TYPE_MAP.get(entry_type, "document"),
    }


def _bibtex_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}")


def _key_base(item: "ZoteroItem") -> str:
    last = "unknown"
    if item.authors:
        first_author = item.authors.split(";")[0]
        last_raw = first_author.split(",")[0].strip().lower()
        last = re.sub(r"[^a-z0-9]", "", last_raw) or "unknown"
    year = (item.date or "")[:4]
    year = year if year.isdigit() else "nd"
    return f"{last}{year}"


def generate_unique_key(item: "ZoteroItem", used_keys: set[str]) -> str:
    base = _key_base(item)
    key = base
    suffix = ord("a")
    while key in used_keys:
        key = f"{base}{chr(suffix)}"
        suffix += 1
    used_keys.add(key)
    return key


def item_to_bibtex_entry(item: "ZoteroItem", key: str) -> str:
    entry_type = REVERSE_ENTRY_TYPE_MAP.get(item.type_name or "", "article")
    lines = [f"@{entry_type}{{{key},"]

    def add(field: str, value: str | None) -> None:
        if value:
            lines.append(f"  {field} = {{{_bibtex_escape(value)}}},")

    add("title", item.title)
    if item.authors:
        authors_bib = " and ".join(a.strip() for a in item.authors.split(";"))
        add("author", authors_bib)
    add("year", (item.date or "")[:4] or None)
    add("journal", item.publication_title)
    add("volume", item.volume)
    add("number", item.issue)
    add("pages", item.pages)
    add("publisher", item.publisher)
    add("doi", item.doi)
    add("url", item.url)

    lines.append("}")
    return "\n".join(lines)


def items_to_bibtex(items: list["ZoteroItem"]) -> str:
    used_keys: set[str] = set()
    entries = []
    for item in items:
        key = generate_unique_key(item, used_keys)
        entries.append(item_to_bibtex_entry(item, key))
    return "\n\n".join(entries) + "\n"
