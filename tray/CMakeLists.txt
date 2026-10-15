cmake_minimum_required(VERSION 3.16)

# Set before project(); otherwise CMake fixes minos to the host SDK. Homebrew Qt
# 6.11 frameworks are built for macOS 14.0, so the app cannot start on older systems.
# FORCE also raises a lower value cached by an existing build directory.
if(APPLE AND (NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.0"))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(hgs-tray VERSION 0.42.2 LANGUAGES CXX)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# A program's moc and rcc do not wait for the libraries it links (none generates a
# header another target's moc reads; rcc runs in a per-resource target), so with
# Ninja hgs-tray and the suites compile their own sources while the libraries build.
set(CMAKE_AUTOGEN_ORIGIN_DEPENDS OFF)
set(CMAKE_GLOBAL_AUTORCC_TARGET ON)
# Чтобы clangd в редакторе знал пути к заголовкам Qt и не краснел на каждый #include.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    # UTF-8 sources and literals also in targets without Qt (Qt adds -utf-8 only to
    # its users), large translation units (SessionsWindow and its test), and
    # <windows.h> without min/max macros next to std::min/std::max.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
    add_compile_definitions(NOMINMAX)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia)
# std::thread сторожа главного потока (MainThreadWatchdog) -- на Linux это -lpthread.
find_package(Threads REQUIRED)
include(cmake/Terminal.cmake)
include(cmake/Markdown.cmake)
if(LINUX)
    # Konsole's tab colour and window raising have no escape sequence — only D-Bus.
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    # Native StatusNotifier activation and a panel-hosted DBusMenu on Wayland.
    find_package(KF6StatusNotifierItem REQUIRED)
    find_package(KF6WindowSystem REQUIRED)
    # Input idle time where org.freedesktop.ScreenSaver has none (Plasma on Wayland).
    find_package(KF6IdleTime REQUIRED)
endif()

add_library(hgs-window-layer STATIC src/WindowLayer.cpp)
target_include_directories(hgs-window-layer PUBLIC src)
target_link_libraries(hgs-window-layer PUBLIC Qt6::Widgets)
if(LINUX)
    target_sources(hgs-window-layer PRIVATE src/KWinWindowLayer.cpp)
    target_link_libraries(hgs-window-layer PRIVATE Qt6::DBus)
endif()

# Opening session files like browser downloads (see ExternalFile.h).
add_library(hgs-external-file STATIC src/ExternalFile.cpp)
target_include_directories(hgs-external-file PUBLIC src)
target_link_libraries(hgs-external-file PUBLIC Qt6::Gui)
if(LINUX)
    target_link_libraries(hgs-external-file PRIVATE Qt6::DBus)
elseif(WIN32)
    target_link_libraries(hgs-external-file PRIVATE shlwapi)
elseif(APPLE)
    target_link_libraries(hgs-external-file PRIVATE "-framework CoreFoundation" "-framework CoreServices")
endif()

add_library(hgs-updates STATIC src/UpdateController.cpp src/UpdateController.h)
target_include_directories(hgs-updates PUBLIC src)
target_link_libraries(hgs-updates PUBLIC Qt6::Widgets Qt6::Network)

# The recorder alone serves the macOS microphone permission suite.
add_library(hgs-speech-recorder STATIC src/speech/SpeechRecorder.cpp)
target_include_directories(hgs-speech-recorder PUBLIC src)
target_link_libraries(hgs-speech-recorder PUBLIC Qt6::Multimedia)
add_library(hgs-speech STATIC src/speech/SpeechClient.cpp
    src/speech/SpeechSettings.cpp src/speech/SpeechComposer.cpp)
target_include_directories(hgs-speech PUBLIC src)
# The voice composer extends MessageComposer (hgs-tray-ui below).
target_link_libraries(hgs-speech PUBLIC Qt6::Widgets Qt6::Multimedia hgs-speech-recorder PRIVATE hgs-tray-ui)
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE ZERUS_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --show-toplevel WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE ZERUS_GIT_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain --untracked-files=normal
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE ZERUS_SOURCE_CHANGES RESULT_VARIABLE ZERUS_GIT_STATUS ERROR_QUIET)
get_filename_component(ZERUS_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)
string(LENGTH "${ZERUS_SOURCE_COMMIT}" ZERUS_SOURCE_COMMIT_LENGTH)
set(ZERUS_CLEAN_SOURCE FALSE)
if(ZERUS_SOURCE_COMMIT MATCHES "^[a-f0-9]+$" AND ZERUS_SOURCE_COMMIT_LENGTH EQUAL 40
   AND ZERUS_GIT_ROOT STREQUAL ZERUS_PROJECT_ROOT AND ZERUS_GIT_STATUS EQUAL 0)
    target_compile_definitions(hgs-updates PRIVATE ZERUS_BUILD_SOURCE_COMMIT="${ZERUS_SOURCE_COMMIT}")
    if(ZERUS_SOURCE_CHANGES STREQUAL "")
        set(ZERUS_CLEAN_SOURCE TRUE)
        target_compile_definitions(hgs-updates PRIVATE ZERUS_SOURCE_COMMIT="${ZERUS_SOURCE_COMMIT}" ZERUS_BUILD_SOURCE_DIRTY=0)
    else()
        target_compile_definitions(hgs-updates PRIVATE ZERUS_BUILD_SOURCE_DIRTY=1)
    endif()
