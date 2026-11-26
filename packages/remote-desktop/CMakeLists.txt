cmake_minimum_required(VERSION 3.22)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum supported sender OS")
endif()
project(DockpipeNativeDesktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(DOCKPIPE_DESKTOP_DEPENDENCIES "" CACHE PATH "Verified package dependency directory")
set(DOCKPIPE_DESKTOP_RUNTIME_PROFILE "native" CACHE STRING "Qualified dependency profile")
set_property(CACHE DOCKPIPE_DESKTOP_RUNTIME_PROFILE PROPERTY STRINGS native flatpak-kde-6.10)
if(NOT DOCKPIPE_DESKTOP_RUNTIME_PROFILE STREQUAL "native"
    AND NOT DOCKPIPE_DESKTOP_RUNTIME_PROFILE STREQUAL "flatpak-kde-6.10")
    message(FATAL_ERROR "Unknown native desktop dependency profile")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DOCKPIPE_DESKTOP_RUNTIME_PROFILE STREQUAL "native")
    message(FATAL_ERROR "The Flatpak viewer profile requires Linux")
endif()
if(NOT IS_ABSOLUTE "${DOCKPIPE_DESKTOP_DEPENDENCIES}")
    message(FATAL_ERROR "Use tooling/build.py with explicit verified dependency artifacts")
endif()
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    message(FATAL_ERROR "Only Linux amd64 viewer and Darwin arm64 sender are supported")
endif()

include(CTest)
add_executable(native-desktop-state-test native/StateTest.cpp native/InputState.cpp)
target_include_directories(native-desktop-state-test PRIVATE native)
target_compile_options(native-desktop-state-test PRIVATE -UNDEBUG)
add_test(NAME native-desktop-state COMMAND native-desktop-state-test)
set_tests_properties(native-desktop-state PROPERTIES TIMEOUT 15)
add_executable(native-desktop-h264-contract native/H264ContractTest.cpp)
target_include_directories(native-desktop-h264-contract PRIVATE native)
target_compile_options(native-desktop-h264-contract PRIVATE -UNDEBUG)
add_test(NAME native-desktop-h264-contract COMMAND native-desktop-h264-contract)
set_tests_properties(native-desktop-h264-contract PROPERTIES TIMEOUT 15)


add_executable(native-desktop-virtual-mode native/NativeVirtualModeTest.cpp)
target_include_directories(native-desktop-virtual-mode PRIVATE native)
target_compile_options(native-desktop-virtual-mode PRIVATE -UNDEBUG)
add_test(NAME native-desktop-virtual-mode COMMAND native-desktop-virtual-mode)
set_tests_properties(native-desktop-virtual-mode PROPERTIES TIMEOUT 15)

find_package(Threads REQUIRED)
function(native_worker_test target source)
    add_executable(${target} "native/worker/${source}" ${ARGN})
    target_include_directories(${target} PRIVATE native/worker native source
        "${DOCKPIPE_DESKTOP_DEPENDENCIES}/json/single_include"
        "${DOCKPIPE_DESKTOP_DEPENDENCIES}/worker-sdk/include")
    target_compile_options(${target} PRIVATE -UNDEBUG)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 15)
