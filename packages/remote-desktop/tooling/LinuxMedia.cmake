# Explicit Linux media dependency scope; shared by the provider and load fixture.
foreach(component IN ITEMS avcodec avutil swscale)
    if(component STREQUAL "avcodec")
        set(version "61.19.101")
    elseif(component STREQUAL "avutil")
        set(version "59.39.100")
    else()
        set(version "8.3.100")
    endif()
    add_library(desktop-${component} SHARED IMPORTED)
    set_target_properties(desktop-${component} PROPERTIES
        IMPORTED_LOCATION "${DOCKPIPE_DESKTOP_QT_PREFIX}/lib/lib${component}.so.${version}")
endforeach()
set(DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX "" CACHE PATH "Explicit hash-verified host media libraries")
if(NOT IS_ABSOLUTE "${DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX}")
    message(FATAL_ERROR "Linux requires an explicit qualified host-library prefix")
endif()
find_library(DESKTOP_CRYPTO NAMES libcrypto.so.3 crypto
    PATHS "${DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX}" NO_DEFAULT_PATH REQUIRED)
find_library(DESKTOP_VA NAMES libva.so.2 va
    PATHS "${DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX}" NO_DEFAULT_PATH REQUIRED)
find_library(DESKTOP_VA_DRM NAMES libva-drm.so.2 va-drm
    PATHS "${DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX}" NO_DEFAULT_PATH REQUIRED)
find_library(DESKTOP_VA_X11 NAMES libva-x11.so.2 va-x11
    PATHS "${DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX}" NO_DEFAULT_PATH REQUIRED)
add_library(desktop-linux-media INTERFACE)
target_include_directories(desktop-linux-media INTERFACE "${DOCKPIPE_DESKTOP_DEPENDENCIES}/ffmpeg")
# Qt's FFmpeg SONAME stubs do not provide the real Crypto/VAAPI symbols.
# Keep only these explicit backends in the dependency scope; no global preload.
target_link_libraries(desktop-linux-media INTERFACE desktop-avcodec desktop-avutil desktop-swscale
    "-Wl,--push-state,--no-as-needed" "${DESKTOP_CRYPTO}" "${DESKTOP_VA}"
    "${DESKTOP_VA_DRM}" "${DESKTOP_VA_X11}" "-Wl,--pop-state")

# Native physical keyboard mapping uses the same qualified host X11 library.
find_library(DESKTOP_X11 NAMES libX11.so.6 X11
    PATHS "${DOCKPIPE_DESKTOP_HOST_LIBRARY_PREFIX}" NO_DEFAULT_PATH REQUIRED)
set(X11_X11_LIB "${DESKTOP_X11}")
find_package(X11 REQUIRED)
