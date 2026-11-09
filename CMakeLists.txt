cmake_minimum_required(VERSION 3.20)
project(rip-zotero LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- FTXUI (the TUI library) and toml++ (reads the edit files): downloaded
# the first time you configure ---
include(FetchContent)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v6.1.9
    GIT_SHALLOW TRUE
)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui tomlplusplus)

# --- SQLite (the library database): installed with pacman ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)

add_executable(rz
    src/main.cpp
    src/bibtex.cpp
    src/db.cpp
    src/edit.cpp
    src/lists.cpp
    src/rolodex.cpp
    src/text.cpp
    src/util.cpp
)
target_compile_options(rz PRIVATE -Wall -Wextra)
# One-time seed data for new author profiles, read from the source tree.
target_compile_definitions(rz PRIVATE RZ_ROLODEX_SEED="${CMAKE_SOURCE_DIR}/src/rolodex.toml")
target_link_libraries(rz PRIVATE
    ftxui::screen ftxui::dom ftxui::component
    tomlplusplus::tomlplusplus
    PkgConfig::SQLITE
)
