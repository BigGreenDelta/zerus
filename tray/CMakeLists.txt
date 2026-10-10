cmake_minimum_required(VERSION 3.16)

# До project(): иначе CMake зафиксирует minos от SDK хоста и переменная не подхватится.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

project(hgs-tray VERSION 0.37.0 LANGUAGES CXX)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# Чтобы clangd в редакторе знал пути к заголовкам Qt и не краснел на каждый #include.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
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

add_executable(hgs-tray
    resources.qrc
    src/main.cpp
    src/AppConfig.cpp
    src/HgsClient.cpp
    src/ProcessRunner.cpp
    src/FleetState.cpp
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
else()
    target_sources(hgs-tray PRIVATE src/QtTrayIcon.cpp)
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
    set_source_files_properties(src/MacAttentionNotifier.mm src/MacAppVisibility.mm src/MacApplicationBadge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(hgs-tray PRIVATE "-framework AppKit" "-framework UserNotifications")
endif()
target_compile_definitions(hgs-tray PRIVATE HGS_TRAY_VERSION="${PROJECT_VERSION}")
# QLocalServer/QLocalSocket (single-instance guard in main.cpp) live in QtNetwork, not
# QtWidgets — the plan's dependency list missed this; noted in the Task 1 report.
target_link_libraries(hgs-tray PRIVATE Qt6::Widgets Qt6::Network Qt6::WebEngineWidgets Threads::Threads hgs-terminal hgs-markdown hgs-window-layer)
if(LINUX)
    target_link_libraries(hgs-tray PRIVATE Qt6::DBus KF6::StatusNotifierItem KF6::WindowSystem)
endif()

target_link_libraries(hgs-tray PRIVATE hgs-updates)

include(CTest)
if(BUILD_TESTING)
    hgs_add_terminal_tests()
    add_subdirectory(tests)
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
