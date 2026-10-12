cmake_minimum_required(VERSION 3.16)

# Set before project(); otherwise CMake fixes minos to the host SDK. Homebrew Qt
# 6.11 frameworks are built for macOS 14.0, so the app cannot start on older systems.
# FORCE also raises a lower value cached by an existing build directory.
if(APPLE AND (NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.0"))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(hgs-tray VERSION 0.42.0 LANGUAGES CXX)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
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

find_package(Qt6 REQUIRED COMPONENTS Widgets Network WebEngineWidgets)
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

add_library(hgs-updates STATIC src/UpdateController.cpp src/UpdateController.h)
target_include_directories(hgs-updates PUBLIC src)
target_link_libraries(hgs-updates PUBLIC Qt6::Widgets Qt6::Network)
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

add_executable(hgs-tray
    resources.qrc
    src/main.cpp
    src/AppConfig.cpp
    src/HgsClient.cpp
    src/ProcessRunner.cpp
    src/FleetState.cpp
    src/ReadMarkSync.cpp
    src/IconFactory.cpp
    src/CommandBackend.cpp
    # Буфер обмена есть везде -- в отличие от Konsole и Terminal.app, этот бэкенд не
    # платформенный и собирается всегда (внутри он спрашивает платформу сам).
    src/ClipboardBackend.cpp
    src/SessionsWindow.cpp src/SessionSelection.cpp
    src/MachinesPage.cpp src/SessionSearch.cpp
    src/MachineFilter.cpp src/DashboardPage.cpp src/AccountsPage.cpp src/AccountUsageStore.cpp
    src/SessionOrganization.cpp
    src/SwarmController.cpp src/SwarmDialog.cpp src/SwarmConflictReview.cpp
    src/SessionList.cpp
    src/ActivityView.cpp
    src/ProcessView.cpp
    src/NativeUiView.cpp
    src/MessageComposer.cpp
    src/ComposerToolbar.cpp
    src/ComposerDraftStore.cpp
    src/QuestionCard.cpp
    src/DirectoryDialog.cpp
    src/NewSessionDialog.cpp
    src/ProjectsDialog.cpp
    src/TrayMenu.cpp
    src/TrayAgent.cpp
    src/AttentionTracker.cpp
    src/DesktopPresence.cpp
    src/AttentionNotifier.h
    # Декоратор над иконкой трея (см. QuietTrayIcon.h). Платформенного в нём ничего нет,
    # собирается везде; надевается на иконку только на маке -- см. makeTrayIcon() в
    # TrayAgent.cpp, там же почему не на Linux.
    src/QuietTrayIcon.cpp
    # Сторож главного потока (см. MainThreadWatchdog.h): везде, повод -- маковский.
    src/MainThreadWatchdog.cpp
)
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
    # Konsole-бэкенд собирается только там, где он вообще применим; TerminalBackend.h
    # (чистый интерфейс) заголовочный и в списке исходников не нужен.
    target_sources(hgs-tray PRIVATE src/KonsoleBackend.cpp)
    # Иконка трея: KStatusNotifierItem здесь, QSystemTrayIcon везде остальное -- тот же
    # принцип, что у Konsole/MacBackend: реализация собирается там, где применима.
    # TrayIcon.h (чистый интерфейс) заголовочный, как и TerminalBackend.h.
    target_sources(hgs-tray PRIVATE src/SniTrayIcon.cpp)
    target_sources(hgs-tray PRIVATE src/LinuxAttentionNotifier.cpp)
    target_sources(hgs-tray PRIVATE src/LinuxApplicationBadge.cpp)
    target_sources(hgs-tray PRIVATE src/LinuxPresenceSource.cpp)
else()
    target_sources(hgs-tray PRIVATE src/QtTrayIcon.cpp)
endif()
if(WIN32)
    # Windows Terminal and console windows (hgs-tray.exe --console-run hosts the
    # latter), tray-icon notifications, the taskbar badge and the swarm worker the
    # tray runs itself (Windows has no user service manager). The resource script
    # carries the icon and version information; the manifest sets UTF-8 as the
    # ANSI code page and long-path support.
    set(HGS_TRAY_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/hgs-zerus.ico")
    configure_file(packaging/windows/hgs-tray.rc.in "${CMAKE_CURRENT_BINARY_DIR}/hgs-tray.rc" @ONLY)
    target_sources(hgs-tray PRIVATE src/WindowsTerminalBackend.cpp src/ConsoleHost.cpp src/WindowsAttentionNotifier.cpp
        src/WindowsApplicationBadge.cpp src/SwarmWorkerSupervisor.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/hgs-tray.rc" packaging/windows/hgs-tray.manifest)
    # A GUI program: no console window opens with it (see attachParentConsole in main.cpp).
    set_target_properties(hgs-tray PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(APPLE)
    # Тот же принцип, что и для Konsole: osascript и Terminal.app существуют только на маке.
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set_target_properties(hgs-tray PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_sources(hgs-tray PRIVATE src/MacBackend.cpp src/MacTrayMenu.mm)
    target_sources(hgs-tray PRIVATE src/MacAttentionNotifier.mm)
    target_sources(hgs-tray PRIVATE src/MacAppVisibility.mm)
    target_sources(hgs-tray PRIVATE src/MacApplicationBadge.mm)
    target_sources(hgs-tray PRIVATE src/MacPresenceSource.cpp)
    set_source_files_properties(src/MacAttentionNotifier.mm src/MacAppVisibility.mm src/MacApplicationBadge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(hgs-tray PRIVATE "-framework AppKit" "-framework UserNotifications" "-framework CoreGraphics")
endif()
target_compile_definitions(hgs-tray PRIVATE HGS_TRAY_VERSION="${PROJECT_VERSION}")
# QLocalServer/QLocalSocket (single-instance guard in main.cpp) live in QtNetwork, not
# QtWidgets — the plan's dependency list missed this; noted in the Task 1 report.
target_link_libraries(hgs-tray PRIVATE Qt6::Widgets Qt6::Network Qt6::WebEngineWidgets Threads::Threads hgs-terminal hgs-markdown hgs-window-layer)
if(LINUX)
    target_link_libraries(hgs-tray PRIVATE Qt6::DBus KF6::StatusNotifierItem KF6::WindowSystem KF6::IdleTime)
endif()

target_link_libraries(hgs-tray PRIVATE hgs-updates)

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

# Regenerate the monochrome window/Linux icon sizes from the same blade geometry.
# The native macOS application icon uses tools/export-macos-icon.sh instead.
# Explicit target only: normal builds use the checked-in PNGs.
add_executable(hgs-export-icons EXCLUDE_FROM_ALL tools/export-icons.cpp src/IconFactory.cpp)
target_include_directories(hgs-export-icons PRIVATE src)
target_link_libraries(hgs-export-icons PRIVATE Qt6::Widgets)
