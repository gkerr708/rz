from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
import subprocess
import sqlite3
import pandas as pd

from rich.markup import escape
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, VerticalScroll
from textual.widgets import Footer, Header, Input, Label, ListItem, ListView, Static


class VimListView(ListView):
    BINDINGS = [
        Binding("j", "cursor_down", "Down", show=False),
        Binding("k", "cursor_up", "Up", show=False),
        Binding("g", "scroll_top", "Top", show=False),
        Binding("G", "scroll_bottom", "Bottom", show=False),
        Binding("o", "show_detail", "Detail", show=False),
    ]

    def action_scroll_top(self) -> None:
        self.index = 0

    def action_scroll_bottom(self) -> None:
        if self._nodes:
            self.index = len(self._nodes) - 1
from textual import work


ZOTERO_DB = Path("/home/gkerr/Zotero/zotero.sqlite")
ZOTERO_STORAGE = Path("/home/gkerr/Zotero/storage")


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

    @property
    def has_pdf(self) -> bool:
        return self.pdf_path is not None and self.pdf_path.exists()

    def open_pdf(self) -> None:
        if self.has_pdf:
            subprocess.Popen(
                ["zathura", str(self.pdf_path)],
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
                f"[green]●[/green] "
                f"[bold cyan]{escape(author)}[/bold cyan] "
                f"[dim]({year})[/dim] "
                f"{escape(title_str)}"
            )
        return f"[dim]○ {escape(author)} ({year}) {escape(title_str)}[/dim]"

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
        WHERE i.itemTypeID != 14
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
        WHERE i.itemTypeID != 14
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
        WHERE i.itemTypeID != 14
        GROUP BY i.itemID
        """,
        con,
    ).set_index("itemID")

    types = pd.read_sql(
        """
        SELECT i.itemID, it.typeName
        FROM items i
        JOIN itemTypes it ON i.itemTypeID = it.itemTypeID
        WHERE i.itemTypeID != 14
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
    attachments = attachments.set_index("itemID")[["resolved_path"]]

    con.close()

    df = wide.join([authors, tags, types, attachments], how="left")
    df = df[df["title"].notna()]
    if with_pdf_only:
        df = df[df["resolved_path"].notna()]

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
        )
        for item_id, row in df.iterrows()
    ]


def _build_detail(item: ZoteroItem) -> str:
    parts: list[str] = []
    parts.append(f"[bold yellow]{escape(item.title)}[/bold yellow]")
    parts.append("")

    def row(label: str, value: str | None) -> None:
        if value:
            parts.append(f"[dim]{label}:[/dim]  {escape(value)}")

    row("Authors", item.authors)
    row("Date", item.date)
    row("Type", item.type_name)

    pub = item.publication_title or item.book_title or item.proceedings_title
    row("Publication", pub)
    row("Journal Abbrev", item.journal_abbrev)

    vol_parts = []
    if item.volume:
        vol_parts.append(f"Vol. {item.volume}")
    if item.issue:
        vol_parts.append(f"No. {item.issue}")
    if item.pages:
        vol_parts.append(f"pp. {item.pages}")
    row("Volume / Issue / Pages", "  ".join(vol_parts) or None)

    row("Publisher", item.publisher)
    row("Place", item.place)
    row("Edition", item.edition)
    row("Series", item.series)
    row("Conference", item.conference_name)
    row("DOI", item.doi)
    row("ISBN", item.isbn)
    row("ISSN", item.issn)
    row("PMID", item.pmid)
    row("PMCID", item.pmcid)
    row("URL", item.url)
    row("Language", item.language)
    row("Library Catalog", item.library_catalog)
    row("Tags", item.tags)
    row("Rights", item.rights)
    row("Extra", item.extra)

    if item.abstract:
        parts.append("")
        parts.append("[dim]Abstract:[/dim]")
        parts.append(escape(item.abstract))

    parts.append("")
    if item.has_pdf:
        parts.append(f"[green]PDF:[/green]  {escape(str(item.pdf_path))}")
    elif item.pdf_path:
        parts.append(f"[red]PDF (missing):[/red]  {escape(str(item.pdf_path))}")
    else:
        parts.append("[dim]No PDF attached[/dim]")

    return "\n".join(parts)