endif()
include(cmake/BuildVersion.cmake)

# Tray sources compile once, into static libraries that hgs-tray and the Qt suites
# link; tests/CMakeLists.txt fails when a suite lists a source that one of them
# builds. Each layer adds Qt modules, so a suite that links the lowest layer
# covering its code keeps its QTEST_MAIN application type.
add_library(hgs-tray-core STATIC src/ProcessRunner.cpp src/HgsClient.cpp src/FleetState.cpp
    src/ReadMarkSync.cpp src/AppConfig.cpp src/AccountUsageStore.cpp src/ComposerDraftStore.cpp
    src/SwarmController.cpp src/DesktopPresence.cpp src/AttentionNotifier.h src/CommandBackend.cpp
    src/LocalSessionHost.cpp src/DownloadCache.cpp
    # Main-thread watchdog (MainThreadWatchdog.h): every platform, prompted by macOS.
    src/MainThreadWatchdog.cpp)
if(NOT WIN32)
    # Service stops and terminal signals quit like the tray menu (main.cpp).
    target_sources(hgs-tray-core PRIVATE src/TerminationSignals.cpp)
endif()
target_include_directories(hgs-tray-core PUBLIC src)
target_link_libraries(hgs-tray-core PUBLIC Qt6::Core Threads::Threads)

# The single-instance socket and its serialized start (SingleInstance.h, main.cpp).
add_library(hgs-single-instance STATIC src/SingleInstance.cpp)
target_include_directories(hgs-single-instance PUBLIC src)
target_link_libraries(hgs-single-instance PUBLIC Qt6::Network)

add_library(hgs-tray-gui STATIC src/AttentionTracker.cpp
    # The clipboard exists everywhere: unlike Konsole and Terminal.app, this
    # backend builds on every platform and asks the platform itself.
    src/ClipboardBackend.cpp)
target_link_libraries(hgs-tray-gui PUBLIC hgs-tray-core Qt6::Gui)
if(LINUX)
    target_link_libraries(hgs-tray-gui PRIVATE Qt6::DBus)
endif()

add_library(hgs-tray-ui STATIC src/SessionOrganization.cpp src/SessionList.cpp src/DashboardPage.cpp src/GraphCanvas.cpp src/GraphPage.cpp src/GraphController.cpp src/SessionGraphStore.cpp
    src/MachineFilter.cpp src/SessionSearch.cpp src/MachinesPage.cpp src/AccountsPage.cpp
    src/ComposerToolbar.cpp src/MessageComposer.cpp src/QuestionCard.cpp src/DirectoryDialog.cpp
    src/NewSessionDialog.cpp src/ProjectsDialog.cpp src/WorktreesDialog.cpp src/SwarmDialog.cpp src/SwarmConflictReview.cpp
    src/ProcessView.cpp src/TrayMenu.cpp src/IconFactory.cpp src/Shortcuts.cpp src/DownloadCacheSettings.cpp src/GoToDialog.cpp
    src/KeyboardSettings.cpp src/ShortcutsDialog.cpp
    # Tray icon decorator (QuietTrayIcon.h): portable, but wrapped around the icon
    # only on macOS; makeTrayIcon() in TrayAgent.cpp explains why not on Linux.
    src/QuietTrayIcon.cpp)
target_link_libraries(hgs-tray-ui PUBLIC hgs-tray-core Qt6::Widgets
    PRIVATE hgs-markdown hgs-external-file hgs-updates)

add_library(hgs-tray-activity STATIC src/ActivityView.cpp)
target_include_directories(hgs-tray-activity PUBLIC src)
target_link_libraries(hgs-tray-activity PUBLIC Qt6::Widgets Qt6::Multimedia PRIVATE hgs-markdown)

