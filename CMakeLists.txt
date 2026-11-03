cmake_minimum_required(VERSION 3.20)
project(luna VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Local builds are tuned for the machine they run on; release zips must run on
# any x86-64 PC from the last ~15 years (x86-64-v2: SSE4.2, POPCNT).
option(LUNA_NATIVE "Optimise for this machine's CPU" ON)
if(LUNA_NATIVE)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native")
else()
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=x86-64-v2 -mtune=generic")
endif()

# Development only:
#   LUNA_TESTS       unit tests (ctest), see tests/
#   LUNA_TEST_HOOKS  the LUNA_* screenshot/benchmark hooks (src/testhooks.cpp);
#                    some act on real files or keyrings, so never in a release
#   LUNA_SANITIZE    AddressSanitizer + UndefinedBehaviorSanitizer
option(LUNA_TESTS "Build the unit tests" OFF)
option(LUNA_TEST_HOOKS "Build the LUNA_* development hooks into the executable" OFF)
option(LUNA_SANITIZE "Build with ASan and UBSan" OFF)
if(LUNA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MEDIA REQUIRED IMPORTED_TARGET libmediainfo exiv2 poppler-qt6)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)

# Everything but main(): the executable and the tests link the same code.
add_library(lunacore STATIC
  src/theme.h src/theme.cpp
  src/settings.h src/settings.cpp
  src/fsmodel.h src/fsmodel.cpp
  src/icons.h src/icons.cpp
  src/thumbs.h src/thumbs.cpp
  src/apps.h src/apps.cpp
  src/fileops.h src/fileops.cpp
  src/undo.h src/undo.cpp
  src/places.h src/places.cpp
  src/pathbar.h src/pathbar.cpp
  src/remote.h src/remote.cpp
  src/sidebar.h src/sidebar.cpp
  src/pane.h src/pane.cpp
  src/properties.h src/properties.cpp
  src/settingsdialog.h src/settingsdialog.cpp
  src/smoothscroll.h src/smoothscroll.cpp
  src/preview.h src/preview.cpp
  src/jump.h src/jump.cpp
  src/discovery.h src/discovery.cpp
  src/mainwindow.h src/mainwindow.cpp
  src/i18n.h src/i18n.cpp
  src/archives.h src/archives.cpp
  src/office.h src/office.cpp
  src/crypto.h src/crypto.cpp
  src/gitmenu.h src/gitmenu.cpp
  src/clip.h src/clip.cpp
)
target_include_directories(lunacore PUBLIC src)
target_link_libraries(lunacore PUBLIC Qt6::Widgets Qt6::Svg PkgConfig::MEDIA PkgConfig::LIBARCHIVE)

# Docked terminal (F4): optional, only with libvterm (Arch: libvterm).
option(LUNA_WITH_TERMINAL "Docked terminal (needs libvterm)" ON)
if(LUNA_WITH_TERMINAL)
  pkg_check_modules(VTERM IMPORTED_TARGET vterm)
endif()
if(VTERM_FOUND)
  target_sources(lunacore PRIVATE src/terminal.h src/terminal.cpp)
  target_compile_definitions(lunacore PUBLIC LUNA_TERMINAL)
  target_link_libraries(lunacore PUBLIC PkgConfig::VTERM util)
else()
  message(STATUS "libvterm not found: building without the docked terminal")
endif()

# 30 s of a song in the preview: optional, with FFmpeg and PipeWire.
option(LUNA_WITH_AUDIO "Play a fragment of songs in the preview (needs FFmpeg and PipeWire)" ON)
if(LUNA_WITH_AUDIO)
  pkg_check_modules(AUDIO IMPORTED_TARGET libavformat libavcodec libavutil libswresample libpipewire-0.3)
endif()
if(AUDIO_FOUND)
  target_compile_definitions(lunacore PUBLIC LUNA_AUDIO)
  target_link_libraries(lunacore PUBLIC PkgConfig::AUDIO)
else()
  message(STATUS "FFmpeg/PipeWire not found: building without song fragments in the preview")
endif()

add_executable(luna src/main.cpp data/luna.qrc)
target_link_libraries(luna PRIVATE lunacore)
if(LUNA_TEST_HOOKS)
  target_sources(luna PRIVATE src/testhooks.h src/testhooks.cpp)
  target_compile_definitions(luna PRIVATE LUNA_TEST_HOOKS)
  target_compile_definitions(lunacore PUBLIC LUNA_TEST_HOOKS)   # LUNA_SEARCHTEST timing, LUNA_AUDIO_TARGET
endif()

if(LUNA_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS luna RUNTIME DESTINATION bin)
install(FILES data/luna.desktop DESTINATION share/applications)
install(FILES data/luna.svg DESTINATION share/icons/hicolor/scalable/apps)
