find_package(Qt6 REQUIRED COMPONENTS Test)

# support/FakeCli.h starts fixture scripts as fake executables on every platform.
set(ZERUS_TEST_SUPPORT "${CMAKE_CURRENT_SOURCE_DIR}/support")
if(WIN32)
    # Windows cannot start scripts: a native shim runs Python fixtures with their
    # command line unchanged and replaces /bin/sh in process tests (TestHelper.cpp).
    # 3.9: fixtures use str.removeprefix.
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_executable(zerus-test-helper support/TestHelper.cpp)
    # A static runtime: copies of the helper run from temporary directories.
    set_target_properties(zerus-test-helper PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    target_compile_definitions(zerus-test-helper PRIVATE ZERUS_TEST_PYTHON="${Python3_EXECUTABLE}")
    # Isolated INI settings (TestIsolation.cpp), the Windows part of FakeCli.h and
    # the Windows UI font (TestFonts.cpp), compiled once instead of per program.
    # OBJECT, never STATIC: nothing references their startup functions, so an
    # archive would drop them and tests would use the developer's registry
    # (zerus_check_tests() fails the configuration instead).
    add_library(zerus-test-windows OBJECT support/TestIsolation.cpp support/FakeCliWindows.cpp)
    target_include_directories(zerus-test-windows PUBLIC "${ZERUS_TEST_SUPPORT}")
    target_compile_definitions(zerus-test-windows PUBLIC ZERUS_TEST_HELPER="$<TARGET_FILE:zerus-test-helper>")
    target_link_libraries(zerus-test-windows PUBLIC Qt6::Core)
    add_library(zerus-test-fonts OBJECT support/TestFonts.cpp)
    target_link_libraries(zerus-test-fonts PUBLIC Qt6::Gui)
endif()
# Applied to every test program at the end of this file. On Windows each one also
# links the support objects above and gets the manifest of hgs-tray.exe.
function(zerus_test_support target)
    target_include_directories(${target} PRIVATE "${ZERUS_TEST_SUPPORT}")
    if(WIN32)
        target_link_libraries(${target} PRIVATE zerus-test-windows)
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/packaging/windows/hgs-tray.manifest")
        add_dependencies(${target} zerus-test-helper)
        # Programs with Qt GUI get the Windows UI font.
        get_target_property(libraries ${target} LINK_LIBRARIES)
        foreach(library IN LISTS libraries)
            if(library MATCHES "^(Qt6::(Gui|Widgets|WebEngineWidgets)|hgs-(markdown|terminal|window-layer|updates|speech|tray-(gui|ui|activity|views|windows)))$")
                target_link_libraries(${target} PRIVATE zerus-test-fonts)
                break()
            endif()
        endforeach()
    endif()
endfunction()
# Configuration fails when a suite lists a source that hgs-tray, a library or a
# support target already builds (link that target instead; a second copy also
# defines its moc output twice), or when a Windows test program would use the
# registry for its settings.
function(zerus_check_tests)
    set(built)
    set(owners)
    set(tests)
    set(errors)
    foreach(directory IN ITEMS "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            if(target MATCHES "^test_")
                list(APPEND tests ${target})
                continue()
            endif()
            get_target_property(base ${target} SOURCE_DIR)
            get_target_property(sources ${target} SOURCES)
            foreach(source IN LISTS sources)
                if(source MATCHES "\\.(c|cpp|mm|h)$")
                    get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${base}")
                    list(APPEND built "${source}")
                    list(APPEND owners ${target})
                endif()
            endforeach()
        endforeach()
    endforeach()
    foreach(test IN LISTS tests)
        get_target_property(base ${test} SOURCE_DIR)
        get_target_property(sources ${test} SOURCES)
        foreach(source IN LISTS sources)
            get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${base}")
            list(FIND built "${source}" index)
            if(index GREATER -1)
                list(GET owners ${index} owner)
                file(RELATIVE_PATH shown "${PROJECT_SOURCE_DIR}" "${source}")
                get_target_property(type ${owner} TYPE)
                if(type STREQUAL "EXECUTABLE")
                    list(APPEND errors "${test} lists ${shown}, which ${owner} builds: move it into a library both link")
                else()
                    list(APPEND errors "${test} lists ${shown}, which ${owner} builds: link ${owner} instead")
                endif()
            endif()
        endforeach()
    endforeach()
    if(WIN32)
        foreach(library IN ITEMS zerus-test-windows zerus-test-fonts)
            get_target_property(type ${library} TYPE)
            if(NOT type STREQUAL "OBJECT_LIBRARY")
                list(APPEND errors "${library} must be an OBJECT library: an archive drops its startup function")
            endif()
        endforeach()
        get_property(programs DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(program IN LISTS tests programs)
            get_target_property(type ${program} TYPE)
            get_target_property(libraries ${program} LINK_LIBRARIES)
            if(type STREQUAL "EXECUTABLE" AND NOT program STREQUAL "zerus-test-helper"
               AND NOT "zerus-test-windows" IN_LIST libraries)
                list(APPEND errors "${program} must link zerus-test-windows (zerus_test_support)")
            endif()
        endforeach()
    endif()
    if(errors)
        list(JOIN errors "\n" errors)
        message(FATAL_ERROR "${errors}")
    endif()
endfunction()
# Windows test programs find the Qt DLLs through PATH. The offscreen platform
# reads fonts from QT_QPA_FONTDIR there; without it text has no glyphs or size.
function(zerus_test_environment)
    if(NOT WIN32)
        return()
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.22)
        message(FATAL_ERROR "Windows tests need CMake 3.22 or later (ENVIRONMENT_MODIFICATION)")
    endif()
    file(TO_CMAKE_PATH "$ENV{SystemRoot}/Fonts" fonts)
    get_property(tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS tests)
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>" "QT_QPA_FONTDIR=set:${fonts}")
    endforeach()
endfunction()

add_executable(test_speech test_speech.cpp)
target_include_directories(test_speech PRIVATE ../src)
target_link_libraries(test_speech PRIVATE Qt6::Test hgs-speech hgs-external-file hgs-tray-ui)
add_test(NAME speech COMMAND test_speech)
set_tests_properties(speech PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(APPLE)
    # No consent requests or audio capture: compare Qt's read-only status with
    # AVFoundation and verify the production binary imports the static backend.
    qt_add_executable(test_macos_microphone_permissions MANUAL_FINALIZATION
        test_macos_microphone_permissions.mm)
    set_target_properties(test_macos_microphone_permissions PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/microphone-permissions.plist")
    target_include_directories(test_macos_microphone_permissions PRIVATE ../src)
    target_link_libraries(test_macos_microphone_permissions PRIVATE Qt6::Test Qt6::Multimedia "-framework AVFoundation"
        hgs-speech-recorder)
    target_compile_definitions(test_macos_microphone_permissions PRIVATE ZERUS_TRAY_BINARY="$<TARGET_FILE:hgs-tray>")
    add_dependencies(test_macos_microphone_permissions hgs-tray)
    qt_finalize_executable(test_macos_microphone_permissions)
    add_test(NAME macos_microphone_permissions COMMAND test_macos_microphone_permissions)
endif()

add_executable(test_attention test_attention.cpp)
target_include_directories(test_attention PRIVATE ../src)
target_link_libraries(test_attention PRIVATE Qt6::Test Qt6::Core Qt6::Gui hgs-tray-gui)
add_test(NAME attention COMMAND test_attention)
set_tests_properties(attention PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(LINUX)
    find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
    add_executable(test_linuxattention test_linuxattention.cpp)
    target_include_directories(test_linuxattention PRIVATE ../src)
    target_link_libraries(test_linuxattention PRIVATE Qt6::Test Qt6::Core Qt6::DBus hgs-tray-linux)
    add_test(NAME linuxattention COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_linuxattention>)
    add_executable(test_linuxapplicationbadge test_linuxapplicationbadge.cpp)
    target_include_directories(test_linuxapplicationbadge PRIVATE ../src)
    target_link_libraries(test_linuxapplicationbadge PRIVATE Qt6::Test Qt6::Core Qt6::DBus hgs-tray-linux)
    add_test(NAME linuxapplicationbadge COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_linuxapplicationbadge>)
    add_executable(test_linuxpresence test_linuxpresence.cpp)
    target_include_directories(test_linuxpresence PRIVATE ../src)
    target_link_libraries(test_linuxpresence PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::DBus KF6::IdleTime hgs-tray-linux)
    add_test(NAME linuxpresence COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_linuxpresence>)
    find_package(Qt6 REQUIRED COMPONENTS Qml)
    add_executable(test_kwinwindowlayer test_kwinwindowlayer.cpp)
    target_link_libraries(test_kwinwindowlayer PRIVATE Qt6::Test Qt6::DBus Qt6::Qml hgs-window-layer)
    add_test(NAME kwinwindowlayer COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_kwinwindowlayer>)
    set_tests_properties(kwinwindowlayer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(test_appconfig test_appconfig.cpp)
target_include_directories(test_appconfig PRIVATE ../src)
target_link_libraries(test_appconfig PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
add_test(NAME appconfig COMMAND test_appconfig)

add_executable(test_hgsclient test_hgsclient.cpp)
target_include_directories(test_hgsclient PRIVATE ../src)
target_link_libraries(test_hgsclient PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
add_test(NAME hgsclient COMMAND test_hgsclient)

add_executable(test_readmarksync test_readmarksync.cpp)
target_include_directories(test_readmarksync PRIVATE ../src)
target_link_libraries(test_readmarksync PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
add_test(NAME readmarksync COMMAND test_readmarksync)

add_executable(test_processrunner test_processrunner.cpp)
target_include_directories(test_processrunner PRIVATE ../src)
target_link_libraries(test_processrunner PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
add_test(NAME processrunner COMMAND test_processrunner)

add_executable(test_swarmcontroller test_swarmcontroller.cpp)
target_include_directories(test_swarmcontroller PRIVATE ../src)
target_link_libraries(test_swarmcontroller PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME swarmcontroller COMMAND test_swarmcontroller)
set_tests_properties(swarmcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_processview test_processview.cpp)
target_include_directories(test_processview PRIVATE ../src)
target_link_libraries(test_processview PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME processview COMMAND test_processview)
set_tests_properties(processview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_fleetstate test_fleetstate.cpp)
target_include_directories(test_fleetstate PRIVATE ../src)
target_link_libraries(test_fleetstate PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
add_test(NAME fleetstate COMMAND test_fleetstate)

# QApplication нужен: IconFactory рисует QPainter-ом на QPixmap и читает
# QApplication::palette()/::font() — этого GUILESS/APPLESS ранер не даёт. QTEST_MAIN
# сам создаёт QApplication, раз видит QT_WIDGETS_LIB через Qt6::Widgets. На боксе без
# дисплея (headless сборка/CI) нужна offscreen-платформа — задаём её тут же тестовым
# свойством, чтобы `ctest` работал сам по себе, без переменных окружения снаружи.
add_executable(test_iconfactory test_iconfactory.cpp ../resources.qrc)
target_include_directories(test_iconfactory PRIVATE ../src)
target_link_libraries(test_iconfactory PRIVATE Qt6::Test Qt6::Widgets Qt6::Core hgs-tray-ui)
add_test(NAME iconfactory COMMAND test_iconfactory)
set_tests_properties(iconfactory PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Бэкенды терминала: проверяется всё, что не требует ни живого рабочего стола, ни
# D-Bus -- подстановка шаблона в CommandBackend, разбор /proc в KonsoleBackend и выбор
# способа копирования в ClipboardBackend. Открытие вкладки, подъём окна и саму запись в
# буфер так не проверить: там нужны запущенная Konsole и живая сессия рабочего стола --
# и трогать НАСТОЯЩИЙ буфер обмена пользователя из ctest было бы прямым вредительством.
#
# Qt6::Gui -- из-за ClipboardBackend (QGuiApplication::clipboard), и он же превращает
# QTEST_MAIN в QGuiApplication, которому нужна платформа; offscreen задаётся тут же,
# тем же приёмом, что у iconfactory выше, чтобы `ctest` работал headless сам по себе.
add_executable(test_terminalbackend test_terminalbackend.cpp)
target_include_directories(test_terminalbackend PRIVATE ../src)
target_link_libraries(test_terminalbackend PRIVATE Qt6::Test Qt6::Core Qt6::Gui hgs-tray-gui)
if(LINUX)
    target_link_libraries(test_terminalbackend PRIVATE Qt6::DBus hgs-tray-linux)
endif()
add_test(NAME terminalbackend COMMAND test_terminalbackend)
set_tests_properties(terminalbackend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Правила имени и подсказка basename -- единственное в редакторе карты, что не виджет.
# Сам ProjectsDialog здесь не конструируется (QTEST_APPLESS_MAIN, приложения нет вовсе),
# но Qt6::Widgets всё равно нужен: ProjectsDialog.cpp наследует QDialog и тянет виджеты
# на этапе линковки, а HgsClient.cpp -- за projectSet/projectRemove, которые он зовёт.
add_executable(test_projectsdialog test_projectsdialog.cpp)
target_include_directories(test_projectsdialog PRIVATE ../src)
target_link_libraries(test_projectsdialog PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME projectsdialog COMMAND test_projectsdialog)
set_tests_properties(projectsdialog PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# Меню трея целиком: дерево QMenu, переиспользование долгоживущих подменю, гейт «не
# трогать неизменившееся содержимое» и сигналы наружу. QApplication нужен, потому что
# QMenu -- виджет; offscreen задаётся тут же, тем же приёмом, что у iconfactory выше,
# чтобы `ctest` работал headless сам по себе. HgsClient.cpp -- ради BoxState/ProjectInfo
# и метаобъекта, который тянет за собой его заголовок; ни одного процесса тест не
# запускает: TrayMenu про hgs и терминалы не знает вовсе, в этом и смысл шва.
add_executable(test_traymenu test_traymenu.cpp)
target_include_directories(test_traymenu PRIVATE ../src)
target_link_libraries(test_traymenu PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME traymenu COMMAND test_traymenu)
set_tests_properties(traymenu PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Иконка трея на Linux (KStatusNotifierItem): три настройки конструктора, от которых
# зависит, видна ли иконка и открывает ли ЛКМ менеджер -- см. SniTrayIcon.h. Живую панель
# тест НЕ трогает: DBUS_SESSION_BUS_ADDRESS указывает в никуда, KSNI не находит
# StatusNotifierWatcher и ничего не регистрирует -- иначе на время теста в настоящем
# трее пользователя появлялась бы вторая иконка hgs-tray (то же вредительство, что и
# запись в его буфер обмена, см. test_terminalbackend выше). Qt6::Widgets -- QMenu
# внутри KSNI и QTEST_MAIN с QApplication; offscreen -- тем же приёмом, что везде.
if(LINUX)
    add_executable(test_snitrayicon test_snitrayicon.cpp)
    target_include_directories(test_snitrayicon PRIVATE ../src)
    target_link_libraries(test_snitrayicon PRIVATE Qt6::Test Qt6::Widgets KF6::StatusNotifierItem hgs-tray-linux)
    add_test(NAME snitrayicon COMMAND test_snitrayicon)
    set_tests_properties(snitrayicon PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/hgs-tray-test-bus")
endif()

# macOS-бэкенд: тестируются только чистые построители команды (AppleScript для `do script`
# и для `display notification`). Запускать osascript из теста нельзя — это открыло бы окна
# на рабочем столе пользователя, поэтому open() тестом не покрыт.
#
# AppConfig.cpp отсюда убран вместе с режимом tabby (2026-08-28): он линковался ради шва
# «строки режимов из AppConfig::openModes(Mac) == то, что разбирает
# MacBackend::modeFromString», а разбора режимов у MacBackend больше нет — терминальный
# режим на маке один. Сам список мака по-прежнему проверяется дословно в test_appconfig,
# на обеих платформах.
if(APPLE)
    add_executable(test_macpresence test_macpresence.cpp)
    target_include_directories(test_macpresence PRIVATE ../src)
    target_link_libraries(test_macpresence PRIVATE Qt6::Test Qt6::Core "-framework CoreGraphics" "-framework CoreFoundation"
        hgs-tray-mac)
    add_test(NAME macpresence COMMAND test_macpresence)
    add_executable(test_macapplicationbadge test_macapplicationbadge.mm)
    target_include_directories(test_macapplicationbadge PRIVATE ../src)
    target_compile_options(test_macapplicationbadge PRIVATE -fobjc-arc)
    target_link_libraries(test_macapplicationbadge PRIVATE Qt6::Test Qt6::Core "-framework AppKit")
    add_test(NAME macapplicationbadge COMMAND test_macapplicationbadge)
    # The object hgs-tray ships, compiled with ARC.
    add_executable(test_macappvisibility test_macappvisibility.cpp)
    target_include_directories(test_macappvisibility PRIVATE ../src)
    target_link_libraries(test_macappvisibility PRIVATE Qt6::Test Qt6::Widgets "-framework AppKit" hgs-tray-mac)
    add_test(NAME macappvisibility COMMAND test_macappvisibility)
    set_tests_properties(macappvisibility PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_macattention test_macattention.mm)
    target_include_directories(test_macattention PRIVATE ../src)
    target_compile_options(test_macattention PRIVATE -fobjc-arc)
    target_link_libraries(test_macattention PRIVATE Qt6::Test Qt6::Core "-framework AppKit" "-framework UserNotifications"
        hgs-tray-core)
    add_test(NAME macattention COMMAND test_macattention)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME macosbundle COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/../tests/test_macos_bundle.py)
    set_tests_properties(macosbundle PROPERTIES ENVIRONMENT "ZERUS_MACOS_BUNDLE=$<TARGET_BUNDLE_DIR:hgs-tray>")
    add_executable(test_qttrayicon test_qttrayicon.mm)
    target_include_directories(test_qttrayicon PRIVATE ../src)
    target_link_libraries(test_qttrayicon PRIVATE Qt6::Test Qt6::Widgets "-framework AppKit" hgs-tray-mac)
    add_test(NAME qttrayicon COMMAND test_qttrayicon)
    set_tests_properties(qttrayicon PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_macbackend test_macbackend.cpp)
    target_include_directories(test_macbackend PRIVATE ../src)
    target_link_libraries(test_macbackend PRIVATE Qt6::Test Qt6::Widgets hgs-tray-mac)
    add_test(NAME macbackend COMMAND test_macbackend)
endif()

# Декоратор «не дёргать иконку без нужды» (см. QuietTrayIcon.h): проверяется на
# подставной внутренней иконке -- что и сколько раз до неё долетело. QMenu здесь только
# источник aboutToShow/aboutToHide, поэтому Qt6::Widgets и offscreen, как у traymenu.
add_executable(test_quiettrayicon test_quiettrayicon.cpp)
target_include_directories(test_quiettrayicon PRIVATE ../src)
target_link_libraries(test_quiettrayicon PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME quiettrayicon COMMAND test_quiettrayicon)
set_tests_properties(quiettrayicon PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Сторож главного потока (см. MainThreadWatchdog.h): тест сам и есть «зависший» главный
# поток -- стоит в msleep и смотрит, дёрнули ли колбэк из сторожевого потока. Пороги
# в сотни миллисекунд против секунды простоя: запас на загруженный бокс, а не точность.
add_executable(test_watchdog test_watchdog.cpp)
target_include_directories(test_watchdog PRIVATE ../src)
target_link_libraries(test_watchdog PRIVATE Qt6::Test Qt6::Core Threads::Threads hgs-tray-core)
add_test(NAME watchdog COMMAND test_watchdog)

# Service stops quit like the tray menu (TerminationSignals.h); Unix only.
if(NOT WIN32)
    add_executable(test_terminationsignals test_terminationsignals.cpp)
    target_include_directories(test_terminationsignals PRIVATE ../src)
    target_link_libraries(test_terminationsignals PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
    add_test(NAME terminationsignals COMMAND test_terminationsignals)
endif()

add_executable(test_sessionswindow test_sessionswindow.cpp ../resources.qrc)
target_include_directories(test_sessionswindow PRIVATE ../src)
target_compile_definitions(test_sessionswindow PRIVATE HGS_TRAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(test_sessionswindow PRIVATE Qt6::Test Qt6::Widgets Qt6::WebEngineWidgets hgs-terminal hgs-markdown hgs-window-layer hgs-speech hgs-external-file
    hgs-tray-views)
# The longest suite runs as parallel shards of its test functions. COST: see the
# end of this file.
set(shard_costs 40 40 52 26 54 28 35 24 37 125)
foreach(shard RANGE 9)
    add_test(NAME sessionswindow-${shard} COMMAND ${CMAKE_COMMAND} -DTEST=$<TARGET_FILE:test_sessionswindow>
        -DSHARD=${shard} -DSHARDS=10 -P ${CMAKE_CURRENT_SOURCE_DIR}/RunQtShard.cmake)
    set_tests_properties(sessionswindow-${shard} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

add_executable(test_nativeuiview test_nativeuiview.cpp)
target_include_directories(test_nativeuiview PRIVATE ../src)
target_link_libraries(test_nativeuiview PRIVATE Qt6::Test Qt6::Widgets Qt6::WebEngineWidgets hgs-tray-views)
# The DeepSeek native UI needs SSH ControlPersist, which Windows OpenSSH lacks; it
# is not offered from Windows in Phase 1. The program is still built there.
if(NOT WIN32)
    add_test(NAME nativeuiview COMMAND test_nativeuiview)
    set_tests_properties(nativeuiview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu;QT_QUICK_BACKEND=software")
endif()

add_executable(test_activityview test_activityview.cpp)
target_include_directories(test_activityview PRIVATE ../src)
target_link_libraries(test_activityview PRIVATE Qt6::Test Qt6::Widgets Qt6::Multimedia hgs-markdown hgs-tray-activity)
add_test(NAME activityview COMMAND test_activityview)
set_tests_properties(activityview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_messagecomposer test_messagecomposer.cpp)
target_include_directories(test_messagecomposer PRIVATE ../src)
target_link_libraries(test_messagecomposer PRIVATE Qt6::Test Qt6::Widgets hgs-external-file hgs-tray-ui)
add_test(NAME messagecomposer COMMAND test_messagecomposer)
set_tests_properties(messagecomposer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_composertoolbar test_composertoolbar.cpp)
target_include_directories(test_composertoolbar PRIVATE ../src)
target_link_libraries(test_composertoolbar PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME composertoolbar COMMAND test_composertoolbar)
set_tests_properties(composertoolbar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_questioncard test_questioncard.cpp)
target_include_directories(test_questioncard PRIVATE ../src)
target_link_libraries(test_questioncard PRIVATE Qt6::Test Qt6::Widgets hgs-markdown hgs-tray-ui)
add_test(NAME questioncard COMMAND test_questioncard)
set_tests_properties(questioncard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_workspace test_workspace.cpp)
target_include_directories(test_workspace PRIVATE ../src)
target_link_libraries(test_workspace PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME workspace COMMAND test_workspace)
set_tests_properties(workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_swarmbutton test_swarmbutton.cpp ../resources.qrc)
target_include_directories(test_swarmbutton PRIVATE ../src)
target_link_libraries(test_swarmbutton PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME swarmbutton COMMAND test_swarmbutton)
set_tests_properties(swarmbutton PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_machinespage test_machinespage.cpp)
target_include_directories(test_machinespage PRIVATE ../src)
target_link_libraries(test_machinespage PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME machinespage COMMAND test_machinespage)
set_tests_properties(machinespage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_sessionsearch test_sessionsearch.cpp)
target_include_directories(test_sessionsearch PRIVATE ../src)
target_link_libraries(test_sessionsearch PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME sessionsearch COMMAND test_sessionsearch)
set_tests_properties(sessionsearch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_dashboard test_dashboard.cpp)
target_include_directories(test_dashboard PRIVATE ../src)
target_link_libraries(test_dashboard PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME dashboard COMMAND test_dashboard)
set_tests_properties(dashboard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_paintcache test_paintcache.cpp)
target_include_directories(test_paintcache PRIVATE ../src)
target_link_libraries(test_paintcache PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME paintcache COMMAND test_paintcache)
set_tests_properties(paintcache PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_machinefilter test_machinefilter.cpp)
target_include_directories(test_machinefilter PRIVATE ../src)
target_link_libraries(test_machinefilter PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME machinefilter COMMAND test_machinefilter)
set_tests_properties(machinefilter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_accountspage test_accountspage.cpp)
target_include_directories(test_accountspage PRIVATE ../src)
target_link_libraries(test_accountspage PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME accountspage COMMAND test_accountspage)
set_tests_properties(accountspage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_accountusage test_accountusage.cpp)
target_include_directories(test_accountusage PRIVATE ../src)
target_link_libraries(test_accountusage PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME accountusage COMMAND test_accountusage)
set_tests_properties(accountusage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_attachmentviewer test_attachmentviewer.cpp)
target_include_directories(test_attachmentviewer PRIVATE ../src)
target_link_libraries(test_attachmentviewer PRIVATE Qt6::Test Qt6::Widgets hgs-external-file hgs-tray-core)
add_test(NAME attachmentviewer COMMAND test_attachmentviewer)
set_tests_properties(attachmentviewer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_externalfile test_externalfile.cpp)
target_link_libraries(test_externalfile PRIVATE Qt6::Test Qt6::Gui hgs-external-file)
add_test(NAME externalfile COMMAND test_externalfile)
set_tests_properties(externalfile PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_worktreepanel test_worktreepanel.cpp)
target_include_directories(test_worktreepanel PRIVATE ../src)
target_link_libraries(test_worktreepanel PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
add_test(NAME worktreepanel COMMAND test_worktreepanel)
set_tests_properties(worktreepanel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_markdownhtml test_markdownhtml.cpp)
target_link_libraries(test_markdownhtml PRIVATE Qt6::Test hgs-markdown)
add_test(NAME markdownhtml COMMAND test_markdownhtml)
set_tests_properties(markdownhtml PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_markdownobjects test_markdownobjects.cpp)
target_link_libraries(test_markdownobjects PRIVATE Qt6::Test hgs-markdown)
add_test(NAME markdownobjects COMMAND test_markdownobjects)
set_tests_properties(markdownobjects PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_updates test_updates.cpp)
target_link_libraries(test_updates PRIVATE hgs-updates Qt6::Test)
add_test(NAME updates COMMAND test_updates)
set_tests_properties(updates PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_link_libraries(test_sessionswindow PRIVATE hgs-updates)
target_link_libraries(test_traymenu PRIVATE hgs-updates)

add_executable(test_relaysettings test_relaysettings.cpp)
target_include_directories(test_relaysettings PRIVATE ../src)
target_link_libraries(test_relaysettings PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME relaysettings COMMAND test_relaysettings)
set_tests_properties(relaysettings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_desktoppresence test_desktoppresence.cpp)
target_include_directories(test_desktoppresence PRIVATE ../src)
target_link_libraries(test_desktoppresence PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
add_test(NAME desktoppresence COMMAND test_desktoppresence)

add_executable(test_mobiledeliverysettings test_mobiledeliverysettings.cpp)
target_include_directories(test_mobiledeliverysettings PRIVATE ../src)
target_link_libraries(test_mobiledeliverysettings PRIVATE Qt6::Test Qt6::Widgets hgs-tray-core)
add_test(NAME mobiledeliverysettings COMMAND test_mobiledeliverysettings)
set_tests_properties(mobiledeliverysettings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Windows pieces with portable logic: the Windows Terminal command, the console
# host, terminal templates, tray-icon notifications and the swarm worker the
# Windows tray supervises.
add_executable(test_windowsdesktop test_windowsdesktop.cpp)
target_include_directories(test_windowsdesktop PRIVATE ../src)
target_link_libraries(test_windowsdesktop PRIVATE Qt6::Test Qt6::Widgets hgs-tray-windows)
if(WIN32)
    # The console host is hgs-tray.exe itself (--console-run).
    add_dependencies(test_windowsdesktop hgs-tray)
    target_compile_definitions(test_windowsdesktop PRIVATE ZERUS_TRAY_EXE="$<TARGET_FILE:hgs-tray>")
endif()
add_test(NAME windowsdesktop COMMAND test_windowsdesktop)
set_tests_properties(windowsdesktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Long tests start first in a fresh build directory. CTest never replaces a COST
# with its measured times, so a directory where ctest has run (it then has
# Testing/Temporary/CTestCostData.txt) keeps the measured order from its next
# configuration on. Seconds on the Windows runner, the slowest (main runs
# 38299237683 and 38313090634).
if(NOT EXISTS "${CMAKE_BINARY_DIR}/Testing/Temporary/CTestCostData.txt")
    foreach(shard RANGE 9)
        list(GET shard_costs ${shard} cost)
        set_tests_properties(sessionswindow-${shard} PROPERTIES COST ${cost})
    endforeach()
    set_tests_properties(activityview PROPERTIES COST 53)
    set_tests_properties(speech PROPERTIES COST 32)
    set_tests_properties(accountspage PROPERTIES COST 30)
    set_tests_properties(hgsclient paintcache questioncard PROPERTIES COST 20)
    set_tests_properties(processview PROPERTIES COST 13)
    set_tests_properties(messagecomposer windowsdesktop PROPERTIES COST 10)
    set_tests_properties(sessionsearch PROPERTIES COST 9)
endif()

get_property(zerus_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS zerus_test_targets)
    if(target MATCHES "^test_")
        zerus_test_support(${target})
    endif()
endforeach()
# Defined by cmake/Terminal.cmake in the parent directory.
if(TARGET test_terminalview)
    zerus_test_support(test_terminalview)
endif()
zerus_check_tests()
zerus_test_environment()