add_library(hgs-tray-views STATIC src/SessionsWindow.cpp src/SessionsWindowShortcuts.cpp src/SessionsWindowGoTo.cpp src/SessionSelection.cpp src/NativeUiView.cpp
    src/NativeBrowserPlugin.cpp src/NativeBrowser.h)
target_link_libraries(hgs-tray-views PUBLIC hgs-tray-ui hgs-tray-activity
    PRIVATE hgs-terminal hgs-markdown hgs-window-layer hgs-speech hgs-external-file hgs-updates)

# The embedded DeepSeek native UI is an optional Qt plugin: hgs-tray never links
# Qt WebEngine. Linux only; AUTO builds it when Qt WebEngine is installed.
# Without the plugin the native UI opens in the system browser.
set(HGS_NATIVE_UI_PLUGIN AUTO CACHE STRING "Build the embedded native UI plugin: AUTO, ON or OFF (Linux only)")
set_property(CACHE HGS_NATIVE_UI_PLUGIN PROPERTY STRINGS AUTO ON OFF)
if(NOT HGS_NATIVE_UI_PLUGIN MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "HGS_NATIVE_UI_PLUGIN must be AUTO, ON or OFF")
endif()
if(NOT LINUX AND HGS_NATIVE_UI_PLUGIN STREQUAL "ON")
    message(FATAL_ERROR "The embedded native UI plugin is built only on Linux")
endif()
if(LINUX AND NOT HGS_NATIVE_UI_PLUGIN STREQUAL "OFF")
    if(HGS_NATIVE_UI_PLUGIN STREQUAL "ON")
        find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets)
    else()
        find_package(Qt6 QUIET OPTIONAL_COMPONENTS WebEngineWidgets)
    endif()
