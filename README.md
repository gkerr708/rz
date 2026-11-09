### Project Goal
* The main project goal is to replace the front end of Zotero with a terminal user interface.
* Zotero uses sqlite to store metadata for each paper.
* The main thing I need to do is build a tool which allows me to search through the database.

## C++ version (`rz`)

The app is written in C++ with [FTXUI](https://github.com/ArthurSonzogni/FTXUI), the same way as crate.
It looks and works the same as the earlier Python/Textual version, and uses the same data in
`~/.local/share/rip-zotero` (library.sqlite, storage/, keywords.txt, projects.txt).

### Dependencies (Arch)

```bash
# needed to build
sudo pacman -S --needed base-devel cmake git pkgconf sqlite

# needed at runtime
sudo pacman -S --needed zathura xdg-utils wl-clipboard zip
```

- `zathura` opens PDFs, `xdg-open` opens author websites.
- `wl-copy` (wl-clipboard) is used for the clipboard on Wayland; elsewhere the terminal's clipboard (OSC 52) is used.
- `zip` makes the .zip when sharing papers (`S`).

FTXUI and toml++ aren't installed with pacman. CMake downloads them the first time you configure, which needs an internet connection.

### Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release   # configure: creates build/ and fetches FTXUI + toml++
cmake --build build -j                      # compile: produces build/rz
```

After changes, only the second command is needed. To start fresh, `rm -rf build` and run both again.

### Putting it on your PATH

```bash
ln -s ~/code/rip-zotero/build/rz ~/.local/bin/rz
```

### Usage

```bash
rz            # browse the library (press ? for keys)
rz migrate    # import ~/Zotero (zotero.sqlite + storage/) into the library
```
