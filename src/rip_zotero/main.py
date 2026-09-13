from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
import os
import re
import subprocess
import sqlite3
import tempfile
import tomllib
import webbrowser
import pandas as pd

from rich.markup import escape
from textual.app import App, ComposeResult, SuspendNotSupported
from textual.binding import Binding
from textual.containers import Horizontal, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Footer, Header, Input, OptionList, Static
from textual.widgets.option_list import Option
from textual import work

from rip_zotero.edit import (
    ZoteroWriteError,
    diff_fields,
    item_to_toml,
    parse_toml_fields,
    save_item_fields,
)
from rip_zotero import db as db_module
from rip_zotero import rolodex as rolodex_module
from rip_zotero.bibtex import items_to_bibtex, parse_bibtex
from rip_zotero.keywords import (
    add_keyword,
    ensure_keywords_file,
    load_keywords,
    unknown_tags,
    KEYWORDS_FILE,
)
from rip_zotero.projects import (
    add_project,
    ensure_projects_file,
    load_projects,
    PROJECTS_FILE,
)

# Catppuccin Mocha
CAT = {
    "rosewater": "#f5e0dc",
    "flamingo": "#f2cdcd",
    "pink": "#f5c2e7",
    "mauve": "#cba6f7",
    "red": "#f38ba8",
    "maroon": "#eba0ac",
    "peach": "#fab387",
    "yellow": "#f9e2af",
    "green": "#a6e3a1",
    "teal": "#94e2d5",
    "sky": "#89dceb",
    "sapphire": "#74c7ec",
    "blue": "#89b4fa",
    "lavender": "#b4befe",
    "text": "#cdd6f4",
    "subtext1": "#bac2de",
    "subtext0": "#a6adc8",
    "overlay2": "#9399b2",
    "overlay1": "#7f849c",
    "overlay0": "#6c7086",
    "surface2": "#585b70",
    "surface1": "#45475a",
    "surface0": "#313244",
    "base": "#1e1e2e",
    "mantle": "#181825",
    "crust": "#11111b",
}


class VimListView(OptionList):
    BINDINGS = [
        Binding("j", "cursor_down", "Down", show=False),
        Binding("k", "cursor_up", "Up", show=False),
        Binding("g", "scroll_top", "Top", show=False),
        Binding("G", "scroll_bottom", "Bottom", show=False),
        Binding("o", "show_detail", "Detail", show=False),
        Binding("e", "edit_item", "Edit", show=False),
        Binding("a", "add_item", "Add", show=False),
        Binding("p", "add_pdf", "Attach PDF", show=False),
        Binding("d", "delete_item", "Delete", show=False),
        Binding("m", "toggle_project", "Project", show=False),
        Binding("t", "toggle_tag", "Tag", show=False),
        Binding("c", "show_paper_authors", "Authors", show=False),
        Binding("x", "copy_bibtex", "Copy BibTeX", show=False),
        Binding("X", "export_bibtex", "Export All", show=False),
        Binding("K", "edit_keywords", "Keywords", show=False),
        Binding("M", "edit_projects", "Projects", show=False),
        Binding("f", "open_filter", "Filter", show=False),
        Binding("w", "open_website", "Website", show=False),
        Binding("r", "toggle_read", "Reading Queue", show=False),
        Binding("R", "toggle_queue_filter", "Queue", show=False),
        Binding("enter", "activate", "Open PDF / Back to Papers", show=True),
    ]

    def action_scroll_top(self) -> None:
        self.action_first()

    def action_scroll_bottom(self) -> None:
        self.action_last()

    def action_show_detail(self) -> None:
        self.app.action_show_detail()

    def action_edit_item(self) -> None:
        self.app.action_edit_item()

    def action_add_item(self) -> None:
        self.app.action_add_item()

    def action_add_pdf(self) -> None:
        self.app.action_add_pdf()

    def action_delete_item(self) -> None:
        self.app.action_delete_item()

    def action_toggle_project(self) -> None:
        self.app.action_toggle_project()

    def action_toggle_tag(self) -> None:
        self.app.action_toggle_tag()

    def action_show_paper_authors(self) -> None:
        self.app.action_show_paper_authors()

    def action_export_bibtex(self) -> None:
        self.app.action_export_bibtex()

    def action_copy_bibtex(self) -> None:
        self.app.action_copy_bibtex()

    def action_edit_keywords(self) -> None:
        self.app.action_edit_keywords()

    def action_edit_projects(self) -> None:
        self.app.action_edit_projects()

    def action_open_filter(self) -> None:
        self.app.action_open_filter()

    def action_toggle_read(self) -> None:
        self.app.action_toggle_read()

    def action_toggle_queue_filter(self) -> None:
        self.app.action_toggle_queue_filter()

    def action_activate(self) -> None:
        # Same underlying behavior as OptionList's built-in Enter/"select"
        # action — this override just gives it a visible footer label.
        self.action_select()

    def action_open_website(self) -> None:
        self.app.action_open_website()

    _PAPERS_ONLY_ACTIONS = {
        "add_item",
        "add_pdf",
        "delete_item",
        "toggle_project",
        "toggle_tag",
        "show_paper_authors",
        "export_bibtex",
        "copy_bibtex",
        "open_filter",
        "toggle_read",
        "toggle_queue_filter",
    }
    _AUTHORS_ONLY_ACTIONS = {"open_website"}

    def check_action(self, action: str, parameters: tuple[object, ...]) -> bool | None:
        in_authors_mode = getattr(self.app, "_mode", "papers") == "authors"
        if in_authors_mode and action in self._PAPERS_ONLY_ACTIONS:
            return False
        if not in_authors_mode and action in self._AUTHORS_ONLY_ACTIONS:
            return False
        return True


class SearchInput(Input):
    BINDINGS = [
        Binding("shift+tab", "open_filter_from_search", "Filter", show=False),
    ]

    def action_open_filter_from_search(self) -> None:
        self.app.action_open_filter_from_search()


class FilterPicker(ModalScreen[list[tuple[str, str]] | None]):
    BINDINGS = [
        Binding("escape", "finish", "Finish", show=False),
        Binding("q", "finish", "Finish", show=False),
        Binding("enter", "enter_key", "Select / Next", show=False, priority=True),
        Binding("down,j", "cursor_down", "Down", show=False),
        Binding("up,k", "cursor_up", "Up", show=False),
        Binding("tab", "toggle", "Toggle", show=False),
        Binding("shift+tab", "focus_list", "Navigate", show=False),
    ]

    CSS = f"""
    FilterPicker {{
        align: center middle;
    }}
    #filter-box {{
        width: 50;
        height: auto;
        max-height: 80%;
        border: tall {CAT['mauve']};
        background: {CAT['base']};
        color: {CAT['text']};
    }}
    #filter-hint {{
        color: {CAT['overlay1']};
        margin: 0 1;
    }}
    """

    def __init__(self, active_filters: list[tuple[str, str]], initial_query: str = "") -> None:
        super().__init__()
        self._selected: set[tuple[str, str]] = set(active_filters)
        self._entries: list[tuple[str, str]] = []
        self._initial_query = initial_query

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="filter-box"):
            yield Input(placeholder="Type to filter…", id="filter-input")
            yield OptionList(id="filter-options")
            yield Static(
                "Enter (search): jump to list · Enter/Tab (list): mark · q/Esc: apply",
                id="filter-hint",
            )

    def on_mount(self) -> None:
        for name in sorted(load_projects()):
            self._entries.append((f"Project: {name}", f"project:{name}"))
        for name in sorted(load_keywords()):
            self._entries.append((f"Keyword: {name}", f"keyword:{name}"))

        input_widget = self.query_one("#filter-input", Input)
        input_widget.value = self._initial_query
        self._render_options(self._initial_query)
        input_widget.focus()

    def action_focus_list(self) -> None:
        ol = self.query_one("#filter-options", OptionList)
        inp = self.query_one("#filter-input", Input)
        if self.focused is ol:
            inp.focus()
        else:
            ol.focus()

    def on_input_changed(self, event: Input.Changed) -> None:
        self._render_options(event.value)

    def _option_label(self, base_label: str, option_id: str) -> str:
        kind, name = option_id.split(":", 1)
        checked = (kind, name) in self._selected
        icon = "PR" if kind == "project" else "KW"
        name_color = CAT["sapphire"] if kind == "project" else CAT["mauve"]
        if checked:
            box = f"[{CAT['green']}][x][/{CAT['green']}]"
        else:
            box = f"[{CAT['overlay0']}][ ][/{CAT['overlay0']}]"
        return f"{box} {icon} [{name_color}]{name}[/{name_color}]"

    def _render_options(self, query: str, preserve_id: str | None = None) -> None:
        ol = self.query_one("#filter-options", OptionList)
        ol.clear_options()
        options: list[Option] = []
        if self._selected:
            options.append(Option("x  Clear all filters", id="clear"))

        q = query.strip().lower()
        matches = [
            e for e in self._entries if q in e[0].lower()
        ] if q else self._entries
        options.extend(
            Option(self._option_label(label, oid), id=oid) for label, oid in matches
        )

        ol.add_options(options)
        if not options:
            return

        target_idx = 0
        if preserve_id is not None:
            for i, opt in enumerate(options):
                if opt.id == preserve_id:
                    target_idx = i
                    break
            else:
                target_idx = 1 if len(options) > 1 else 0
        elif self._selected and len(options) > 1:
            # Skip past "Clear all filters" so Tab lands on a real entry.
            target_idx = 1
        ol.highlighted = target_idx

    def action_cursor_down(self) -> None:
        self.query_one("#filter-options", OptionList).action_cursor_down()

    def action_cursor_up(self) -> None:
        self.query_one("#filter-options", OptionList).action_cursor_up()

    def action_toggle(self) -> None:
        ol = self.query_one("#filter-options", OptionList)
        if ol.highlighted is None:
            return
        option = ol.get_option_at_index(ol.highlighted)
        option_id = option.id or ""
        if option_id == "clear":
            self._selected.clear()
        elif ":" in option_id:
            kind, name = option_id.split(":", 1)
            key = (kind, name)
            if key in self._selected:
                self._selected.discard(key)
            else:
                self._selected.add(key)

        preserve = None if option_id == "clear" else option_id
        self._render_options(self._initial_query, preserve_id=preserve)

    def action_finish(self) -> None:
        self.dismiss(sorted(self._selected))

    def action_enter_key(self) -> None:
        input_widget = self.query_one("#filter-input", Input)
        if self.focused is input_widget:
            self.action_focus_list()
            return
        self.action_toggle()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        # Mouse click path — toggle then apply immediately.
        option_id = event.option.id or ""
        if option_id == "clear":
            self.dismiss([])
            return
        if ":" in option_id:
            kind, name = option_id.split(":", 1)
            key = (kind, name)
            if key in self._selected:
                self._selected.discard(key)
            else:
                self._selected.add(key)
        self.dismiss(sorted(self._selected))


