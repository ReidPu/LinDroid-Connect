cmake_minimum_required(VERSION 3.20)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(GTK3 QUIET gtk+-3.0)
    pkg_check_modules(APPINDICATOR QUIET ayatana-appindicator3-0.1 appindicator3-0.1)
endif()

# LINUX-15: Dynamic multiarch & cross-distro library search paths (Debian/Ubuntu, Fedora, Arch, Alpine, etc.)
set(SYSTEM_LIB_SEARCH_PATHS
    "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
    "/usr/lib/x86_64-linux-gnu"
    "/usr/lib/aarch64-linux-gnu"
    "/usr/lib64"
    "/usr/lib"
    "/usr/local/lib"
    "/lib64"
    "/lib"
)

find_library(LIB_GTK3 NAMES libgtk-3.so.0 gtk-3 PATHS ${SYSTEM_LIB_SEARCH_PATHS})
find_library(LIB_GDK3 NAMES libgdk-3.so.0 gdk-3 PATHS ${SYSTEM_LIB_SEARCH_PATHS})
find_library(LIB_GDK_PIXBUF NAMES libgdk_pixbuf-2.0.so.0 gdk_pixbuf-2.0 PATHS ${SYSTEM_LIB_SEARCH_PATHS})
find_library(LIB_X11 NAMES libX11.so.6 libX11.so X11 PATHS ${SYSTEM_LIB_SEARCH_PATHS})
find_library(LIB_GOBJECT NAMES libgobject-2.0.so.0 gobject-2.0 PATHS ${SYSTEM_LIB_SEARCH_PATHS})
find_library(LIB_GLIB NAMES libglib-2.0.so.0 glib-2.0 PATHS ${SYSTEM_LIB_SEARCH_PATHS})
find_library(LIB_APPINDICATOR NAMES libayatana-appindicator3.so.1 ayatana-appindicator3 appindicator3 PATHS ${SYSTEM_LIB_SEARCH_PATHS})

add_executable(l2a-desktop
    src/main.cpp
    src/wayland_clipboard.cpp
    src/settings_manager.cpp
    src/adb_helper.cpp
    src/nfc_ntag5332.cpp
)

target_include_directories(l2a-desktop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(l2a-desktop PRIVATE
    -Wall -Wextra -Wpedantic
)

target_link_libraries(l2a-desktop PRIVATE
    l2a_core
    ${LIB_GTK3}
    ${LIB_GDK3}
    ${LIB_GDK_PIXBUF}
    ${LIB_X11}
    ${LIB_GOBJECT}
    ${LIB_GLIB}
    ${LIB_APPINDICATOR}
    ${CMAKE_DL_LIBS}
)

if(L2A_BUILD_TESTS)
    add_executable(test_linux_subsystem
        tests/test_linux_subsystem.cpp
        src/wayland_clipboard.cpp
        src/settings_manager.cpp
        src/adb_helper.cpp
        src/nfc_ntag5332.cpp
    )

    target_include_directories(test_linux_subsystem PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_options(test_linux_subsystem PRIVATE
        -Wall -Wextra -Wpedantic
    )

    target_link_libraries(test_linux_subsystem PRIVATE
        l2a_core
        ${LIB_GTK3}
        ${LIB_GDK3}
        ${LIB_GDK_PIXBUF}
        ${LIB_X11}
        ${LIB_GOBJECT}
        ${LIB_GLIB}
        ${LIB_APPINDICATOR}
        ${CMAKE_DL_LIBS}
    )

    add_test(NAME LinuxSubsystemTest COMMAND test_linux_subsystem)
endif()