endfunction()
native_worker_test(native-desktop-worker-protocol WorkerProtocolTest.cpp)
native_worker_test(native-desktop-worker-capture-profile WorkerCaptureProfileTest.cpp)
native_worker_test(native-desktop-worker-arguments WorkerArgumentsTest.cpp)
native_worker_test(native-desktop-worker-cleanup WorkerCleanupTest.cpp)
native_worker_test(native-desktop-worker-reports WorkerReportsTest.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-worker-channel WorkerChannelTest.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-clipboard-protocol ClipboardProtocolTest.cpp)
native_worker_test(native-desktop-worker-clipboard WorkerClipboardTest.cpp)
native_worker_test(native-desktop-worker-diagnostics WorkerDiagnosticsTest.cpp)
native_worker_test(native-desktop-worker-readiness WorkerReadinessTest.cpp)
native_worker_test(native-desktop-worker-package-contract WorkerPackageContractTest.cpp native/NativeVirtualDisplayOwnerProtocol.cpp native/NativeVirtualDisplayObserverProtocol.cpp)
native_worker_test(native-desktop-worker-host-template WorkerHostTemplateTest.cpp)
native_worker_test(native-desktop-worker-child-process-deadline ChildProcessDeadlineTest.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-virtual-snapshot ../NativeVirtualDisplaySnapshotTest.cpp native/NativeVirtualDisplaySnapshot.cpp)
native_worker_test(native-desktop-virtual-owner-protocol ../NativeVirtualDisplayOwnerProtocolTest.cpp native/NativeVirtualDisplayOwnerProtocol.cpp)
native_worker_test(native-desktop-virtual-observer ../NativeVirtualDisplayObserverTest.cpp native/NativeVirtualDisplayObserverClient.cpp native/NativeVirtualDisplayObserverProtocol.cpp native/NativeVirtualDisplayOwnerProtocol.cpp native/NativeVirtualDisplaySnapshot.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-virtual-owner-client ../NativeVirtualDisplayOwnerClientTest.cpp native/NativeVirtualDisplayOwnerClient.cpp native/NativeVirtualDisplayOwnerProtocol.cpp native/NativeVirtualDisplayObserverClient.cpp native/NativeVirtualDisplayObserverProtocol.cpp native/NativeVirtualDisplaySnapshot.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-worker-power-policy PowerPolicyTest.cpp)
add_executable(native-desktop-power-assertion native/NativePowerAssertionTest.cpp native/NativePowerAssertion.cpp)
target_include_directories(native-desktop-power-assertion PRIVATE native)
target_compile_options(native-desktop-power-assertion PRIVATE -UNDEBUG)
target_link_libraries(native-desktop-power-assertion PRIVATE Threads::Threads)
add_test(NAME native-desktop-power-assertion COMMAND native-desktop-power-assertion)
set_tests_properties(native-desktop-power-assertion PROPERTIES TIMEOUT 15)
native_worker_test(native-desktop-worker-host-pipe HostPipeTest.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-worker-process ProcessIOTest.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-worker-media MediaWriterTest.cpp native/worker/ChildProcess.cpp)
native_worker_test(native-desktop-asset-protocol ../AssetClipboardProtocolTest.cpp)
native_worker_test(native-desktop-asset-store AssetStoreTest.cpp native/worker/AssetStore.cpp)
native_worker_test(native-desktop-asset-transfer ../AssetClipboardTransferTest.cpp
    native/AssetClipboardTransfer.cpp native/worker/AssetStore.cpp)
