# This profile is compiled in the pinned KDE SDK and resolves media from the
# matching Platform. Native Qt FFmpeg stubs and host libraries are not bundled.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    OR NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "15.2.0")
    message(FATAL_ERROR "The KDE 6.10 viewer requires GNU 15.2.0")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(DESKTOP_FLATPAK_MEDIA REQUIRED IMPORTED_TARGET
    libavcodec=61.19.101 libavutil=59.39.100 libswscale=8.3.100)
add_library(desktop-linux-media INTERFACE)
target_link_libraries(desktop-linux-media INTERFACE PkgConfig::DESKTOP_FLATPAK_MEDIA)
find_package(X11 REQUIRED)