class NamePickerScreen(ModalScreen[list[str] | None]):
    """Pick one or more names (tags or projects) from a filterable list, with
    an inline "+ Create '...'" option when the typed text is new. Enter in
    the search box jumps to the list (like Shift+Tab); Enter or Tab on the
    list marks an entry without closing, so multiple can be marked; q/Esc
    applies everything marked and closes."""

    BINDINGS = [
        Binding("escape", "finish", "Finish", show=False),
        Binding("q", "finish", "Finish", show=False),
        Binding("enter", "enter_key", "Select / Next", show=False, priority=True),
        Binding("down,j", "cursor_down", "Down", show=False),
        Binding("up,k", "cursor_up", "Up", show=False),
        Binding("tab", "toggle", "Toggle", show=False),
        Binding("shift+tab", "focus_list", "Navigate", show=False),
    ]

    CSS = f"""
    NamePickerScreen {{
        align: center middle;
    }}
    #picker-box {{
        width: 50;
        height: auto;
        max-height: 80%;
        border: tall {CAT['mauve']};
        background: {CAT['base']};
        color: {CAT['text']};
    }}
    #picker-hint {{
        color: {CAT['overlay1']};
        margin: 0 1;
    }}
    """

    def __init__(self, label: str, known: set[str], current: set[str]) -> None:
        super().__init__()
        self._label = label
        self._known: set[str] = set(known)
        self._current = current
        self._pending: set[str] = set()
        self._last_query = ""

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="picker-box"):
            yield Input(
                placeholder=f"Type to filter or create a {self._label.lower()}…",
                id="picker-input",
            )
            yield OptionList(id="picker-options")
            yield Static(
                "Enter (search): jump to list · Enter/Tab (list): mark · q/Esc: apply",
                id="picker-hint",
            )

    def on_mount(self) -> None:
        self._render_options("")
        self.query_one("#picker-input", Input).focus()

    def on_input_changed(self, event: Input.Changed) -> None:
        self._render_options(event.value)

    def _effective_checked(self, name: str) -> bool:
        return (name in self._current) != (name in self._pending)

    def _option_label(self, name: str) -> str:
        checked = self._effective_checked(name)
        marker = (
            f"[{CAT['green']}][x][/{CAT['green']}]"
            if checked
            else f"[{CAT['overlay0']}][ ][/{CAT['overlay0']}]"
        )
        mark = f" [{CAT['yellow']}]*[/{CAT['yellow']}]" if name in self._pending else ""
        return f"{marker} {escape(name)}{mark}"

    def _render_options(self, query: str, preserve_id: str | None = None) -> None:
        self._last_query = query
        ol = self.query_one("#picker-options", OptionList)
        ol.clear_options()
        q = query.strip()
        ql = q.lower()
        names = sorted(self._known)
        matches = [n for n in names if ql in n.lower()] if ql else names

        options: list[Option] = []
        exact = bool(q) and any(n.lower() == ql for n in names)
        if q and not exact:
            options.append(
                Option(f"[{CAT['green']}]+ Create '{escape(q)}'[/{CAT['green']}]", id=f"create:{q}")
            )
        options.extend(Option(self._option_label(n), id=f"pick:{n}") for n in matches)

        ol.add_options(options)
        if not options:
            return

        target_idx = 0
        if preserve_id is not None:
            for i, opt in enumerate(options):
                if opt.id == preserve_id:
                    target_idx = i
                    break
        ol.highlighted = target_idx

    def action_cursor_down(self) -> None:
        self.query_one("#picker-options", OptionList).action_cursor_down()

    def action_cursor_up(self) -> None:
        self.query_one("#picker-options", OptionList).action_cursor_up()

    def action_focus_list(self) -> None:
        ol = self.query_one("#picker-options", OptionList)
        inp = self.query_one("#picker-input", Input)
        if self.focused is ol:
            inp.focus()
        else:
            ol.focus()

    def _toggle_option_id(self, option_id: str) -> None:
        if option_id.startswith("create:"):
            name = option_id[len("create:") :]
            self._known.add(name)
        elif option_id.startswith("pick:"):
            name = option_id[len("pick:") :]
        else:
            return
        if name in self._pending:
            self._pending.discard(name)
        else:
            self._pending.add(name)
        self._render_options(self._last_query, preserve_id=f"pick:{name}")

    def action_toggle(self) -> None:
        ol = self.query_one("#picker-options", OptionList)
        if ol.highlighted is None:
            return
        option = ol.get_option_at_index(ol.highlighted)
        self._toggle_option_id(option.id or "")

    def action_finish(self) -> None:
        self.dismiss(sorted(self._pending))

    def action_enter_key(self) -> None:
        input_widget = self.query_one("#picker-input", Input)
        if self.focused is input_widget:
            self.action_focus_list()
            return
        self.action_toggle()

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        # Mouse click path — toggle then apply immediately.
        self._toggle_option_id(event.option.id or "")
        self.dismiss(sorted(self._pending))


HELP_TEXT = f"""\
[bold {CAT['yellow']}]Navigation[/bold {CAT['yellow']}]
  j / down        Move down
  k / up          Move up
  g               Jump to top
  G               Jump to bottom
  o               Show detail panel
  Escape          Focus list / cancel
  /               Focus search
  Shift+A         Toggle papers / authors view

[bold {CAT['yellow']}]Authors view[/bold {CAT['yellow']}]
  Type in search          Live-filter authors by name/lab/university/etc.
  Enter (on an author)    Filter papers list to the highlighted author
  e               Edit that author's profile (opens $EDITOR)
  w               Open that author's website in your browser
                  Each author's detail panel also lists their co-authors,
                  ranked by number of shared papers.

[bold {CAT['yellow']}]Items[/bold {CAT['yellow']}]
  e               Edit item (opens $EDITOR)
  a               Add item from BibTeX
                  (point the PDF prompt at a directory to attach every
                  PDF inside)
  p               Attach an additional PDF to an item
                  (point it at a directory to attach every PDF inside)
  d               Delete item
  m               Add/remove item from one or more projects — opens a
                  picker (type to filter, Shift+Tab then j/k to move,
                  Tab to mark several, Enter to apply/create)
  t               Add/remove one or more tags on the item (same picker
                  as 'm')
  c               Jump to authors view, showing only this paper's
                  authors
  r               Add/remove item from the reading queue (★)
  R               Show only reading-queue items (toggle)

[bold {CAT['yellow']}]Filtering & lists[/bold {CAT['yellow']}]
  Type a keyword/project name in search, Enter  Apply as filter
  Shift+Tab (in search)  Open filter picker, pre-filtered by what
                  you typed — j/k to move, Tab to stack multiple,
                  Enter/Esc to apply
  f               Open filter picker from the list (all entries)
  K               Edit the keyword list — deleting a name unlinks it
                  from every paper; replacing exactly one name with
                  exactly one other offers to rename it everywhere
                  instead
  M               Edit the project list (same rename/removal
                  behavior as K, above)
  x               Copy the highlighted item's BibTeX to the clipboard
  X               Export every item in the current filtered view to a
                  .bib file (prompts for a path; blank to cancel)

[bold {CAT['yellow']}]Other[/bold {CAT['yellow']}]
  Ctrl+S          Cycle sort order (papers: Date added -> A-Z ->
                  Publication date; authors: Profiled first -> A-Z ->
                  Most papers). Current sort shows in the status bar.
  Ctrl+R          Refresh library from disk
  ?               This help screen
  q               Focus list / cancel (same as Escape)
  Ctrl+Q          Quit

Press any key to close.
"""


class HelpScreen(ModalScreen[None]):
    CSS = f"""
    HelpScreen {{
        align: center middle;
    }}
    #help-box {{
        width: 86;
        max-width: 95%;
        height: auto;
        max-height: 80%;
        border: tall {CAT['mauve']};
        background: {CAT['base']};
        color: {CAT['text']};
        padding: 1 2;
    }}
    """

    def compose(self) -> ComposeResult:
        with VerticalScroll(id="help-box"):
            yield Static(HELP_TEXT)

    def on_key(self, event) -> None:
        if event.key == "j":
            self.query_one("#help-box", VerticalScroll).scroll_down(animate=False)
            event.stop()
            return
        if event.key == "k":
            self.query_one("#help-box", VerticalScroll).scroll_up(animate=False)
            event.stop()
            return
        self.dismiss(None)


ZOTERO_DB = Path("/home/gkerr/Zotero/zotero.sqlite")
ZOTERO_STORAGE = Path("/home/gkerr/Zotero/storage")