class ZoteroApp(App):
    TITLE = "Zotero TUI"
    SUB_TITLE = "Loading..."

    CSS = """
    #search {
        margin: 1 1 0 1;
    }

    #main {
        height: 1fr;
        margin: 0 1 0 1;
    }

    #item-list {
        width: 55%;
        border: tall $primary;
    }

    #detail-panel {
        width: 1fr;
        border: tall $primary;
    }

    #detail-text {
        height: auto;
        padding: 1 2;
    }

    #status {
        height: 1;
        background: $surface-darken-2;
        color: $text-muted;
        padding: 0 2;
    }
    """

    BINDINGS = [
        Binding("ctrl+f", "focus_search", "Search"),
        Binding("escape", "focus_list", "Browse", show=False),
        Binding("q", "quit", "Quit"),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._items: list[ZoteroItem] = []
        self._filtered: list[ZoteroItem] = []
        self._search_timer = None

    def compose(self) -> ComposeResult:
        yield Header()
        yield Input(placeholder="Search title, author, tag, abstract...", id="search")
        with Horizontal(id="main"):
            yield VimListView(id="item-list")
            with VerticalScroll(id="detail-panel"):
                yield Static("", id="detail-text")
        yield Static("Loading library...", id="status")
        yield Footer()

    def on_mount(self) -> None:
        self._load_library()
        self.query_one("#search", Input).focus()

    @work(thread=True)
    def _load_library(self) -> None:
        items = load_library()
        self.call_from_thread(self._on_loaded, items)

    def _on_loaded(self, items: list[ZoteroItem]) -> None:
        self._items = items
        self._filtered = list(items)
        self._rebuild_list()
        n_pdf = sum(1 for i in items if i.has_pdf)
        self.sub_title = f"{len(items)} items · {n_pdf} with PDF"
        self._set_status(f"{len(items)} items · {n_pdf} with PDF")

    def _rebuild_list(self) -> None:
        lv = self.query_one("#item-list", VimListView)
        lv.clear()
        for item in self._filtered:
            lv.append(ListItem(Label(item.list_markup())))

    def _set_status(self, msg: str) -> None:
        self.query_one("#status", Static).update(msg)

    def on_input_changed(self, event: Input.Changed) -> None:
        if event.input.id != "search":
            return
        if self._search_timer is not None:
            self._search_timer.stop()
        self._search_timer = self.set_timer(0.3, lambda: self._apply_filter(event.value))

    def _apply_filter(self, query: str) -> None:
        q = query.strip().lower()
        if q:
            words = q.split()
            self._filtered = [
                it for it in self._items if all(w in it.search_text() for w in words)
            ]
            self._set_status(f"{len(self._filtered)} of {len(self._items)} match")
        else:
            self._filtered = list(self._items)
            n_pdf = sum(1 for i in self._items if i.has_pdf)
            self._set_status(f"{len(self._items)} items · {n_pdf} with PDF")
        self._rebuild_list()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id == "search":
            self.query_one("#item-list", VimListView).focus()

    def on_list_view_highlighted(self, event: ListView.Highlighted) -> None:
        lv = self.query_one("#item-list", VimListView)
        idx = lv.index
        if idx is not None and 0 <= idx < len(self._filtered):
            self._show_detail(self._filtered[idx])

    def on_list_view_selected(self, event: ListView.Selected) -> None:
        lv = self.query_one("#item-list", VimListView)
        idx = lv.index
        if idx is None or idx >= len(self._filtered):
            return
        item = self._filtered[idx]
        if item.has_pdf:
            item.open_pdf()
            self._set_status(f"Opened: {item.pdf_path.name}")
        else:
            self.notify("No PDF attached for this item", severity="warning")

    def action_show_detail(self) -> None:
        lv = self.query_one("#item-list", VimListView)
        idx = lv.index
        if idx is not None and 0 <= idx < len(self._filtered):
            self._show_detail(self._filtered[idx])

    def _show_detail(self, item: ZoteroItem) -> None:
        self.query_one("#detail-text", Static).update(_build_detail(item))
        self.query_one("#detail-panel", VerticalScroll).scroll_home(animate=False)

    def action_focus_search(self) -> None:
        self.query_one("#search", Input).focus()

    def action_focus_list(self) -> None:
        self.query_one("#item-list", VimListView).focus()


def main() -> None:
    ZoteroApp().run()
