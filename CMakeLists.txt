cmake_minimum_required(VERSION 3.20)
project(luna VERSION 1.1.0 LANGUAGES CXX)

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

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MEDIA REQUIRED IMPORTED_TARGET libmediainfo exiv2 poppler-qt6)

add_executable(luna
  src/main.cpp
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
  src/mainwindow.h src/mainwindow.cpp
  data/luna.qrc
)
target_link_libraries(luna PRIVATE Qt6::Widgets Qt6::Svg PkgConfig::MEDIA)

# Docked terminal (F4): optional, only with libvterm (Arch: libvterm).
pkg_check_modules(VTERM IMPORTED_TARGET vterm)
if(VTERM_FOUND)
  target_sources(luna PRIVATE src/terminal.h src/terminal.cpp)
  target_compile_definitions(luna PRIVATE LUNA_TERMINAL)
  target_link_libraries(luna PRIVATE PkgConfig::VTERM util)
else()
  message(STATUS "libvterm not found: building without the docked terminal")
endif()

install(TARGETS luna RUNTIME DESTINATION bin)
install(FILES data/luna.desktop DESTINATION share/applications)
install(FILES data/luna.svg DESTINATION share/icons/hicolor/scalable/apps)