DOWNLOADS_DIR = Path.home() / "Downloads"

NEW_ITEM_TEMPLATE = """\
% Paste a BibTeX entry below (e.g. Google Scholar's 'Cite' -> BibTeX),
% or just fill in the fields by hand. Save and quit when done.

@article{,
  title = {},
  author = {},
  year = {},
  journal = {},
  doi = {},
  url = {},
}
"""


def _latest_download_pdf() -> Path | None:
    if not DOWNLOADS_DIR.exists():
        return None
    pdfs = sorted(
        DOWNLOADS_DIR.glob("*.pdf"), key=lambda p: p.stat().st_mtime, reverse=True
    )
    return pdfs[0] if pdfs else None


@dataclass
class ZoteroItem:
    item_id: int
    title: str
    authors: str | None = None
    tags: str | None = None
    type_name: str | None = None
    pdf_path: Path | None = None
    doi: str | None = None
    isbn: str | None = None
    issn: str | None = None
    pmid: str | None = None
    pmcid: str | None = None
    abstract: str | None = None
    date: str | None = None
    url: str | None = None
    publication_title: str | None = None
    journal_abbrev: str | None = None
    volume: str | None = None
    issue: str | None = None
    pages: str | None = None
    publisher: str | None = None
    place: str | None = None
    edition: str | None = None
    series: str | None = None
    series_number: str | None = None
    num_pages: str | None = None
    language: str | None = None
    book_title: str | None = None
    conference_name: str | None = None
    proceedings_title: str | None = None
    short_title: str | None = None
    call_number: str | None = None
    archive_id: str | None = None
    repository: str | None = None
    library_catalog: str | None = None
    rights: str | None = None
    extra: str | None = None
    access_date: str | None = None
    date_added: str | None = None
    to_read: bool = False

    @property
    def has_pdf(self) -> bool:
        return self.pdf_path is not None and self.pdf_path.exists()

    def open_pdf(self, path: Path | None = None) -> None:
        target = path or self.pdf_path
        if target is not None and target.exists():
            subprocess.Popen(
                ["zathura", str(target)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

    def _condensed_authors(self) -> str:
        if not self.authors:
            return "Unknown"
        last_names = [
            entry.strip().split(",")[0].strip()
            for entry in self.authors.split(";")
            if entry.strip()
        ]
        if not last_names:
            return "Unknown"
        if len(last_names) == 1:
            return last_names[0]
        if len(last_names) == 2:
            return f"{last_names[0]} & {last_names[1]}"
        return f"{last_names[0]} ... {last_names[-1]}"

    def list_markup(self) -> str:
        year = self.date[:4] if self.date else "n.d."
        author = self._condensed_authors()
        title_str = self.title[:80]
        is_book = self.type_name == "book"
        queue_marker = f"[{CAT['yellow']}]★[/{CAT['yellow']}] " if self.to_read else ""
        if self.has_pdf:
            title_color = CAT["peach"] if is_book else CAT["text"]
            return (
                f"{queue_marker}[{CAT['green']}]*[/{CAT['green']}] "
                f"[bold {CAT['blue']}]{escape(author)}[/bold {CAT['blue']}] "
                f"[{CAT['overlay0']}]({year})[/{CAT['overlay0']}] "
                f"[{title_color}]{escape(title_str)}[/{title_color}]"
            )
        if is_book:
            return (
                f"{queue_marker}[{CAT['overlay0']}]- {escape(author)} ({year}) [/{CAT['overlay0']}]"
                f"[{CAT['peach']}]{escape(title_str)}[/{CAT['peach']}]"
            )
        return (
            f"{queue_marker}[{CAT['overlay0']}]- {escape(author)} ({year}) "
            f"{escape(title_str)}[/{CAT['overlay0']}]"
        )

    def match_score(self, words: list[str]) -> int:
        title = (self.title or "").lower()
        authors = (self.authors or "").lower()
        tags = (self.tags or "").lower()
        rest = self.search_text()
        score = 0
        for w in words:
            if w in title:
                score += 4
            if w in authors:
                score += 2
            if w in tags:
                score += 2
            if w in rest:
                score += 1
        return score

    def search_text(self) -> str:
        return " ".join(
            filter(
                None,
                [
                    self.title,
                    self.authors,
                    self.tags,
                    self.publication_title,
                    self.type_name,
                    self.date,
                    self.abstract,
                    self.doi,
                    self.publisher,
                ],
            )
        ).lower()


def _resolve_pdf_path(path: str, key: str) -> Path:
    if path.startswith("storage:"):
        return ZOTERO_STORAGE / key / path.removeprefix("storage:")
    return Path(path)


def _get(row, key):
    v = row.get(key)
    return None if pd.isna(v) else v


def load_library(with_pdf_only: bool = False) -> list[ZoteroItem]:
    con = sqlite3.connect(f"file:{ZOTERO_DB}?mode=ro", uri=True)

    wide = pd.read_sql(
        """
        SELECT i.itemID, f.fieldName, idv.value
        FROM items i
        JOIN itemData id ON i.itemID = id.itemID
        JOIN fields f ON id.fieldID = f.fieldID
        JOIN itemDataValues idv ON id.valueID = idv.valueID
        WHERE i.itemTypeID != 3
        """,
        con,
    ).pivot_table(
        index="itemID", columns="fieldName", values="value", aggfunc="first"
    )

    authors = pd.read_sql(
        """
        SELECT i.itemID,
               GROUP_CONCAT(c.lastName || ', ' || c.firstName, '; ') AS authors
        FROM items i
        JOIN itemCreators ic ON i.itemID = ic.itemID
        JOIN creators c ON ic.creatorID = c.creatorID
        WHERE i.itemTypeID != 3
        GROUP BY i.itemID
        """,
        con,
    ).set_index("itemID")

    tags = pd.read_sql(
        """
        SELECT i.itemID,
               GROUP_CONCAT(t.name, '; ') AS tags
        FROM items i
        JOIN itemTags it ON i.itemID = it.itemID
        JOIN tags t ON it.tagID = t.tagID
        WHERE i.itemTypeID != 3
        GROUP BY i.itemID
        """,
        con,
    ).set_index("itemID")

    types = pd.read_sql(
        """
        SELECT i.itemID, it.typeName
        FROM items i
        JOIN itemTypes it ON i.itemTypeID = it.itemTypeID
        WHERE i.itemTypeID != 3
        """,
        con,
    ).set_index("itemID")

    attachments = pd.read_sql(
        """
        SELECT ia.parentItemID AS itemID, i.key, ia.path
        FROM itemAttachments ia
        JOIN items i ON ia.itemID = i.itemID
        WHERE ia.contentType = 'application/pdf'
          AND ia.path IS NOT NULL
        """,
        con,
    )
    attachments["resolved_path"] = attachments.apply(
        lambda r: _resolve_pdf_path(r["path"], r["key"]), axis=1
    )
    # An item can have multiple PDF attachments (e.g. a book split into
    # per-chapter files) — keep only one so the item isn't duplicated below.
    attachments = attachments.drop_duplicates(subset="itemID", keep="first")
    attachments = attachments.set_index("itemID")[["resolved_path"]]

    date_added = pd.read_sql(
        "SELECT itemID, dateAdded FROM items WHERE itemTypeID != 3",
        con,
    ).set_index("itemID")

    con.close()

    df = wide.join([authors, tags, types, attachments, date_added], how="left")
    df = df[df["title"].notna()]
    if with_pdf_only:
        df = df[df["resolved_path"].notna()]
    df = df.sort_values("dateAdded", ascending=False)

    return [
        ZoteroItem(
            item_id=item_id,
            title=row["title"],
            authors=_get(row, "authors"),
            tags=_get(row, "tags"),
            type_name=_get(row, "typeName"),
            pdf_path=(
                row.get("resolved_path")
                if not pd.isna(row.get("resolved_path"))
                else None
            ),
            doi=_get(row, "DOI"),
            isbn=_get(row, "ISBN"),
            issn=_get(row, "ISSN"),
            pmid=_get(row, "PMID"),
            pmcid=_get(row, "PMCID"),
            abstract=_get(row, "abstractNote"),
            date=_get(row, "date"),
            url=_get(row, "url"),
            publication_title=_get(row, "publicationTitle"),
            journal_abbrev=_get(row, "journalAbbreviation"),
            volume=_get(row, "volume"),
            issue=_get(row, "issue"),
            pages=_get(row, "pages"),
            publisher=_get(row, "publisher"),
            place=_get(row, "place"),
            edition=_get(row, "edition"),
            series=_get(row, "series"),
            series_number=_get(row, "seriesNumber"),
            num_pages=_get(row, "numPages"),
            language=_get(row, "language"),
            book_title=_get(row, "bookTitle"),
            conference_name=_get(row, "conferenceName"),
            proceedings_title=_get(row, "proceedingsTitle"),
            short_title=_get(row, "shortTitle"),
            call_number=_get(row, "callNumber"),
            archive_id=_get(row, "archiveID"),
            repository=_get(row, "repository"),
            library_catalog=_get(row, "libraryCatalog"),
            rights=_get(row, "rights"),
            extra=_get(row, "extra"),
            access_date=_get(row, "accessDate"),
            date_added=_get(row, "dateAdded"),
        )
        for item_id, row in df.iterrows()
    ]


def _truncate(value: str, width: int = 55) -> str:
    value = " ".join(value.split())
    if len(value) > width:
        return value[: width - 1] + "…"
    return value


def _wrap_title(title: str, width: int = 65) -> tuple[str, str]:
    """Split title into exactly two fixed-width lines (second may be blank)."""
    title = " ".join(title.split())
    if len(title) <= width:
        return title, ""
    line1 = title[:width]
    remainder = title[width:]
    line2 = remainder[: width - 1] + "…" if len(remainder) > width else remainder
    return line1, line2


_SQL_DATE_PREFIX_RE = re.compile(r"\d{4}(-\d{2}){0,2}")


def _display_date(value: str | None) -> str | None:
    """Zotero stores the date field as "<sql-normalized-date> <original text>"
    (e.g. "1982-09-10 1982/09/10"). Strip the redundant SQL-normalized prefix
    so only the original, human-entered date is shown."""
    if not value or " " not in value:
        return value
    prefix, rest = value.split(" ", 1)
    if _SQL_DATE_PREFIX_RE.fullmatch(prefix):
        return rest
    return value


def _link_open_tag(url: str) -> str:
    """Textual's markup requires a link's URL as a quoted string value:
    [link="URL"]...[/link] — a bare [link=URL] (Rich console-markup style)
    raises MarkupError as soon as the URL contains a ":" or "/"."""
    safe_url = url.replace("\\", "").replace('"', "")
    return f'[link="{safe_url}"]'


_AUTHOR_PROFILE_FIELDS = (
    "email",
    "lab",
    "department",
    "university",
    "location",
    "urls",
    "research_areas",
    "notes",
)


def _has_profile_info(record: dict) -> bool:
    return any(record.get(f) for f in _AUTHOR_PROFILE_FIELDS)


def _display_name_from_entry(raw: str) -> str:
    if "," in raw:
        last, first = raw.split(",", 1)
        first, last = first.strip(), last.strip()
        return f"{first} {last}".strip() if first else last
    return raw.strip()


def _distinct_author_entries(items: list[ZoteroItem]) -> list[tuple[tuple[str, str], str]]:
    """[(name_key, display_name), ...] for every distinct author across items."""
    seen: dict[tuple[str, str], str] = {}
    for it in items:
        if not it.authors:
            continue
        for raw in it.authors.split(";"):
            raw = raw.strip()
            if not raw:
                continue
            key = rolodex_module.author_key_from_entry(raw)
            if key not in seen:
                seen[key] = _display_name_from_entry(raw)
    return list(seen.items())


def _build_canonical_map(rows: list[dict]) -> dict[tuple[str, str], tuple[str, str]]:
    """Map every author's (last_key, first_key) to the key of the canonical
    record it should display/group under (itself, unless canonical_id is
    set — e.g. "G. H. McKinley" resolves to "Gareth H. McKinley")."""
    by_id = {r["id"]: r for r in rows}

    def resolve(r: dict) -> dict:
        seen: set[int] = set()
        while r.get("canonical_id") and r["id"] not in seen:
            seen.add(r["id"])
            nxt = by_id.get(r["canonical_id"])
            if nxt is None:
                break
            r = nxt
        return r

    return {
        (r["last_key"], r["first_key"]): (
            resolve(r)["last_key"],
            resolve(r)["first_key"],
        )
        for r in rows
    }


def _resolve_key(
    key: tuple[str, str], canonical_map: dict[tuple[str, str], tuple[str, str]]
) -> tuple[str, str]:
    return canonical_map.get(key, key)


def _author_records_for(
    authors: str | None, authors_by_key: dict, canonical_map: dict
) -> list[dict]:
    """Author DB records for `authors` ("Last, First; ...") that actually
    have profile info filled in — used for the papers-mode 'Profile' rows."""
    if not authors:
        return []
    records = []
    seen_ids: set[int] = set()
    for entry in authors.split(";"):
        entry = entry.strip()
        if not entry:
            continue
        key = _resolve_key(rolodex_module.author_key_from_entry(entry), canonical_map)
        record = authors_by_key.get(key)
        if record and record["id"] not in seen_ids and _has_profile_info(record):
            records.append(record)
            seen_ids.add(record["id"])
    return records


@dataclass
class AuthorEntry:
    key: tuple[str, str]
    record: dict
    items: list[ZoteroItem]

    @property
    def name(self) -> str:
        return self.record["name"]

    @property
    def count(self) -> int:
        return len(self.items)

    @property
    def has_profile(self) -> bool:
        return _has_profile_info(self.record)


def _author_search_text(entry: AuthorEntry) -> str:
    r = entry.record
    return " ".join(
        filter(
            None,
            [
                entry.name.lower(),
                (r.get("email") or "").lower(),
                (r.get("lab") or "").lower(),
                (r.get("department") or "").lower(),
                (r.get("university") or "").lower(),
                (r.get("location") or "").lower(),
                (r.get("research_areas") or "").lower(),
                (r.get("notes") or "").lower(),
            ],
        )
    )


def _build_author_entries(
    items: list[ZoteroItem], authors_by_key: dict, canonical_map: dict
) -> list[AuthorEntry]:
    grouped: dict[tuple[str, str], list[ZoteroItem]] = {}
    for it in items:
        if not it.authors:
            continue
        for raw in it.authors.split(";"):
            raw = raw.strip()
            if not raw:
                continue
            key = _resolve_key(rolodex_module.author_key_from_entry(raw), canonical_map)
            grouped.setdefault(key, []).append(it)

    entries = []
    for key, its in grouped.items():
        record = authors_by_key.get(key)
        if record is None:
            continue
        entries.append(AuthorEntry(key=key, record=record, items=its))
    return entries


PAPERS_SORT_MODES = ["date_added", "alpha", "date_of_work"]
PAPERS_SORT_LABELS = {
    "date_added": "Date added",
    "alpha": "A-Z",
    "date_of_work": "Publication date",
}
AUTHORS_SORT_MODES = ["profile", "alpha", "papers"]
AUTHORS_SORT_LABELS = {
    "profile": "Profiled first",
    "alpha": "A-Z",
    "papers": "Most papers",
}


def _sort_key_author(item: ZoteroItem) -> str:
    if not item.authors:
        return "￿"
    return item.authors.split(";")[0].strip().split(",")[0].strip().lower()


def _sort_items(items: list[ZoteroItem], mode: str) -> list[ZoteroItem]:
    if mode == "alpha":
        return sorted(items, key=lambda it: (_sort_key_author(it), it.title or ""))
    if mode == "date_of_work":
        return sorted(items, key=lambda it: it.date or "", reverse=True)
    return sorted(items, key=lambda it: it.date_added or "", reverse=True)


def _sort_author_entries(entries: list[AuthorEntry], mode: str) -> list[AuthorEntry]:
    if mode == "alpha":
        return sorted(entries, key=lambda e: e.key)
    if mode == "papers":
        return sorted(entries, key=lambda e: (-e.count, e.key))
    return sorted(entries, key=lambda e: (not e.has_profile, e.key))


def _author_markup(entry: AuthorEntry) -> str:
    plural = "paper" if entry.count == 1 else "papers"
    if entry.has_profile:
        marker = f"[{CAT['green']}]*[/{CAT['green']}]"
        name_style = f"[bold {CAT['blue']}]{escape(entry.name)}[/bold {CAT['blue']}]"
    else:
        marker = f"[{CAT['overlay0']}]-[/{CAT['overlay0']}]"
        name_style = f"[{CAT['text']}]{escape(entry.name)}[/{CAT['text']}]"

    bits = [entry.record.get("university"), entry.record.get("location")]
    detail = ", ".join(escape(b) for b in bits if b)
    detail_markup = f" [{CAT['text']}]{detail}[/{CAT['text']}]" if detail else ""

    return (
        f"{marker} {name_style} "
        f"[{CAT['overlay0']}]({entry.count} {plural})[/{CAT['overlay0']}]"
        f"{detail_markup}"
    )


def _co_authors_for(
    entry: AuthorEntry, authors_by_key: dict, canonical_map: dict
) -> list[tuple[dict, int]]:
    """[(author record, papers together), ...] for entry's co-authors, sorted
    by shared-paper count descending, then name."""
    counts: dict[tuple[str, str], int] = {}
    for it in entry.items:
        if not it.authors:
            continue
        for raw in it.authors.split(";"):
            raw = raw.strip()
            if not raw:
                continue
            key = _resolve_key(rolodex_module.author_key_from_entry(raw), canonical_map)
            if key == entry.key:
                continue
            counts[key] = counts.get(key, 0) + 1

    pairs = []
    for key, count in counts.items():
        record = authors_by_key.get(key)
        if record:
            pairs.append((record, count))
    pairs.sort(key=lambda rc: (-rc[1], rc[0]["name"]))
    return pairs


def _build_author_detail(
    entry: AuthorEntry, authors_by_key: dict, canonical_map: dict
) -> str:
    label_color = CAT["overlay1"]
    nan_color = CAT["overlay0"]
    value_color = CAT["text"]
    r = entry.record

    parts: list[str] = [
        f"[bold {CAT['yellow']}]{escape(entry.name)}[/bold {CAT['yellow']}]",
        "",
    ]

    def row(label: str, value: str | None, color: str = value_color) -> None:
        if value:
            text = f"[{color}]{escape(_truncate(value, width=70))}[/{color}]"
        else:
            text = f"[{nan_color}]NaN[/{nan_color}]"
        parts.append(f"[{label_color}]{label:<11}[/{label_color}] {text}")

    row("Email", r.get("email"))
    row("Lab", r.get("lab") or r.get("department"))
    row("Institution", r.get("university"))
    row("Location", r.get("location"))
    areas = r.get("research_areas")
    if areas:
        row("Areas", areas.replace("; ", ", "), color=CAT["mauve"])
    urls = [u for u in (r.get("urls") or "").split("; ") if u]
    if urls:
        link = (
            f"{_link_open_tag(urls[0])}"
            f"[{CAT['sapphire']}]{escape(urls[0])}[/{CAT['sapphire']}][/link]"
        )
        parts.append(f"[{label_color}]{'Website':<11}[/{label_color}] {link}")
    if r.get("notes"):
        row("Notes", r["notes"])
    if not entry.has_profile:
        parts.append(
            f"[{nan_color}]No profile info yet — press 'e' to fill it in[/{nan_color}]"
        )

    parts.append("")
    parts.append(f"[{label_color}]Papers ({entry.count})[/{label_color}]")
    for it in entry.items[:10]:
        title = it.title or "Untitled"
        parts.append(f"[{value_color}]  - {escape(_truncate(title, width=70))}[/{value_color}]")
    if entry.count > 10:
        parts.append(f"[{nan_color}]  … and {entry.count - 10} more[/{nan_color}]")

    co_authors = _co_authors_for(entry, authors_by_key, canonical_map)
    if co_authors:
        parts.append("")
        parts.append(f"[{label_color}]Co-authors ({len(co_authors)})[/{label_color}]")
        for record, count in co_authors[:10]:
            plural = "paper" if count == 1 else "papers"
            name_color = CAT["blue"] if _has_profile_info(record) else value_color
            parts.append(
                f"[{value_color}]  - [/{value_color}]"
                f"[{name_color}]{escape(record['name'])}[/{name_color}]"
                f"[{nan_color}] ({count} {plural} together)[/{nan_color}]"
            )
        if len(co_authors) > 10:
            parts.append(f"[{nan_color}]  … and {len(co_authors) - 10} more[/{nan_color}]")

    return "\n".join(parts)


_TOML_ESCAPE_TABLE = str.maketrans({"\\": "\\\\", '"': '\\"', "\n": "\\n"})


def _toml_escape(value: str) -> str:
    return value.translate(_TOML_ESCAPE_TABLE)


AUTHOR_EDIT_FIELDS = [
    "name",
    "email",
    "lab",
    "department",
    "university",
    "location",
    "urls",
    "research_areas",
    "notes",
]
_AUTHOR_LIST_FIELDS = {"urls", "research_areas"}


def _author_to_toml(record: dict) -> str:
    lines = [
        f"# Editing profile for: {record['name']}",
        "# Save and quit to apply changes. Leave a field blank to clear it.",
        "",
    ]
    for f in AUTHOR_EDIT_FIELDS:
        value = record.get(f)
        if f in _AUTHOR_LIST_FIELDS:
            items = [v for v in (value or "").split("; ") if v]
            joined = ", ".join(f'"{_toml_escape(v)}"' for v in items)
            lines.append(f"{f} = [{joined}]")
        else:
            lines.append(f'{f} = "{_toml_escape(value or "")}"')
    return "\n".join(lines) + "\n"


def _parse_author_toml(text: str) -> dict[str, str | None]:
    data = tomllib.loads(text)
    fields: dict[str, str | None] = {}
    for f in AUTHOR_EDIT_FIELDS:
        if f not in data:
            continue
        if f in _AUTHOR_LIST_FIELDS:
            items = [str(v).strip() for v in data[f] if str(v).strip()]
            fields[f] = "; ".join(items) or None
        else:
            value = str(data[f]).strip()
            fields[f] = value or None
    return fields


def _build_detail(item: ZoteroItem, authors_by_key: dict, canonical_map: dict) -> str:
    label_color = CAT["overlay1"]
    nan_color = CAT["overlay0"]
    value_color = CAT["text"]

    parts: list[str] = []
    if item.title:
        line1, line2 = _wrap_title(item.title)
    else:
        line1, line2 = "NaN", ""
    title_color = CAT["yellow"] if item.title else nan_color
    parts.append(f"[bold {title_color}]{escape(line1)}[/bold {title_color}]")
    parts.append(
        f"[bold {title_color}]{escape(line2)}[/bold {title_color}]" if line2 else ""
    )
    parts.append("")

    def row(label: str, value: str | None, color: str = value_color) -> None:
        if value:
            text = f"[{color}]{escape(_truncate(value))}[/{color}]"
        else:
            text = f"[{nan_color}]NaN[/{nan_color}]"
        parts.append(f"[{label_color}]{label:<11}[/{label_color}] {text}")

    if item.to_read:
        parts.append(
            f"[{label_color}]{'Queue':<11}[/{label_color}] "
            f"[{CAT['yellow']}]★ To read[/{CAT['yellow']}]"
        )

    row("Date", _display_date(item.date))
    row("Author", item.authors)

    for record in _author_records_for(item.authors, authors_by_key, canonical_map):
        bits = [record.get("lab") or record.get("department"), record.get("university")]
        detail = ", ".join(escape(b) for b in bits if b)
        name_text = escape(record["name"])
        urls = [u for u in (record.get("urls") or "").split("; ") if u]
        if urls:
            name_markup = (
                f"{_link_open_tag(urls[0])}"
                f"[{CAT['sapphire']}]{name_text}[/{CAT['sapphire']}][/link]"
            )
        else:
            name_markup = f"[{CAT['sapphire']}]{name_text}[/{CAT['sapphire']}]"
        value_markup = f"{name_markup} — {detail}" if detail else name_markup
        parts.append(f"[{label_color}]{'Profile':<11}[/{label_color}] {value_markup}")

    journal = item.publication_title or item.book_title or item.proceedings_title
    row("Journal", journal)
    row("Type", item.type_name)
    row("Tags", item.tags, color=CAT["mauve"])

    projects = db_module.projects_for_item(item.item_id)
    row(
        "Projects",
        "; ".join(projects) if projects else None,
        color=CAT["sapphire"],
    )

    pdfs = db_module.list_item_pdfs(item.item_id)
    if len(pdfs) > 1:
        pdf_status, pdf_color = f"Yes ({len(pdfs)} files)", CAT["green"]
    elif item.has_pdf:
        pdf_status, pdf_color = "Yes", CAT["green"]
    elif item.pdf_path:
        pdf_status, pdf_color = "Missing", CAT["red"]
    else:
        pdf_status, pdf_color = None, nan_color
    row("PDF", pdf_status, color=pdf_color)

    return "\n".join(parts)


class ZoteroApp(App):
    TITLE = "Zotero TUI"
    SUB_TITLE = "Loading..."

    CSS = f"""
    Screen {{
        background: {CAT['base']};
    }}

    #search {{
        margin: 1 1 0 1;
        border: tall {CAT['surface2']};
        background: {CAT['base']};
        color: {CAT['text']};
    }}

    #search:focus {{
        border: tall {CAT['mauve']};
    }}

    #main {{
        height: 1fr;
        margin: 0 1 0 1;
    }}

    #item-list {{
        width: 55%;
        border: tall {CAT['surface2']};
        background: {CAT['base']};
        color: {CAT['text']};
    }}

    #item-list:focus {{
        border: tall {CAT['mauve']};
    }}

    #item-list > .option-list--option-highlighted {{
        background: {CAT['surface1']};
        text-style: bold;
    }}

    #item-list:focus > .option-list--option-highlighted {{
        background: {CAT['surface2']};
        text-style: bold;
    }}

    #detail-panel {{
        width: 1fr;
        border: tall {CAT['surface2']};
        background: {CAT['base']};
        color: {CAT['text']};
    }}

    #detail-text {{
        height: auto;
        padding: 1 2;
    }}

    #status {{
        height: 1;
        background: {CAT['surface1']};
        color: {CAT['subtext0']};
        padding: 0 2;
    }}
    """

    BINDINGS = [
        Binding("/", "focus_search", "Search"),
        Binding("escape", "focus_list", "Browse", show=False),
        Binding("q", "focus_list", "Browse", show=False),
        Binding("ctrl+r", "refresh", "Refresh", show=False),
        Binding("question_mark", "help", "Help"),
        Binding("ctrl+q", "quit", "Quit"),
        Binding("A", "toggle_mode", "Authors ⇄ Papers"),
        Binding("ctrl+s", "cycle_sort", "Sort", show=False),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._items: list[ZoteroItem] = []
        self._filtered: list[ZoteroItem] = []
        self._search_timer = None
        self._active_filters: list[tuple[str, str]] = []
        self._mode: str = "papers"
        self._authors_by_key: dict[tuple[str, str], dict] = {}
        self._canonical_map: dict[tuple[str, str], tuple[str, str]] = {}
        self._author_entries: list[AuthorEntry] = []
        self._filtered_authors: list[AuthorEntry] = []
        self._papers_sort: str = PAPERS_SORT_MODES[0]
        self._authors_sort: str = AUTHORS_SORT_MODES[0]

    def compose(self) -> ComposeResult:
        yield Header()
        yield SearchInput(placeholder="Search title, author, tag, abstract...", id="search")
        with Horizontal(id="main"):
            yield VimListView(id="item-list")
            with VerticalScroll(id="detail-panel"):
                yield Static("", id="detail-text")
        yield Static("Loading library...", id="status")
        yield Footer()

    def on_mount(self) -> None:
        self._load_library()
        self.query_one("#item-list", VimListView).focus()

    @work(thread=True, exclusive=True)
    def _load_library(self) -> None:
        db_module.init_db()
        items = db_module.load_items()
        self.call_from_thread(self._on_loaded, items)

    def _on_loaded(self, items: list[ZoteroItem]) -> None:
        self._items = items
        self._sync_authors()
        self._update_subtitle()
        query = self.query_one("#search", Input).value
        if self._mode == "authors":
            self._author_entries = _build_author_entries(
                self._items, self._authors_by_key, self._canonical_map
            )
            self._apply_author_filter(query)
        else:
            self._apply_filter(query)

    def _sync_authors(self) -> None:
        seeds = rolodex_module.load_seed_contacts()
        entries = _distinct_author_entries(self._items)
        db_module.ensure_authors(entries, seed=seeds)
        rows = db_module.list_authors()
        self._authors_by_key = {(r["last_key"], r["first_key"]): r for r in rows}
        self._canonical_map = _build_canonical_map(rows)

    def _filter_label(self) -> str:
        if not self._active_filters:
            return ""
        icons = {"project": "PR", "keyword": "KW", "author": "AU"}
        parts = []
        for kind, name in self._active_filters:
            if kind == "queue":
                parts.append("Queue")
            else:
                parts.append(f"{icons.get(kind, '?')}:{name}")
        return " + ".join(parts) + " · "

    def _update_subtitle(self) -> None:
        if self._mode == "authors":
            n_profiled = sum(1 for e in self._author_entries if e.has_profile)
            self.sub_title = (
                f"Authors · {len(self._author_entries)} people · "
                f"{n_profiled} with profile info"
            )
            return
        n_pdf = sum(1 for i in self._items if i.has_pdf)
        self.sub_title = f"{self._filter_label()}{len(self._items)} items · {n_pdf} with PDF"

    def action_toggle_mode(self) -> None:
        self._mode = "authors" if self._mode == "papers" else "papers"
        query = self.query_one("#search", Input).value
        if self._mode == "authors":
            self._author_entries = _build_author_entries(
                self._items, self._authors_by_key, self._canonical_map
            )
            self._apply_author_filter(query)
        else:
            self._apply_filter(query)
        self._update_subtitle()
        self.query_one("#item-list", VimListView).refresh_bindings()

    def action_show_paper_authors(self) -> None:
        if not self._require_papers_mode():
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]
        if not item.authors:
            self.notify("No authors on this paper", severity="warning")
            return

        keys = set()
        for raw in item.authors.split(";"):
            raw = raw.strip()
            if raw:
                keys.add(_resolve_key(rolodex_module.author_key_from_entry(raw), self._canonical_map))

        self._mode = "authors"
        self._author_entries = _build_author_entries(
            self._items, self._authors_by_key, self._canonical_map
        )
        matched = [e for e in self._author_entries if e.key in keys]
        self._filtered_authors = _sort_author_entries(matched, self._authors_sort)
        self._rebuild_author_list()
        self._update_subtitle()
        self.query_one("#item-list", VimListView).refresh_bindings()
        short_title = _truncate(item.title, width=40)
        self._set_status(f"Authors of '{short_title}' · {len(self._filtered_authors)}")

    def action_cycle_sort(self) -> None:
        query = self.query_one("#search", Input).value
        if self._mode == "authors":
            idx = AUTHORS_SORT_MODES.index(self._authors_sort)
            self._authors_sort = AUTHORS_SORT_MODES[(idx + 1) % len(AUTHORS_SORT_MODES)]
            self._apply_author_filter(query)
        else:
            idx = PAPERS_SORT_MODES.index(self._papers_sort)
            self._papers_sort = PAPERS_SORT_MODES[(idx + 1) % len(PAPERS_SORT_MODES)]
            self._apply_filter(query)

    def _rebuild_author_list(self) -> None:
        lv = self.query_one("#item-list", VimListView)
        lv.clear_options()
        lv.add_options([_author_markup(e) for e in self._filtered_authors])
        if self._filtered_authors and (
            lv.highlighted is None or lv.highlighted >= len(self._filtered_authors)
        ):
            lv.highlighted = 0
        # Rebuilding doesn't always change `highlighted`'s numeric value (e.g.
        # switching modes while both lists happen to be at index 0), and the
        # highlight-change event only fires on an actual value change — so
        # explicitly refresh the detail panel here instead of relying on it.
        if lv.highlighted is not None and 0 <= lv.highlighted < len(self._filtered_authors):
            self._show_author_detail(self._filtered_authors[lv.highlighted])
        elif not self._filtered_authors:
            self.query_one("#detail-text", Static).update("")

    def _apply_author_filter(self, query: str) -> None:
        q = query.strip().lower()
        if q:
            words = q.split()
            matched = [
                e
                for e in self._author_entries
                if all(w in _author_search_text(e) for w in words)
            ]
        else:
            matched = list(self._author_entries)
        self._filtered_authors = _sort_author_entries(matched, self._authors_sort)
        self._rebuild_author_list()
        n_profiled = sum(1 for e in matched if e.has_profile)
        self._set_status(
            f"Authors · {len(matched)} of {len(self._author_entries)} · "
            f"{n_profiled} with profile info · Sort: {AUTHORS_SORT_LABELS[self._authors_sort]}"
        )

    def _show_author_detail(self, entry: AuthorEntry) -> None:
        self.query_one("#detail-text", Static).update(
            _build_author_detail(entry, self._authors_by_key, self._canonical_map)
        )
        self.query_one("#detail-panel", VerticalScroll).scroll_home(animate=False)

    def _filter_to_author(self, entry: AuthorEntry) -> None:
        self._mode = "papers"
        self._active_filters = [("author", entry.name)]
        search = self.query_one("#search", Input)
        search.value = ""
        self._update_subtitle()
        self._apply_filter("")
        self.query_one("#item-list", VimListView).refresh_bindings()

    def action_open_website(self) -> None:
        if self._mode != "authors":
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered_authors)):
            return
        entry = self._filtered_authors[idx]
        urls = [u for u in (entry.record.get("urls") or "").split("; ") if u]
        if not urls:
            self.notify("No website on file for this author", severity="warning")
            return
        url = urls[0]
        if not re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*://", url):
            url = f"https://{url}"
        webbrowser.open(url)
        self._set_status(f"Opened {url}")

    def _edit_author(self) -> None:
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered_authors)):
            return
        entry = self._filtered_authors[idx]

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".toml", delete=False, prefix="rip-zotero-author-"
        ) as f:
            f.write(_author_to_toml(entry.record))
            tmp_path = Path(f.name)

        editor = os.environ.get("EDITOR", "nvim")
        try:
            with self.suspend():
                subprocess.call([editor, str(tmp_path)])
        except SuspendNotSupported:
            tmp_path.unlink(missing_ok=True)
            self.notify(
                "Editing isn't supported in this terminal environment",
                severity="error",
            )
            return

        try:
            text = tmp_path.read_text()
            fields = _parse_author_toml(text)
        except tomllib.TOMLDecodeError as e:
            self.notify(
                f"Invalid TOML: {e} Edits kept at {tmp_path}",
                severity="error",
                timeout=10,
            )
            return

        if not fields.get("name"):
            self.notify("Name cannot be empty — edit not applied", severity="warning")
            return

        changes = {k: v for k, v in fields.items() if v != entry.record.get(k)}
        if not changes:
            tmp_path.unlink(missing_ok=True)
            self._set_status("No changes")
            return

        db_module.update_author_fields(entry.record["id"], changes)
        tmp_path.unlink(missing_ok=True)

        rows = db_module.list_authors()
        self._authors_by_key = {(r["last_key"], r["first_key"]): r for r in rows}
        self._canonical_map = _build_canonical_map(rows)
        self._author_entries = _build_author_entries(
            self._items, self._authors_by_key, self._canonical_map
        )
        query = self.query_one("#search", Input).value
        self._apply_author_filter(query)
        if self._filtered_authors:
            new_idx = next(
                (i for i, e in enumerate(self._filtered_authors) if e.key == entry.key), 0
            )
            lv.highlighted = new_idx
            self._show_author_detail(self._filtered_authors[new_idx])
        self._set_status(f"Saved profile: {fields['name']}")

    def _scope_items(self) -> list[ZoteroItem]:
        items = self._items
        for kind, name in self._active_filters:
            if kind == "project":
                ids = db_module.project_item_ids(name)
                items = [it for it in items if int(it.item_id) in ids]
            elif kind == "keyword":
                items = [
                    it
                    for it in items
                    if name in [t.strip() for t in (it.tags or "").split(";")]
                ]
            elif kind == "author":
                target_key = _resolve_key(rolodex_module.name_key(name), self._canonical_map)
                items = [
                    it
                    for it in items
                    if any(
                        _resolve_key(
                            rolodex_module.author_key_from_entry(a), self._canonical_map
                        )
                        == target_key
                        for a in (it.authors or "").split(";")
                        if a.strip()
                    )
                ]
            elif kind == "queue":
                items = [it for it in items if it.to_read]
        return items

    def _handle_filter_result(self, result: list[tuple[str, str]] | None) -> None:
        if result is None:
            return
        self._active_filters = result
        self._update_subtitle()
        search = self.query_one("#search", Input)
        search.value = ""
        self._apply_filter("")

    def _require_papers_mode(self) -> bool:
        if self._mode == "authors":
            self.notify("Switch to papers view (Shift+A) first", severity="warning")
            return False
        return True

    def _confirm(self, prompt: str) -> bool:
        try:
            with self.suspend():
                try:
                    answer = input(prompt).strip().lower()
                except EOFError:
                    answer = ""
        except SuspendNotSupported:
            return False
        return answer == "y"

    def action_open_filter(self) -> None:
        if not self._require_papers_mode():
            return
        self.push_screen(FilterPicker(list(self._active_filters)), self._handle_filter_result)

    def action_open_filter_from_search(self) -> None:
        if not self._require_papers_mode():
            return
        query = self.query_one("#search", Input).value
        self.push_screen(
            FilterPicker(list(self._active_filters), initial_query=query),
            self._handle_filter_result,
        )

    def action_help(self) -> None:
        self.push_screen(HelpScreen())

    def action_refresh(self) -> None:
        self._set_status("Refreshing library…")
        self._load_library()

    def _rebuild_list(self) -> None:
        lv = self.query_one("#item-list", VimListView)
        lv.clear_options()
        lv.add_options([item.list_markup() for item in self._filtered])
        if self._filtered and (
            lv.highlighted is None or lv.highlighted >= len(self._filtered)
        ):
            lv.highlighted = 0
        # See _rebuild_author_list: don't rely on the highlight-change event,
        # since the numeric index can stay the same across a mode switch.
        if lv.highlighted is not None and 0 <= lv.highlighted < len(self._filtered):
            self._show_detail(self._filtered[lv.highlighted])
        elif not self._filtered:
            self.query_one("#detail-text", Static).update("")

    def _set_status(self, msg: str) -> None:
        self.query_one("#status", Static).update(msg)

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id != "search":
            return
        if self._search_timer is not None:
            self._search_timer.stop()
        if self._mode == "authors":
            self._search_timer = self.set_timer(
                0.3, lambda: self._apply_author_filter(event.value)
            )
        else:
            self._search_timer = self.set_timer(0.3, lambda: self._apply_filter(event.value))

    @work(thread=True, exclusive=True)
    def _apply_filter(self, query: str) -> None:
        if not self._items and not self._active_filters:
            self.call_from_thread(
                self._update_results,
                [],
                "No items yet — run `rz-migrate` to import your library",
            )
            return

        scope = self._scope_items()
        q = query.strip()
        label = self._filter_label()
        if q:
            words = q.lower().split()
            matched = [
                it for it in scope if all(w in it.search_text() for w in words)
            ]
            filtered = sorted(matched, key=lambda it: it.match_score(words), reverse=True)
            status = f"{label}{len(filtered)} of {len(scope)} match"
        else:
            filtered = _sort_items(list(scope), self._papers_sort)
            n_pdf = sum(1 for i in scope if i.has_pdf)
            status = (
                f"{label}{len(filtered)} items · {n_pdf} with PDF · "
                f"Sort: {PAPERS_SORT_LABELS[self._papers_sort]}"
            )
        self.call_from_thread(self._update_results, filtered, status)

    def _update_results(self, filtered: list[ZoteroItem], status: str) -> None:
        self._filtered = filtered
        self._set_status(status)
        self._rebuild_list()

    def _match_filter_by_name(self, text: str) -> tuple[str, str] | None:
        name = text.strip().lower()
        if not name:
            return None
        if name in load_projects():
            return ("project", name)
        if name in load_keywords():
            return ("keyword", name)
        return None

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id != "search":
            return
        match = self._match_filter_by_name(event.value)
        if match:
            self._active_filters = [match]
            self._update_subtitle()
            search = self.query_one("#search", Input)
            search.value = ""
            self._apply_filter("")
        self.query_one("#item-list", VimListView).focus()

    def on_option_list_option_highlighted(
        self, event: OptionList.OptionHighlighted
    ) -> None:
        idx = event.option_index
        if idx is None:
            return
        if self._mode == "authors":
            if 0 <= idx < len(self._filtered_authors):
                self._show_author_detail(self._filtered_authors[idx])
            return
        if 0 <= idx < len(self._filtered):
            self._show_detail(self._filtered[idx])

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        idx = event.option_index
        if idx is None:
            return
        if self._mode == "authors":
            if 0 <= idx < len(self._filtered_authors):
                self._filter_to_author(self._filtered_authors[idx])
            return
        if idx >= len(self._filtered):
            return
        item = self._filtered[idx]
        pdfs = db_module.list_item_pdfs(item.item_id)
        if len(pdfs) > 1:
            self._choose_and_open_pdf(item, pdfs)
        elif item.has_pdf:
            item.open_pdf()
            self._set_status(f"Opened: {item.pdf_path.name}")
        else:
            self.notify("No PDF attached for this item", severity="warning")

    def _choose_and_open_pdf(
        self, item: ZoteroItem, pdfs: list[tuple[str, str | None]]
    ) -> None:
        choice = ""
        try:
            with self.suspend():
                print(f"Multiple PDFs for '{item.title}':")
                for i, (path, label) in enumerate(pdfs, 1):
                    print(f"  {i}. {label or Path(path).name}")
                try:
                    choice = input("Choose number (blank to cancel): ").strip()
                except EOFError:
                    choice = ""
        except SuspendNotSupported:
            self.notify(
                "Choosing a PDF isn't supported in this terminal environment",
                severity="error",
            )
            return

        if not choice:
            self._set_status("Cancelled")
            return
        try:
            path, label = pdfs[int(choice) - 1]
        except (ValueError, IndexError):
            self.notify("Invalid choice", severity="warning")
            return
        item.open_pdf(Path(path))
        self._set_status(f"Opened: {label or Path(path).name}")

    def action_show_detail(self) -> None:
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None:
            return
        if self._mode == "authors":
            if 0 <= idx < len(self._filtered_authors):
                self._show_author_detail(self._filtered_authors[idx])
            return
        if 0 <= idx < len(self._filtered):
            self._show_detail(self._filtered[idx])

    def _show_detail(self, item: ZoteroItem) -> None:
        self.query_one("#detail-text", Static).update(
            _build_detail(item, self._authors_by_key, self._canonical_map)
        )
        self.query_one("#detail-panel", VerticalScroll).scroll_home(animate=False)

    def action_edit_item(self) -> None:
        if self._mode == "authors":
            self._edit_author()
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".toml", delete=False, prefix="rip-zotero-"
        ) as f:
            f.write(item_to_toml(item))
            tmp_path = Path(f.name)

        editor = os.environ.get("EDITOR", "nvim")
        try:
            with self.suspend():
                subprocess.call([editor, str(tmp_path)])
        except SuspendNotSupported:
            tmp_path.unlink(missing_ok=True)
            self.notify(
                "Editing isn't supported in this terminal environment",
                severity="error",
            )
            return

        try:
            text = tmp_path.read_text()
            changes = diff_fields(item, parse_toml_fields(text))
            if not changes:
                self._set_status("No changes")
                return
            if "tags" in changes:
                bad = unknown_tags(changes["tags"])
                if bad:
                    self.notify(
                        f"Unknown keyword(s): {', '.join(bad)} — add them via "
                        f"'K' first. Edits kept at {tmp_path}",
                        severity="error",
                        timeout=10,
                    )
                    return
            save_item_fields(item.item_id, changes)
        except ZoteroWriteError as e:
            self.notify(
                f"{e} Edits kept at {tmp_path}", severity="error", timeout=10
            )
            return
        else:
            tmp_path.unlink(missing_ok=True)

        for attr, new_value in changes.items():
            setattr(item, attr, new_value)
        lv.replace_option_prompt_at_index(idx, item.list_markup())
        self._show_detail(item)
        self._set_status(f"Saved: {', '.join(changes)}")

    def action_add_item(self) -> None:
        if not self._require_papers_mode():
            return
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".bib", delete=False, prefix="rip-zotero-new-"
        ) as f:
            f.write(NEW_ITEM_TEMPLATE)
            tmp_path = Path(f.name)

        editor = os.environ.get("EDITOR", "nvim")
        pdf_input = ""
        try:
            with self.suspend():
                subprocess.call([editor, str(tmp_path)])
                try:
                    pdf_input = input(
                        "PDF file or directory (blank to skip, 'def' = latest "
                        "~/Downloads PDF, a directory attaches every PDF inside): "
                    ).strip()
                except EOFError:
                    pdf_input = ""
        except SuspendNotSupported:
            tmp_path.unlink(missing_ok=True)
            self.notify(
                "Adding items isn't supported in this terminal environment",
                severity="error",
            )
            return

        text = tmp_path.read_text()
        tmp_path.unlink(missing_ok=True)

        fields = parse_bibtex(text)
        if not fields.get("title"):
            self.notify("No BibTeX entry found — nothing added", severity="warning")
            return

        item_id = db_module.create_item(fields)

        src = None
        if pdf_input.lower() == "def":
            src = _latest_download_pdf()
            if src is None:
                self.notify("No PDF found in ~/Downloads", severity="warning")
        elif pdf_input:
            src = Path(pdf_input).expanduser()

        if src is not None:
            if not src.exists():
                self.notify(f"Path not found: {src}", severity="warning")
            elif src.is_dir():
                files = sorted(src.glob("*.pdf"))
                if not files:
                    self.notify(f"No PDFs found in {src}", severity="warning")
                else:
                    for i, f in enumerate(files):
                        data = f.read_bytes()
                        if i == 0:
                            pdf_path = db_module.save_pdf(item_id, data)
                            db_module.update_item_fields(
                                item_id, {"pdf_path": str(pdf_path)}
                            )
                        else:
                            pdf_path = db_module.save_extra_pdf(item_id, data, i)
                        db_module.add_item_pdf(
                            item_id, str(pdf_path), f.stem, sort_order=i
                        )
            else:
                data = src.read_bytes()
                pdf_path = db_module.save_pdf(item_id, data)
                db_module.update_item_fields(item_id, {"pdf_path": str(pdf_path)})

        self._set_status(f"Added: {fields['title']}")
        self._load_library()

    def action_add_pdf(self) -> None:
        if not self._require_papers_mode():
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]

        pdf_input = ""
        label = ""
        try:
            with self.suspend():
                try:
                    pdf_input = input(
                        f"PDF file or directory to attach to '{item.title}' "
                        "(blank to cancel, 'def' = latest ~/Downloads PDF, "
                        "a directory attaches every PDF inside): "
                    ).strip()
                    src_is_dir = (
                        pdf_input
                        and pdf_input.lower() != "def"
                        and Path(pdf_input).expanduser().is_dir()
                    )
                    if pdf_input and not src_is_dir:
                        label = input(
                            "Label for this PDF (blank = filename): "
                        ).strip()
                except EOFError:
                    pdf_input = ""
        except SuspendNotSupported:
            self.notify(
                "Attaching PDFs isn't supported in this terminal environment",
                severity="error",
            )
            return

        if not pdf_input:
            self._set_status("Cancelled")
            return

        if pdf_input.lower() == "def":
            src = _latest_download_pdf()
            if src is None:
                self.notify("No PDF found in ~/Downloads", severity="warning")
                return
            sources = [(src, label)]
        else:
            src = Path(pdf_input).expanduser()
            if not src.exists():
                self.notify(f"Path not found: {src}", severity="warning")
                return
            if src.is_dir():
                files = sorted(src.glob("*.pdf"))
                if not files:
                    self.notify(f"No PDFs found in {src}", severity="warning")
                    return
                sources = [(f, f.stem) for f in files]
            else:
                sources = [(src, label)]

        existing = db_module.list_item_pdfs(item.item_id)
        if not existing and item.has_pdf:
            # First extra PDF for an item that only has the single legacy
            # pdf_path — seed item_pdfs with it so it's included in the list.
            db_module.add_item_pdf(
                item.item_id, str(item.pdf_path), None, sort_order=0
            )
            existing = db_module.list_item_pdfs(item.item_id)

        next_index = len(existing)
        attached_names: list[str] = []
        first_dest: Path | None = None
        for file_src, file_label in sources:
            if not file_src.exists():
                self.notify(f"PDF not found: {file_src}", severity="warning")
                continue
            data = file_src.read_bytes()
            dest = db_module.save_extra_pdf(item.item_id, data, next_index)
            db_module.add_item_pdf(
                item.item_id, str(dest), file_label or None, sort_order=next_index
            )
            attached_names.append(file_label or dest.name)
            first_dest = first_dest or dest
            next_index += 1

        if not attached_names:
            self._set_status("Nothing attached")
            return

        if not item.has_pdf and first_dest is not None:
            db_module.update_item_fields(item.item_id, {"pdf_path": str(first_dest)})
            item.pdf_path = first_dest
            lv.replace_option_prompt_at_index(idx, item.list_markup())

        if len(attached_names) == 1:
            self._set_status(f"Attached: {attached_names[0]} -> {item.title}")
        else:
            self._set_status(
                f"Attached {len(attached_names)} PDFs -> {item.title}"
            )
        self._show_detail(item)

    def action_delete_item(self) -> None:
        if not self._require_papers_mode():
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]

        confirmed = False
        try:
            with self.suspend():
                try:
                    answer = input(f"Delete '{item.title}'? [y/N] ").strip().lower()
                except EOFError:
                    answer = ""
                confirmed = answer == "y"
        except SuspendNotSupported:
            self.notify(
                "Deleting isn't supported in this terminal environment",
                severity="error",
            )
            return

        if not confirmed:
            self._set_status("Delete cancelled")
            return

        db_module.delete_item(item.item_id)
        self._set_status(f"Deleted: {item.title}")
        self._load_library()

    def action_toggle_read(self) -> None:
        if not self._require_papers_mode():
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]
        item.to_read = not item.to_read
        db_module.update_item_fields(item.item_id, {"to_read": int(item.to_read)})
        lv.replace_option_prompt_at_index(idx, item.list_markup())
        self._show_detail(item)
        verb = "Added to" if item.to_read else "Removed from"
        self._set_status(f"{verb} reading queue: {item.title}")

    def action_toggle_queue_filter(self) -> None:
        if not self._require_papers_mode():
            return
        if ("queue", "") in self._active_filters:
            self._active_filters.remove(("queue", ""))
        else:
            self._active_filters.append(("queue", ""))
        self._update_subtitle()
        self._apply_filter(self.query_one("#search", Input).value)

    def action_toggle_project(self) -> None:
        if not self._require_papers_mode():
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]
        current = set(db_module.projects_for_item(item.item_id))
        self.push_screen(
            NamePickerScreen("Project", load_projects(), current),
            lambda names: self._apply_project_pick(item, idx, names),
        )

    def _apply_project_pick(
        self, item: ZoteroItem, idx: int, names: list[str] | None
    ) -> None:
        short_title = _truncate(item.title, width=40)
        if not names:
            self._set_status("Cancelled")
            return

        added_names = []
        removed_names = []
        for raw_name in names:
            name = raw_name.strip().lower()
            if not name:
                continue
            if name not in load_projects():
                add_project(name)
            added = db_module.toggle_project_item(name, item.item_id)
            (added_names if added else removed_names).append(name)

        parts = []
        if added_names:
            parts.append(f"added {', '.join(added_names)}")
        if removed_names:
            parts.append(f"removed {', '.join(removed_names)}")
        self._set_status(f"Projects for '{short_title}': {'; '.join(parts)}")
        self._show_detail(item)

    def action_toggle_tag(self) -> None:
        if not self._require_papers_mode():
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]
        current = {t.strip() for t in (item.tags or "").split(";") if t.strip()}
        self.push_screen(
            NamePickerScreen("Tag", load_keywords(), current),
            lambda names: self._apply_tag_pick(item, idx, names),
        )

    def _apply_tag_pick(
        self, item: ZoteroItem, idx: int, names: list[str] | None
    ) -> None:
        short_title = _truncate(item.title, width=40)
        if not names:
            self._set_status("Cancelled")
            return

        current = [t.strip() for t in (item.tags or "").split(";") if t.strip()]
        added_names = []
        removed_names = []
        for raw_name in names:
            name = raw_name.strip()
            if not name:
                continue
            if name not in load_keywords():
                add_keyword(name)
            if name in current:
                current.remove(name)
                removed_names.append(name)
            else:
                current.append(name)
                added_names.append(name)

        item.tags = "; ".join(current) or None
        db_module.update_item_fields(item.item_id, {"tags": item.tags})
        lv = self.query_one("#item-list", VimListView)
        if 0 <= idx < len(self._filtered) and self._filtered[idx] is item:
            lv.replace_option_prompt_at_index(idx, item.list_markup())
        self._show_detail(item)

        parts = []
        if added_names:
            parts.append(f"added {', '.join(added_names)}")
        if removed_names:
            parts.append(f"removed {', '.join(removed_names)}")
        self._set_status(f"Tags for '{short_title}': {'; '.join(parts)}")

    def action_export_bibtex(self) -> None:
        if not self._require_papers_mode():
            return
        if not self._filtered:
            self.notify("Nothing to export in the current view", severity="warning")
            return

        out_input = ""
        try:
            with self.suspend():
                try:
                    out_input = input(
                        f"Export {len(self._filtered)} item(s) to .bib file path "
                        f"(blank to cancel, go back): "
                    ).strip()
                except EOFError:
                    out_input = ""
        except SuspendNotSupported:
            self.notify(
                "Exporting isn't supported in this terminal environment",
                severity="error",
            )
            return

        if not out_input:
            self._set_status("Export cancelled")
            return

        out_path = Path(out_input).expanduser()
        out_path.write_text(items_to_bibtex(self._filtered))
        self._set_status(f"Exported {len(self._filtered)} item(s) to {out_path}")

    def action_copy_bibtex(self) -> None:
        if not self._require_papers_mode():
            return
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]
        bibtex_text = items_to_bibtex([item])
        self.copy_to_clipboard(bibtex_text)
        short_title = _truncate(item.title, width=40)
        self._set_status(f"Copied BibTeX to clipboard: {short_title}")

    def action_edit_keywords(self) -> None:
        ensure_keywords_file()
        before = load_keywords()
        editor = os.environ.get("EDITOR", "nvim")
        try:
            with self.suspend():
                subprocess.call([editor, str(KEYWORDS_FILE)])
        except SuspendNotSupported:
            self.notify(
                "Editing isn't supported in this terminal environment",
                severity="error",
            )
            return

        after = load_keywords()
        removed_names = before - after
        added_names = after - before
        if len(removed_names) == 1 and len(added_names) == 1:
            old_name = next(iter(removed_names))
            new_name = next(iter(added_names))
            if self._confirm(
                f"Rename tag '{old_name}' -> '{new_name}' on every paper? [y/N] "
            ):
                changed = db_module.rename_tag(old_name, new_name)
                self._set_status(
                    f"Renamed tag '{old_name}' -> '{new_name}' on {changed} item(s)"
                )
                self._load_library()
                return

        removed = db_module.cleanup_tags(after)
        if removed:
            self._set_status(
                f"Keyword list updated · removed stale tags from {removed} item(s)"
            )
            self._load_library()
        else:
            self._set_status("Keyword list updated")

    def action_edit_projects(self) -> None:
        ensure_projects_file()
        before = load_projects()
        editor = os.environ.get("EDITOR", "nvim")
        try:
            with self.suspend():
                subprocess.call([editor, str(PROJECTS_FILE)])
        except SuspendNotSupported:
            self.notify(
                "Editing isn't supported in this terminal environment",
                severity="error",
            )
            return

        after = load_projects()
        removed_names = before - after
        added_names = after - before
        if len(removed_names) == 1 and len(added_names) == 1:
            old_name = next(iter(removed_names))
            new_name = next(iter(added_names))
            if self._confirm(
                f"Rename project '{old_name}' -> '{new_name}' on every paper? [y/N] "
            ):
                changed = db_module.rename_project(old_name, new_name)
                self._set_status(
                    f"Renamed project '{old_name}' -> '{new_name}' on {changed} item(s)"
                )
                self._load_library()
                return

        removed = db_module.cleanup_projects(after)
        if removed:
            self._set_status(
                f"Project list updated · removed {removed} project(s) no longer listed"
            )
            self._load_library()
        else:
            self._set_status("Project list updated")

    def action_focus_search(self) -> None:
        self.query_one("#search", Input).focus()

    def action_focus_list(self) -> None:
        self.query_one("#item-list", VimListView).focus()
        if self._active_filters:
            self._active_filters = []
            self._update_subtitle()
            self._apply_filter(self.query_one("#search", Input).value)


def main() -> None:
    ZoteroApp().run()