native_worker_test(native-desktop-clipboard-png ../ClipboardPngTest.cpp)
native_worker_test(native-desktop-worker-assets NativeAssetsTest.cpp native/worker/NativeAssets.cpp
    native/AssetClipboardTransfer.cpp native/worker/AssetStore.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR "The initial viewer requires Linux amd64")
    endif()
    set(CMAKE_AUTOMOC ON)
    set(desktop_test_platform "$ENV{QT_QPA_PLATFORM}")
    if(desktop_test_platform STREQUAL "")
        set(desktop_test_platform "offscreen")
    endif()
    find_package(Qt6 6.10.3 EXACT REQUIRED COMPONENTS Widgets Network Multimedia Test OpenGLWidgets)
    find_package(DockPipeDisplaySDK 1.0.0 EXACT REQUIRED
        PATHS "${DOCKPIPE_DESKTOP_DEPENDENCIES}/sdk/lib/cmake/DockPipeDisplaySDK" NO_DEFAULT_PATH)
    set(DOCKPIPE_DESKTOP_QT_PREFIX "" CACHE PATH "Exact verified Qt 6.10.3 gcc_64 kit")
    if(DOCKPIPE_DESKTOP_RUNTIME_PROFILE STREQUAL "flatpak-kde-6.10")
        include(tooling/FlatpakMedia.cmake)
    else()
        include(tooling/LinuxMedia.cmake)
    endif()
    add_library(dockpipe-native-desktop-display MODULE
        source/DesktopDisplayPlugin.cpp
        source/DesktopViewerSurface.cpp source/ClipboardPasteIntent.cpp source/GpuVideoWidget.cpp
        source/H264Decoder.cpp source/HardwareVideoDecoder.cpp
        source/InputMapping.cpp
        source/ProcessChannel.cpp source/ClipboardSelection.cpp source/RichClipboardTransfer.cpp
        native/AssetClipboardTransfer.cpp native/worker/AssetStore.cpp)
    target_include_directories(dockpipe-native-desktop-display PRIVATE
        source "${CMAKE_CURRENT_BINARY_DIR}" "${DOCKPIPE_DESKTOP_DEPENDENCIES}/ffmpeg"
        "${DOCKPIPE_DESKTOP_DEPENDENCIES}/json/single_include")
    target_link_libraries(dockpipe-native-desktop-display PRIVATE
        DockPipe::DisplayHeaders Qt6::Widgets Qt6::OpenGLWidgets Qt6::Network
        desktop-linux-media X11::X11)
    set(display_build_abi "x86_64-little_endian-lp64")
    configure_file(assets/display-metadata.json.in display-metadata.json @ONLY)
    set_target_properties(dockpipe-native-desktop-display PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/assets/display"
        BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN/lib")
    target_link_options(dockpipe-native-desktop-display PRIVATE -Wl,--disable-new-dtags)
    add_executable(native-desktop-clipboard-paste-intent-test
        tests/ClipboardPasteIntentTest.cpp source/ClipboardPasteIntent.cpp)
    target_include_directories(native-desktop-clipboard-paste-intent-test PRIVATE source)
    target_compile_options(native-desktop-clipboard-paste-intent-test PRIVATE -UNDEBUG)
    target_link_libraries(native-desktop-clipboard-paste-intent-test PRIVATE Qt6::Core)
    add_test(NAME native-desktop-clipboard-paste-intent COMMAND native-desktop-clipboard-paste-intent-test)
    set_tests_properties(native-desktop-clipboard-paste-intent PROPERTIES TIMEOUT 10)
    add_executable(native-desktop-rich-clipboard-test tests/RichClipboardTransferTest.cpp
        source/RichClipboardTransfer.cpp source/ClipboardSelection.cpp source/ProcessChannel.cpp
        native/AssetClipboardTransfer.cpp native/worker/AssetStore.cpp)
    target_include_directories(native-desktop-rich-clipboard-test PRIVATE source
        "${DOCKPIPE_DESKTOP_DEPENDENCIES}/json/single_include")
    target_link_libraries(native-desktop-rich-clipboard-test PRIVATE Qt6::Widgets Qt6::Test Threads::Threads)
    add_test(NAME native-desktop-rich-clipboard COMMAND native-desktop-rich-clipboard-test)
    set_tests_properties(native-desktop-rich-clipboard PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=${desktop_test_platform};LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    add_executable(native-desktop-clipboard-selection-test
        tests/ClipboardSelectionTest.cpp source/ClipboardSelection.cpp)
    target_include_directories(native-desktop-clipboard-selection-test PRIVATE source)
    target_link_libraries(native-desktop-clipboard-selection-test PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME native-desktop-clipboard-selection COMMAND native-desktop-clipboard-selection-test)
    set_tests_properties(native-desktop-clipboard-selection PROPERTIES TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    add_executable(native-desktop-ipc-test tests/ProcessChannelTest.cpp source/ProcessChannel.cpp)
    target_include_directories(native-desktop-ipc-test PRIVATE source)
    target_link_libraries(native-desktop-ipc-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME native-desktop-ipc COMMAND native-desktop-ipc-test)
    set_tests_properties(native-desktop-ipc PROPERTIES TIMEOUT 20)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_library(native-desktop-media-load SHARED tests/MediaClosure.cpp)
    target_link_libraries(native-desktop-media-load PRIVATE desktop-linux-media)
    add_test(NAME native-desktop-media-load
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/media_load.py"
        --module "$<TARGET_FILE:native-desktop-media-load>")
    set_tests_properties(native-desktop-media-load PROPERTIES
        TIMEOUT 10 ENVIRONMENT "LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    add_executable(native-desktop-viewer-test tests/DesktopViewerSurfaceTest.cpp
        source/DesktopViewerSurface.cpp source/ClipboardPasteIntent.cpp source/GpuVideoWidget.cpp source/H264Decoder.cpp source/HardwareVideoDecoder.cpp source/ProcessChannel.cpp source/InputMapping.cpp
        source/ClipboardSelection.cpp source/RichClipboardTransfer.cpp
        native/AssetClipboardTransfer.cpp native/worker/AssetStore.cpp)
    target_include_directories(native-desktop-viewer-test PRIVATE source "${DOCKPIPE_DESKTOP_DEPENDENCIES}/json/single_include")
    target_compile_definitions(native-desktop-viewer-test PRIVATE
        FIXTURE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_fixture.py"
        VIDEO_FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/h264")
    target_link_libraries(native-desktop-viewer-test PRIVATE
        DockPipe::DisplayHeaders Qt6::Widgets Qt6::OpenGLWidgets Qt6::Network Qt6::Test desktop-linux-media Threads::Threads X11::X11)
    add_test(NAME native-desktop-viewer COMMAND native-desktop-viewer-test)
    set_tests_properties(native-desktop-viewer PROPERTIES TIMEOUT 75
        ENVIRONMENT "QT_QPA_PLATFORM=${desktop_test_platform};LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    add_executable(native-desktop-plugin-test tests/PluginAdmissionTest.cpp)
    target_link_libraries(native-desktop-plugin-test PRIVATE DockPipe::DisplayHeaders Qt6::Widgets Qt6::Test)
    target_compile_definitions(native-desktop-plugin-test PRIVATE
        PLUGIN_PATH="$<TARGET_FILE:dockpipe-native-desktop-display>"
        DISPLAY_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        DISPLAY_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}")
    add_dependencies(native-desktop-plugin-test dockpipe-native-desktop-display)
    add_test(NAME native-desktop-plugin-admission COMMAND native-desktop-plugin-test)
    set_tests_properties(native-desktop-plugin-admission PROPERTIES TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=${desktop_test_platform};LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    add_executable(native-desktop-hardware-decoder-test tests/HardwareVideoDecoderTest.cpp source/HardwareVideoDecoder.cpp)
    target_include_directories(native-desktop-hardware-decoder-test PRIVATE source)
    target_link_libraries(native-desktop-hardware-decoder-test PRIVATE desktop-linux-media)
    target_compile_options(native-desktop-hardware-decoder-test PRIVATE -UNDEBUG)
    add_test(NAME native-desktop-hardware-decoder COMMAND native-desktop-hardware-decoder-test)
    set_tests_properties(native-desktop-hardware-decoder PROPERTIES TIMEOUT 15
        ENVIRONMENT "LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    add_executable(native-desktop-gpu-presentation-test tests/GpuVideoWidgetTest.cpp
        source/GpuVideoWidget.cpp source/H264Decoder.cpp source/HardwareVideoDecoder.cpp)
    target_include_directories(native-desktop-gpu-presentation-test PRIVATE source)
    target_compile_definitions(native-desktop-gpu-presentation-test PRIVATE
        FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/h264")
    target_link_libraries(native-desktop-gpu-presentation-test PRIVATE
        Qt6::OpenGLWidgets Qt6::Test desktop-linux-media)
    add_test(NAME native-desktop-gpu-presentation COMMAND native-desktop-gpu-presentation-test)
    set_tests_properties(native-desktop-gpu-presentation PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=${desktop_test_platform};LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")

    add_executable(native-desktop-frame-test tests/FrameCodecTest.cpp)
    target_include_directories(native-desktop-frame-test PRIVATE source)
    target_compile_options(native-desktop-frame-test PRIVATE -UNDEBUG)
    add_test(NAME native-desktop-frame COMMAND native-desktop-frame-test)
    set_tests_properties(native-desktop-frame PROPERTIES TIMEOUT 10)
    add_executable(native-desktop-decoder-test tests/H264DecoderTest.cpp source/H264Decoder.cpp source/HardwareVideoDecoder.cpp)
    target_include_directories(native-desktop-decoder-test PRIVATE source)
    target_compile_definitions(native-desktop-decoder-test PRIVATE
        FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/h264")
    target_link_libraries(native-desktop-decoder-test PRIVATE Qt6::Gui Qt6::Test desktop-linux-media)
    add_test(NAME native-desktop-decoder COMMAND native-desktop-decoder-test)
    set_tests_properties(native-desktop-decoder PROPERTIES TIMEOUT 15
        ENVIRONMENT "LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    add_executable(native-desktop-input-mapping-test tests/InputMappingTest.cpp
        source/InputMapping.cpp native/InputState.cpp)
    target_include_directories(native-desktop-input-mapping-test PRIVATE source native)
    target_link_libraries(native-desktop-input-mapping-test PRIVATE Qt6::Gui Qt6::Test X11::X11)
    add_test(NAME native-desktop-input-mapping COMMAND native-desktop-input-mapping-test)
    set_tests_properties(native-desktop-input-mapping PROPERTIES TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=${desktop_test_platform};LD_LIBRARY_PATH=${DOCKPIPE_DESKTOP_QT_PREFIX}/lib")
    install(TARGETS dockpipe-native-desktop-display LIBRARY DESTINATION assets/display)
elseif(APPLE)
    if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        message(FATAL_ERROR "The initial sender requires native Darwin arm64")
    endif()
    enable_language(OBJCXX)
    native_worker_test(native-desktop-asset-image NativeAssetImageTest.mm native/worker/NativeAssetClipboard.mm)
    target_compile_options(native-desktop-asset-image PRIVATE -fobjc-arc -fblocks -Wno-deprecated-declarations)
    target_link_libraries(native-desktop-asset-image PRIVATE
        "-framework Foundation" "-framework AppKit" "-framework CoreGraphics"
        "-framework ImageIO" "-framework Security")
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    native_worker_test(native-desktop-worker-host-template-io WorkerHostTemplateIOTest.mm native/worker/WorkerHostTemplateMac.mm)
    target_compile_options(native-desktop-worker-host-template-io PRIVATE -fobjc-arc -fblocks)
    target_link_libraries(native-desktop-worker-host-template-io PRIVATE "-framework Foundation" "-framework Security")
    native_worker_test(native-desktop-worker-runloop WorkerRunLoopTest.mm native/worker/WorkerRunLoop.mm)
    target_compile_options(native-desktop-worker-runloop PRIVATE -fobjc-arc -fblocks)
    native_worker_test(native-desktop-worker-application WorkerApplicationTest.mm)
    target_compile_options(native-desktop-worker-application PRIVATE -fobjc-arc -fblocks)
    add_library(dockpipe-native-desktop-worker-module MODULE
        native/worker/NativeWorkerPackage.mm
        native/worker/NativeWorker.mm
        native/worker/NativeClipboard.mm
        native/worker/NativeAssets.cpp
        native/worker/NativeAssetClipboard.mm
        native/worker/AssetStore.cpp
        native/AssetClipboardTransfer.cpp
        native/worker/WorkerRunLoop.mm
        native/worker/NativePowerPolicy.mm
        native/worker/WorkerHostTemplateMac.mm
        native/NativeVirtualDisplayOwnerProtocol.cpp
        native/NativeVirtualDisplayOwnerClient.cpp
        native/NativeVirtualDisplaySnapshot.cpp
        native/NativeVirtualDisplayOwner.mm
        native/NativeVirtualDisplayObserver.mm
        native/NativeVirtualDisplayObserverClient.cpp
        native/NativeVirtualDisplayObserverProtocol.cpp
        native/worker/ChildProcess.cpp
        native/NativeDesktopBackend.mm
        native/NativeVirtualDisplay.mm
        native/NativePowerAssertion.cpp
        native/NativePowerAssertion.mm
        native/InputState.cpp)
    target_compile_definitions(dockpipe-native-desktop-worker-module PRIVATE DOCKPIPE_NATIVE_WORKER_MODULE=1)
    target_include_directories(dockpipe-native-desktop-worker-module PRIVATE
        source native native/worker "${DOCKPIPE_DESKTOP_DEPENDENCIES}/json/single_include"
        "${DOCKPIPE_DESKTOP_DEPENDENCIES}/worker-sdk/include")
    target_link_libraries(dockpipe-native-desktop-worker-module PRIVATE Threads::Threads)
    target_compile_options(dockpipe-native-desktop-worker-module PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>" "$<$<COMPILE_LANGUAGE:OBJCXX>:-fblocks>")
    set_target_properties(dockpipe-native-desktop-worker-module PROPERTIES
        CXX_VISIBILITY_PRESET hidden OBJCXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    install(TARGETS dockpipe-native-desktop-worker-module LIBRARY DESTINATION assets/worker)
    add_executable(native-desktop-worker-module-contract native/worker/WorkerModuleTest.cpp)
    target_include_directories(native-desktop-worker-module-contract PRIVATE
        "${DOCKPIPE_DESKTOP_DEPENDENCIES}/worker-sdk/include")
    target_compile_options(native-desktop-worker-module-contract PRIVATE -UNDEBUG)
    target_link_libraries(native-desktop-worker-module-contract PRIVATE Threads::Threads)
    add_dependencies(native-desktop-worker-module-contract dockpipe-native-desktop-worker-module)
    add_test(NAME native-desktop-worker-module-contract COMMAND native-desktop-worker-module-contract
        "$<TARGET_FILE:dockpipe-native-desktop-worker-module>")
    set_tests_properties(native-desktop-worker-module-contract PROPERTIES TIMEOUT 15)
    add_executable(dockpipe-native-desktop-worker MACOSX_BUNDLE
        native/worker/NativeWorker.mm
        native/worker/NativeClipboard.mm
        native/worker/NativeAssets.cpp
        native/worker/NativeAssetClipboard.mm
        native/worker/AssetStore.cpp
        native/AssetClipboardTransfer.cpp
        native/worker/WorkerRunLoop.mm
        native/worker/NativePowerPolicy.mm
        native/worker/WorkerHostTemplateMac.mm
        native/NativeVirtualDisplayOwnerProtocol.cpp
        native/NativeVirtualDisplayOwnerClient.cpp
        native/NativeVirtualDisplaySnapshot.cpp
        native/NativeVirtualDisplayOwner.mm
        native/NativeVirtualDisplayObserver.mm
        native/NativeVirtualDisplayObserverClient.cpp
        native/NativeVirtualDisplayObserverProtocol.cpp
        native/worker/ChildProcess.cpp
        native/NativeDesktopBackend.mm
        native/NativeVirtualDisplay.mm
        native/NativePowerAssertion.cpp
        native/NativePowerAssertion.mm
        native/InputState.cpp)
    target_include_directories(dockpipe-native-desktop-worker PRIVATE
        source native native/worker "${DOCKPIPE_DESKTOP_DEPENDENCIES}/json/single_include"
        "${DOCKPIPE_DESKTOP_DEPENDENCIES}/worker-sdk/include")
    target_link_libraries(dockpipe-native-desktop-worker PRIVATE Threads::Threads)
    target_compile_options(dockpipe-native-desktop-worker PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fblocks>")
    foreach(framework IN ITEMS Foundation AppKit ScreenCaptureKit VideoToolbox CoreMedia CoreVideo CoreGraphics ApplicationServices IOKit CoreFoundation Security ImageIO)
        find_library(${framework}_FRAMEWORK ${framework} REQUIRED)
        target_link_libraries(dockpipe-native-desktop-worker PRIVATE "${${framework}_FRAMEWORK}")
        target_link_libraries(dockpipe-native-desktop-worker-module PRIVATE "${${framework}_FRAMEWORK}")
    endforeach()
    target_link_libraries(native-desktop-worker-runloop PRIVATE "${Foundation_FRAMEWORK}")
    target_link_libraries(native-desktop-worker-application PRIVATE "${AppKit_FRAMEWORK}")
    native_worker_test(native-desktop-stop-ownership NativeStopOwnershipTest.mm native/InputState.cpp native/NativeVirtualDisplay.mm)
    target_compile_options(native-desktop-stop-ownership PRIVATE -fobjc-arc -fblocks)
    foreach(framework IN ITEMS Foundation AppKit ScreenCaptureKit VideoToolbox CoreMedia CoreVideo CoreGraphics ApplicationServices)
        target_link_libraries(native-desktop-stop-ownership PRIVATE "${${framework}_FRAMEWORK}")
    endforeach()
    native_worker_test(native-desktop-capture-configuration NativeCaptureConfigurationTest.mm native/InputState.cpp native/NativeVirtualDisplay.mm)
    target_compile_options(native-desktop-capture-configuration PRIVATE -fobjc-arc -fblocks)
    foreach(framework IN ITEMS Foundation AppKit ScreenCaptureKit VideoToolbox CoreMedia CoreVideo CoreGraphics ApplicationServices)
        target_link_libraries(native-desktop-capture-configuration PRIVATE "${${framework}_FRAMEWORK}")
    endforeach()
    native_worker_test(native-desktop-hardware-capture NativeHardwareCaptureTest.mm native/InputState.cpp native/NativeVirtualDisplay.mm)
    target_compile_options(native-desktop-hardware-capture PRIVATE -fobjc-arc -fblocks)
    foreach(framework IN ITEMS Foundation AppKit ScreenCaptureKit VideoToolbox CoreMedia CoreVideo CoreGraphics ApplicationServices)
        target_link_libraries(native-desktop-hardware-capture PRIVATE "${${framework}_FRAMEWORK}")
    endforeach()
    native_worker_test(native-desktop-virtual-display ../NativeVirtualDisplayTest.mm native/NativeVirtualDisplay.mm)
    target_compile_options(native-desktop-virtual-display PRIVATE -fobjc-arc -fblocks)
    target_link_libraries(native-desktop-virtual-display PRIVATE "${Foundation_FRAMEWORK}" "${CoreGraphics_FRAMEWORK}")
    add_test(NAME native-desktop-virtual-display-online-unknown COMMAND native-desktop-virtual-display --unknown-online)
    add_test(NAME native-desktop-virtual-display-query-unknown COMMAND native-desktop-virtual-display --unknown-query)
    add_test(NAME native-desktop-virtual-display-late-unknown COMMAND native-desktop-virtual-display --unknown-late)
    set_tests_properties(native-desktop-virtual-display-online-unknown native-desktop-virtual-display-query-unknown
        native-desktop-virtual-display-late-unknown
        PROPERTIES TIMEOUT 15)
    # Real power assertions are opt-in; the producer only builds this probe.
    add_executable(native-desktop-power-probe native/NativePowerProbe.mm
        native/NativePowerAssertion.cpp native/NativePowerAssertion.mm)
    target_include_directories(native-desktop-power-probe PRIVATE native)
    target_compile_options(native-desktop-power-probe PRIVATE -fobjc-arc)
    target_link_libraries(native-desktop-power-probe PRIVATE Threads::Threads
        "${IOKit_FRAMEWORK}" "${CoreFoundation_FRAMEWORK}")
    # Deliberately not a CTest: actual display creation needs separate operator review.
    add_executable(native-desktop-virtual-display-probe native/NativeVirtualDisplayProbe.mm native/NativeVirtualDisplay.mm)
    target_include_directories(native-desktop-virtual-display-probe PRIVATE native)
    target_compile_options(native-desktop-virtual-display-probe PRIVATE -fobjc-arc -fblocks)
    target_link_libraries(native-desktop-virtual-display-probe PRIVATE Threads::Threads
        "${Foundation_FRAMEWORK}" "${CoreGraphics_FRAMEWORK}")
    set_target_properties(dockpipe-native-desktop-worker PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Dockpipe Native Desktop Worker"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/macos/Info.plist.in")
    install(TARGETS dockpipe-native-desktop-worker BUNDLE DESTINATION assets/tooling)
endif()
