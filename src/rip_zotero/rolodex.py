from __future__ import annotations

import tomllib
from pathlib import Path

ROLODEX_SEED_FILE = Path(__file__).parent / "rolodex.toml"


def name_key(name: str) -> tuple[str, str]:
    """(last name, first given-name token), both lowercased.

    Comparing on just these two tokens (rather than the full string) means a
    middle initial or extra given name in one source but not the other (e.g.
    "John Frostad" vs. "John M. Frostad") still matches, while the last name
    still has to agree exactly — no partial/last-name-only matching.
    """
    tokens = name.strip().split()
    if not tokens:
        return ("", "")
    return (tokens[-1].lower(), tokens[0].lower())


def author_key_from_entry(entry: str) -> tuple[str, str]:
    """Same as name_key(), but for a single "Last, First" Zotero author entry."""
    entry = entry.strip()
    if "," in entry:
        last, first = entry.split(",", 1)
        first_token = first.strip().split()[0].lower() if first.strip() else ""
        return (last.strip().lower(), first_token)
    return name_key(entry)


def load_seed_contacts() -> dict[tuple[str, str], dict]:
    """One-time seed data, keyed by name_key(), used to pre-fill a
    newly-discovered author's profile the first time they're seen (see
    db.ensure_authors). After that the `authors` table in library.sqlite is
    authoritative — this file is read-only from the app's perspective and is
    never written back to."""
    if not ROLODEX_SEED_FILE.exists():
        return {}
    data = tomllib.loads(ROLODEX_SEED_FILE.read_text())
    seeds: dict[tuple[str, str], dict] = {}
    for entry in data.get("group", []):
        name = (entry.get("name") or "").strip()
        if not name or name.lower() == "template":
            continue
        seeds[name_key(name)] = {
            "name": name,
            "email": entry.get("email") or None,
            "lab": entry.get("lab") or None,
            "university": entry.get("university") or None,
            "department": entry.get("department") or None,
            "location": entry.get("location") or None,
            "urls": [u for u in entry.get("urls", []) if u],
            "research_areas": [r for r in entry.get("research_areas", []) if r],
            "notes": entry.get("notes") or None,
        }
    return seeds
