cmake_minimum_required(VERSION 3.21.1)
project(DockpipeGuestTools VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Guest Tools must be built and qualified on native Windows")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS Widgets Network Test)

add_library(guest-tools-status STATIC
    src/ProvisioningStatus.cpp src/ProvisioningStatus.h
    src/ClipboardRequest.cpp src/ClipboardRequest.h)
target_link_libraries(guest-tools-status PUBLIC Qt6::Core)
target_include_directories(guest-tools-status PUBLIC src)

qt_add_executable(dockpipe-guest-tools WIN32
    src/main.cpp
    src/GuestToolsWindow.cpp
    src/GuestToolsWindow.h
    src/NativePlatform.cpp
    src/NativePlatform.h
    src/ApplicationOptions.h
    src/ClipboardServer.cpp
    src/ClipboardServer.h
    src/NativeClipboard.cpp
    src/NativeClipboard.h
    src/NativeDisplay.cpp
    src/NativeDisplay.h
    assets/application.rc)
target_include_directories(dockpipe-guest-tools PRIVATE assets)
target_compile_definitions(dockpipe-guest-tools PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dockpipe-guest-tools PRIVATE guest-tools-status Qt6::Widgets Qt6::Network shell32 advapi32)

include(CTest)
if(BUILD_TESTING)
    add_executable(guest-tools-status-test tests/ProvisioningStatusTest.cpp)
    target_link_libraries(guest-tools-status-test PRIVATE guest-tools-status Qt6::Test)
    add_test(NAME guest-tools-status COMMAND guest-tools-status-test)
    add_executable(guest-tools-clipboard-test tests/ClipboardRequestTest.cpp)
    target_link_libraries(guest-tools-clipboard-test PRIVATE guest-tools-status Qt6::Test)
    add_test(NAME guest-tools-clipboard COMMAND guest-tools-clipboard-test)
endif()
