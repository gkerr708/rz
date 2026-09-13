from __future__ import annotations

from rip_zotero.db import APP_DIR

PROJECTS_FILE = APP_DIR / "projects.txt"
_OLD_MANUSCRIPTS_FILE = APP_DIR / "manuscripts.txt"

DEFAULT_PROJECTS = """\
# One project name per line. Lines starting with # are ignored.
# These are the only names allowed when adding a paper to a project
# (via 'm') or filtering to one (via ':name' in search), to keep names
# consistent and typo-free. Add new ones here first.
"""


def ensure_projects_file() -> None:
    PROJECTS_FILE.parent.mkdir(parents=True, exist_ok=True)
    if not PROJECTS_FILE.exists():
        if _OLD_MANUSCRIPTS_FILE.exists():
            PROJECTS_FILE.write_text(_OLD_MANUSCRIPTS_FILE.read_text())
        else:
            PROJECTS_FILE.write_text(DEFAULT_PROJECTS)


def load_projects() -> set[str]:
    ensure_projects_file()
    names: set[str] = set()
    for line in PROJECTS_FILE.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            names.add(line.lower())
    return names


def add_project(name: str) -> None:
    ensure_projects_file()
    if name in load_projects():
        return
    with PROJECTS_FILE.open("a") as f:
        f.write(f"{name}\n")
