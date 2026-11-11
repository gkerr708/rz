# rz (rip zotero)

A fast, keyboard-driven terminal app for your Zotero library.

Zotero keeps everything (papers, authors, tags, collections) in a SQLite database. `rz` imports
that library and gives you a terminal UI to search, filter, tag and open your papers, without
running the Zotero desktop app.

## Features

- **Instant search** across your whole library, with vim-style navigation (`j`/`k`, `g`/`G`, `/`)
- **Tags and projects**: tag papers and group them into projects, then stack filters to narrow things down
- **Authors view**: browse by author, see each author's co-authors, and keep notes on research groups
- **Reading queue**: star the papers you want to read next
- **BibTeX**: add papers from BibTeX, copy one entry, or export a whole filtered view to a `.bib` file
- **Sharing**: collect papers from several searches and export them as neatly renamed PDFs plus a
  `references.bib`, optionally zipped for email
- **Open PDFs** in zathura with a single key

## Installation

These steps are for Arch Linux, but any distro with the same packages will work.

### 1. Install dependencies

```bash
# to build
sudo pacman -S --needed base-devel cmake git pkgconf sqlite

# to run
sudo pacman -S --needed zathura xdg-utils wl-clipboard zip
```

| Package        | Used for                                                              |
| -------------- | --------------------------------------------------------------------- |
| `zathura`      | Opening PDFs                                                          |
| `xdg-utils`    | Opening author websites in your browser                               |
| `wl-clipboard` | Clipboard on Wayland (other setups use the terminal's OSC 52 clipboard) |
| `zip`          | Zipping shared papers                                                 |

You don't need to install [FTXUI](https://github.com/ArthurSonzogni/FTXUI) or
[toml++](https://github.com/marzer/tomlplusplus) yourself. CMake downloads them the first time you build,
so you'll need an internet connection for that step.

### 2. Build

```bash
git clone https://github.com/gkerr708/rz.git
cd rz
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/rz`. After pulling changes, rebuild with just `cmake --build build -j`.
If a build ever gets into a bad state, `rm -rf build` and run both commands again.

### 3. Put it on your PATH

```bash
ln -s "$PWD/build/rz" ~/.local/bin/rz
```

## Getting started

Import your existing Zotero library (from `~/Zotero`), then launch the app:

```bash
rz migrate    # one-time import of ~/Zotero (zotero.sqlite + storage/)
rz            # open your library
```

Press **`?`** inside the app to see every shortcut. Here are the essentials:

| Key        | Action                                  |
| ---------- | --------------------------------------- |
| `j` / `k`  | Move down / up                          |
| `/`        | Search                                  |
| `Enter`    | Open the paper's PDF                    |
| `o`        | Show the detail panel                   |
| `a`        | Add a paper from BibTeX                 |
| `e`        | Edit a paper in `$EDITOR`               |
| `t` / `m`  | Tag a paper / add it to a project       |
| `f`        | Filter by tag or project                |
| `r` / `R`  | Add to reading queue / show the queue   |
| `Space`    | Mark papers, then `S` to share them     |
| `x`        | Copy BibTeX to the clipboard            |
| `Shift+A`  | Switch between papers and authors views |
| `q`        | Quit                                    |

## Where your data lives

`rz` never changes your original Zotero library. `rz migrate` copies it into its own folder:

```
~/.local/share/rip-zotero/
├── library.sqlite   # papers, authors, tags, projects
├── storage/         # PDFs
├── keywords.txt     # your tag list
└── projects.txt     # your project list
```

To back up your library or move it to another machine, copy that folder.