endif()
if(LINUX AND NOT HGS_NATIVE_UI_PLUGIN STREQUAL "OFF" AND TARGET Qt6::WebEngineWidgets)
    # The GUI looks for it in <bin>/plugins (this build tree) and <bin>/../lib/hgs-tray/plugins.
    add_library(hgs-tray-native-ui MODULE plugins/NativeUiPlugin.cpp src/NativeBrowser.h)
    target_include_directories(hgs-tray-native-ui PRIVATE src)
    target_link_libraries(hgs-tray-native-ui PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
    set_target_properties(hgs-tray-native-ui PROPERTIES PREFIX "lib" OUTPUT_NAME "hgs-tray-native-ui"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
    message(STATUS "Embedded native UI plugin: enabled")
else()
    # A plugin left by an earlier configuration must not be installed with this GUI.
    file(REMOVE "${CMAKE_BINARY_DIR}/plugins/libhgs-tray-native-ui.so")
    message(STATUS "Embedded native UI plugin: disabled (the native UI opens in the browser)")
endif()

# Platform integration, each where it applies. The Windows pieces have portable
# logic that a suite checks everywhere; other systems build them only for it.
if(LINUX)
    # Konsole (TerminalBackend.h is a header-only interface), KStatusNotifierItem
    # as the tray icon (TrayIcon.h is header-only too), D-Bus notifications, the
    # launcher badge and input idle time.
    add_library(hgs-tray-linux STATIC src/KonsoleBackend.cpp src/SniTrayIcon.cpp
        src/LinuxAttentionNotifier.cpp src/LinuxApplicationBadge.cpp src/LinuxPresenceSource.cpp)
    target_link_libraries(hgs-tray-linux PUBLIC hgs-tray-core Qt6::DBus
        PRIVATE Qt6::Widgets KF6::StatusNotifierItem KF6::IdleTime)
endif()
# Windows Terminal and console windows (hgs-tray.exe --console-run hosts the
# latter), tray-icon notifications, the swarm worker the tray runs itself
# (Windows has no user service manager) and the note while it picks its hgs.
add_library(hgs-tray-windows STATIC src/WindowsTerminalBackend.cpp src/ConsoleHost.cpp
    src/WindowsAttentionNotifier.cpp src/SwarmWorkerSupervisor.cpp src/StartupNotice.cpp)
target_link_libraries(hgs-tray-windows PUBLIC hgs-tray-core Qt6::Widgets)
if(NOT WIN32)
    set_target_properties(hgs-tray-windows PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
if(APPLE)
    # Like Konsole: osascript and Terminal.app exist only on macOS. QSystemTrayIcon
    # with a native menu, notifications, Dock visibility, the badge and idle time.
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    add_library(hgs-tray-mac STATIC src/MacBackend.cpp src/QtTrayIcon.cpp src/MacTrayMenu.mm
        src/MacAttentionNotifier.mm src/MacAppVisibility.mm src/MacApplicationBadge.mm src/MacPresenceSource.cpp)
    set_source_files_properties(src/MacAttentionNotifier.mm src/MacAppVisibility.mm src/MacApplicationBadge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(hgs-tray-mac PUBLIC hgs-tray-core Qt6::Widgets
        PRIVATE "-framework AppKit" "-framework UserNotifications" "-framework CoreGraphics")
endif()

# Qt finalization imports macOS permission plugins selected by our Info.plist.
# A plain add_executable silently leaves QMicrophonePermission without a backend.
qt_add_executable(hgs-tray MANUAL_FINALIZATION resources.qrc src/main.cpp src/TrayAgent.cpp)
if(APPLE)
    # LSUIElement живёт в Info.plist; без MACOSX_BUNDLE получится голый исполняемый файл.
    # Icon Composer catalog provides native layers/masking; actool also produces
    # the ICNS fallback. Both are checked in so regular builds need no Xcode 26.
    set_source_files_properties(resources/icons/hgs-zerus-swarm.icns resources/icons/macos/Assets.car PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(hgs-tray PRIVATE resources/icons/hgs-zerus-swarm.icns resources/icons/macos/Assets.car)
    # Localized display name: macOS shows Zerus for the compatible hgs-tray.app path.
    set_source_files_properties(packaging/en.lproj/InfoPlist.strings PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources/en.lproj)
    target_sources(hgs-tray PRIVATE packaging/en.lproj/InfoPlist.strings)
    set_target_properties(hgs-tray PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.hgdev.hgs-tray"
        MACOSX_BUNDLE_ICON_FILE "hgs-zerus-swarm.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/Info.plist"
    )
endif()
if(LINUX)
    target_link_libraries(hgs-tray PRIVATE hgs-tray-linux)
elseif(APPLE)
    target_link_libraries(hgs-tray PRIVATE hgs-tray-mac)
else()
    # QSystemTrayIcon, as on macOS (where it has a native menu).
    target_sources(hgs-tray PRIVATE src/QtTrayIcon.cpp)
endif()
if(WIN32)
    # The taskbar badge. The resource script carries the icon and version
    # information; the manifest sets UTF-8 as the ANSI code page and long-path support.
    set(HGS_TRAY_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/hgs-zerus.ico")
    configure_file(packaging/windows/hgs-tray.rc.in "${CMAKE_CURRENT_BINARY_DIR}/hgs-tray.rc" @ONLY)
    target_sources(hgs-tray PRIVATE src/WindowsApplicationBadge.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/hgs-tray.rc" packaging/windows/hgs-tray.manifest)
    target_link_libraries(hgs-tray PRIVATE hgs-tray-windows)
    # A GUI program: no console window opens with it (see attachParentConsole in main.cpp).
    set_target_properties(hgs-tray PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_compile_definitions(hgs-tray PRIVATE HGS_TRAY_VERSION="${PROJECT_VERSION}")
# QLocalServer/QLocalSocket (single-instance guard in main.cpp) live in QtNetwork, not
# QtWidgets — the plan's dependency list missed this; noted in the Task 1 report.
target_link_libraries(hgs-tray PRIVATE hgs-tray-views hgs-tray-gui hgs-single-instance Qt6::Widgets Qt6::Network
    Threads::Threads hgs-terminal hgs-markdown hgs-window-layer)
if(LINUX)
    target_link_libraries(hgs-tray PRIVATE Qt6::DBus KF6::StatusNotifierItem KF6::WindowSystem KF6::IdleTime)
endif()

target_link_libraries(hgs-tray PRIVATE hgs-updates hgs-speech hgs-external-file)
qt_finalize_executable(hgs-tray)

include(CTest)
if(BUILD_TESTING)
    hgs_add_terminal_tests()
    add_subdirectory(tests)
    # test_terminalview is registered in this directory.
    zerus_test_environment()
endif()

include(GNUInstallDirs)
if(APPLE)
    install(TARGETS hgs-tray BUNDLE DESTINATION .)
else()
    install(TARGETS hgs-tray RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET hgs-tray-native-ui)
    install(TARGETS hgs-tray-native-ui LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/hgs-tray/plugins)
endif()

# Regenerate the monochrome window/Linux icon sizes from the same blade geometry.
# The native macOS application icon uses tools/export-macos-icon.sh instead.
# Explicit target only: normal builds use the checked-in PNGs.
add_executable(hgs-export-icons EXCLUDE_FROM_ALL tools/export-icons.cpp src/IconFactory.cpp)
target_include_directories(hgs-export-icons PRIVATE src)
target_link_libraries(hgs-export-icons PRIVATE Qt6::Widgets)
