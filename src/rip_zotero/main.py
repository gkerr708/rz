from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
import os
import subprocess
import sqlite3
import tempfile
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
from rip_zotero.bibtex import items_to_bibtex, parse_bibtex
from rip_zotero.keywords import (
    ensure_keywords_file,
    load_keywords,
    unknown_tags,
    KEYWORDS_FILE,
)
from rip_zotero.manuscripts import (
    ensure_manuscripts_file,
    load_manuscripts,
    MANUSCRIPTS_FILE,
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
        Binding("e", "edit_item", "Edit"),
        Binding("a", "add_item", "Add"),
        Binding("p", "add_pdf", "Attach PDF"),
        Binding("d", "delete_item", "Delete"),
        Binding("m", "toggle_manuscript", "Manuscript"),
        Binding("x", "export_bibtex", "Export"),
        Binding("K", "edit_keywords", "Keywords"),
        Binding("M", "edit_manuscripts", "Manuscripts"),
        Binding("f", "open_filter", "Filter"),
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

    def action_toggle_manuscript(self) -> None:
        self.app.action_toggle_manuscript()

    def action_export_bibtex(self) -> None:
        self.app.action_export_bibtex()

    def action_edit_keywords(self) -> None:
        self.app.action_edit_keywords()

    def action_edit_manuscripts(self) -> None:
        self.app.action_edit_manuscripts()

    def action_open_filter(self) -> None:
        self.app.action_open_filter()


class SearchInput(Input):
    BINDINGS = [
        Binding("shift+tab", "open_filter_from_search", "Filter", show=False),
    ]

    def action_open_filter_from_search(self) -> None:
        self.app.action_open_filter_from_search()


class FilterPicker(ModalScreen[list[tuple[str, str]] | None]):
    BINDINGS = [
        Binding("escape", "apply", "Apply", show=False),
        Binding("enter", "apply", "Apply", show=False, priority=True),
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
                "Tab: toggle · Shift+Tab: switch typing/j-k nav · Enter/Esc: apply",
                id="filter-hint",
            )

    def on_mount(self) -> None:
        for name in sorted(load_manuscripts()):
            self._entries.append((f"Manuscript: {name}", f"manuscript:{name}"))
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
        icon = "MS" if kind == "manuscript" else "KW"
        name_color = CAT["sapphire"] if kind == "manuscript" else CAT["mauve"]
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

    def action_apply(self) -> None:
        self.dismiss(sorted(self._selected))

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


HELP_TEXT = f"""\
[bold {CAT['yellow']}]Navigation[/bold {CAT['yellow']}]
  j / down        Move down
  k / up          Move up
  g               Jump to top
  G               Jump to bottom
  o               Show detail panel
  Escape          Focus list / cancel
  Ctrl+F          Focus search

[bold {CAT['yellow']}]Items[/bold {CAT['yellow']}]
  e               Edit item (opens $EDITOR)
  a               Add item from BibTeX
                  (point the PDF prompt at a directory to attach every
                  PDF inside)
  p               Attach an additional PDF to an item
                  (point it at a directory to attach every PDF inside)
  d               Delete item
  m               Add/remove item from a manuscript

[bold {CAT['yellow']}]Filtering & lists[/bold {CAT['yellow']}]
  Type a keyword/manuscript name in search, Enter  Apply as filter
  Shift+Tab (in search)  Open filter picker, pre-filtered by what
                  you typed — j/k to move, Tab to stack multiple,
                  Enter/Esc to apply
  f               Open filter picker from the list (all entries)
  K               Edit the keyword list
  M               Edit the manuscript list
  x               Export current view to BibTeX

[bold {CAT['yellow']}]Other[/bold {CAT['yellow']}]
  Ctrl+R          Refresh library from disk
  ?               This help screen
  q               Quit

Press any key to close.
"""


class HelpScreen(ModalScreen[None]):
    CSS = f"""
    HelpScreen {{
        align: center middle;
    }}
    #help-box {{
        width: 60;
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

    def _first_author_last(self) -> str:
        if not self.authors:
            return "Unknown"
        return self.authors.split(";")[0].strip().split(",")[0].strip()

    def list_markup(self) -> str:
        year = self.date[:4] if self.date else "n.d."
        author = self._first_author_last()
        if self.authors and ";" in self.authors:
            author += " et al."
        title_str = self.title[:80]
        if self.has_pdf:
            return (
                f"[{CAT['green']}]*[/{CAT['green']}] "
                f"[bold {CAT['blue']}]{escape(author)}[/bold {CAT['blue']}] "
                f"[{CAT['overlay0']}]({year})[/{CAT['overlay0']}] "
                f"[{CAT['text']}]{escape(title_str)}[/{CAT['text']}]"
            )
        return (
            f"[{CAT['overlay0']}]- {escape(author)} ({year}) "
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


def _build_detail(item: ZoteroItem) -> str:
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

    row("Date", item.date)
    row("Author", item.authors)
    journal = item.publication_title or item.book_title or item.proceedings_title
    row("Journal", journal)
    row("Type", item.type_name)
    row("Tags", item.tags, color=CAT["mauve"])

    manuscripts = db_module.manuscripts_for_item(item.item_id)
    row(
        "Manuscripts",
        "; ".join(manuscripts) if manuscripts else None,
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
        Binding("ctrl+f", "focus_search", "Search"),
        Binding("escape", "focus_list", "Browse", show=False),
        Binding("ctrl+r", "refresh", "Refresh"),
        Binding("question_mark", "help", "Help"),
        Binding("q", "quit", "Quit"),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._items: list[ZoteroItem] = []
        self._filtered: list[ZoteroItem] = []
        self._search_timer = None
        self._active_filters: list[tuple[str, str]] = []

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
        self._update_subtitle()
        query = self.query_one("#search", Input).value
        self._apply_filter(query)

    def _filter_label(self) -> str:
        if not self._active_filters:
            return ""
        parts = []
        for kind, name in self._active_filters:
            icon = "MS" if kind == "manuscript" else "KW"
            parts.append(f"{icon}:{name}")
        return " + ".join(parts) + " · "

    def _update_subtitle(self) -> None:
        n_pdf = sum(1 for i in self._items if i.has_pdf)
        self.sub_title = f"{self._filter_label()}{len(self._items)} items · {n_pdf} with PDF"

    def _scope_items(self) -> list[ZoteroItem]:
        items = self._items
        for kind, name in self._active_filters:
            if kind == "manuscript":
                ids = db_module.manuscript_item_ids(name)
                items = [it for it in items if int(it.item_id) in ids]
            elif kind == "keyword":
                items = [
                    it
                    for it in items
                    if name in [t.strip() for t in (it.tags or "").split(";")]
                ]
        return items

    def _handle_filter_result(self, result: list[tuple[str, str]] | None) -> None:
        if result is None:
            return
        self._active_filters = result
        self._update_subtitle()
        search = self.query_one("#search", Input)
        search.value = ""
        self._apply_filter("")

    def action_open_filter(self) -> None:
        self.push_screen(FilterPicker(list(self._active_filters)), self._handle_filter_result)

    def action_open_filter_from_search(self) -> None:
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

    def _set_status(self, msg: str) -> None:
        self.query_one("#status", Static).update(msg)

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id != "search":
            return
        if self._search_timer is not None:
            self._search_timer.stop()
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
            filtered = list(scope)
            n_pdf = sum(1 for i in scope if i.has_pdf)
            status = f"{label}{len(filtered)} items · {n_pdf} with PDF"
        self.call_from_thread(self._update_results, filtered, status)

    def _update_results(self, filtered: list[ZoteroItem], status: str) -> None:
        self._filtered = filtered
        self._set_status(status)
        self._rebuild_list()

    def _match_filter_by_name(self, text: str) -> tuple[str, str] | None:
        name = text.strip().lower()
        if not name:
            return None
        if name in load_manuscripts():
            return ("manuscript", name)
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
        if idx is not None and 0 <= idx < len(self._filtered):
            self._show_detail(self._filtered[idx])

    def on_option_list_option_selected(self, event: OptionList.OptionSelected) -> None:
        idx = event.option_index
        if idx is None or idx >= len(self._filtered):
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
        if idx is not None and 0 <= idx < len(self._filtered):
            self._show_detail(self._filtered[idx])

    def _show_detail(self, item: ZoteroItem) -> None:
        self.query_one("#detail-text", Static).update(_build_detail(item))
        self.query_one("#detail-panel", VerticalScroll).scroll_home(animate=False)

    def action_edit_item(self) -> None:
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

    def action_toggle_manuscript(self) -> None:
        lv = self.query_one("#item-list", VimListView)
        idx = lv.highlighted
        if idx is None or not (0 <= idx < len(self._filtered)):
            return
        item = self._filtered[idx]

        name = ""
        try:
            with self.suspend():
                try:
                    name = input(
                        f"Manuscript name for '{item.title}' (blank to cancel): "
                    ).strip()
                except EOFError:
                    name = ""
        except SuspendNotSupported:
            self.notify(
                "Manuscripts aren't supported in this terminal environment",
                severity="error",
            )
            return

        if not name:
            self._set_status("Cancelled")
            return

        name = name.lower()
        if name not in load_manuscripts():
            self.notify(
                f"Unknown manuscript '{name}' — add it via 'M' first",
                severity="error",
            )
            return

        added = db_module.toggle_manuscript_item(name, item.item_id)
        verb = "Added to" if added else "Removed from"
        self._set_status(f"{verb} manuscript '{name}': {item.title}")

    def action_export_bibtex(self) -> None:
        if not self._filtered:
            self.notify("Nothing to export in the current view", severity="warning")
            return

        out_input = ""
        try:
            with self.suspend():
                try:
                    out_input = input(
                        f"Export {len(self._filtered)} item(s) to .bib file path: "
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

    def action_edit_keywords(self) -> None:
        ensure_keywords_file()
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

        removed = db_module.cleanup_tags(load_keywords())
        if removed:
            self._set_status(
                f"Keyword list updated · removed stale tags from {removed} item(s)"
            )
            self._load_library()
        else:
            self._set_status("Keyword list updated")

    def action_edit_manuscripts(self) -> None:
        ensure_manuscripts_file()
        editor = os.environ.get("EDITOR", "nvim")
        try:
            with self.suspend():
                subprocess.call([editor, str(MANUSCRIPTS_FILE)])
        except SuspendNotSupported:
            self.notify(
                "Editing isn't supported in this terminal environment",
                severity="error",
            )
            return
        self._set_status("Manuscript list updated")

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
