find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(test_attention test_attention.cpp ../src/AttentionTracker.cpp ../src/FleetState.cpp)
target_include_directories(test_attention PRIVATE ../src)
target_link_libraries(test_attention PRIVATE Qt6::Test Qt6::Core)
add_test(NAME attention COMMAND test_attention)
if(LINUX)
    find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
    add_executable(test_linuxattention test_linuxattention.cpp ../src/LinuxAttentionNotifier.cpp ../src/AttentionNotifier.h)
    target_include_directories(test_linuxattention PRIVATE ../src)
    target_link_libraries(test_linuxattention PRIVATE Qt6::Test Qt6::Core Qt6::DBus)
    add_test(NAME linuxattention COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_linuxattention>)
    add_executable(test_linuxapplicationbadge test_linuxapplicationbadge.cpp ../src/LinuxApplicationBadge.cpp)
    target_include_directories(test_linuxapplicationbadge PRIVATE ../src)
    target_link_libraries(test_linuxapplicationbadge PRIVATE Qt6::Test Qt6::Core Qt6::DBus)
    add_test(NAME linuxapplicationbadge COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_linuxapplicationbadge>)
    add_executable(test_linuxpresence test_linuxpresence.cpp ../src/LinuxPresenceSource.cpp ../src/DesktopPresence.cpp)
    target_include_directories(test_linuxpresence PRIVATE ../src)
    target_link_libraries(test_linuxpresence PRIVATE Qt6::Test Qt6::Core Qt6::Gui Qt6::DBus KF6::IdleTime)
    add_test(NAME linuxpresence COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_linuxpresence>)
    find_package(Qt6 REQUIRED COMPONENTS Qml)
    add_executable(test_kwinwindowlayer test_kwinwindowlayer.cpp)
    target_link_libraries(test_kwinwindowlayer PRIVATE Qt6::Test Qt6::DBus Qt6::Qml hgs-window-layer)
    add_test(NAME kwinwindowlayer COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_kwinwindowlayer>)
    set_tests_properties(kwinwindowlayer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(test_appconfig test_appconfig.cpp ../src/AppConfig.cpp)
target_include_directories(test_appconfig PRIVATE ../src)
target_link_libraries(test_appconfig PRIVATE Qt6::Test Qt6::Core)
add_test(NAME appconfig COMMAND test_appconfig)

add_executable(test_hgsclient test_hgsclient.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_hgsclient PRIVATE ../src)
target_link_libraries(test_hgsclient PRIVATE Qt6::Test Qt6::Core)
add_test(NAME hgsclient COMMAND test_hgsclient)

add_executable(test_readmarksync test_readmarksync.cpp ../src/ReadMarkSync.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_readmarksync PRIVATE ../src)
target_link_libraries(test_readmarksync PRIVATE Qt6::Test Qt6::Core)
add_test(NAME readmarksync COMMAND test_readmarksync)

add_executable(test_processrunner test_processrunner.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_processrunner PRIVATE ../src)
target_link_libraries(test_processrunner PRIVATE Qt6::Test Qt6::Core)
add_test(NAME processrunner COMMAND test_processrunner)

add_executable(test_swarmcontroller test_swarmcontroller.cpp ../src/SwarmController.cpp ../src/SwarmDialog.cpp ../src/SwarmConflictReview.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/FleetState.cpp)
target_include_directories(test_swarmcontroller PRIVATE ../src)
target_link_libraries(test_swarmcontroller PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME swarmcontroller COMMAND test_swarmcontroller)
set_tests_properties(swarmcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_processview test_processview.cpp ../src/ProcessView.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_processview PRIVATE ../src)
target_link_libraries(test_processview PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME processview COMMAND test_processview)
set_tests_properties(processview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_fleetstate test_fleetstate.cpp ../src/FleetState.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_fleetstate PRIVATE ../src)
target_link_libraries(test_fleetstate PRIVATE Qt6::Test Qt6::Core)
add_test(NAME fleetstate COMMAND test_fleetstate)

# QApplication нужен: IconFactory рисует QPainter-ом на QPixmap и читает
# QApplication::palette()/::font() — этого GUILESS/APPLESS ранер не даёт. QTEST_MAIN
# сам создаёт QApplication, раз видит QT_WIDGETS_LIB через Qt6::Widgets. На боксе без
# дисплея (headless сборка/CI) нужна offscreen-платформа — задаём её тут же тестовым
# свойством, чтобы `ctest` работал сам по себе, без переменных окружения снаружи.
add_executable(test_iconfactory test_iconfactory.cpp ../src/IconFactory.cpp ../resources.qrc)
target_include_directories(test_iconfactory PRIVATE ../src)
target_link_libraries(test_iconfactory PRIVATE Qt6::Test Qt6::Widgets Qt6::Core)
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
add_executable(test_terminalbackend test_terminalbackend.cpp ../src/CommandBackend.cpp
               ../src/ClipboardBackend.cpp)
target_include_directories(test_terminalbackend PRIVATE ../src)
target_link_libraries(test_terminalbackend PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
if(LINUX)
    target_sources(test_terminalbackend PRIVATE ../src/KonsoleBackend.cpp)
    target_link_libraries(test_terminalbackend PRIVATE Qt6::DBus)
endif()
add_test(NAME terminalbackend COMMAND test_terminalbackend)
set_tests_properties(terminalbackend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Правила имени и подсказка basename -- единственное в редакторе карты, что не виджет.
# Сам ProjectsDialog здесь не конструируется (QTEST_APPLESS_MAIN, приложения нет вовсе),
# но Qt6::Widgets всё равно нужен: ProjectsDialog.cpp наследует QDialog и тянет виджеты
# на этапе линковки, а HgsClient.cpp -- за projectSet/projectRemove, которые он зовёт.
add_executable(test_projectsdialog test_projectsdialog.cpp ../src/SessionOrganization.cpp ../src/ProjectsDialog.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/DirectoryDialog.cpp ../src/FleetState.cpp)
target_include_directories(test_projectsdialog PRIVATE ../src)
target_link_libraries(test_projectsdialog PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME projectsdialog COMMAND test_projectsdialog)
set_tests_properties(projectsdialog PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)

# Меню трея целиком: дерево QMenu, переиспользование долгоживущих подменю, гейт «не
# трогать неизменившееся содержимое» и сигналы наружу. QApplication нужен, потому что
# QMenu -- виджет; offscreen задаётся тут же, тем же приёмом, что у iconfactory выше,
# чтобы `ctest` работал headless сам по себе. HgsClient.cpp -- ради BoxState/ProjectInfo
# и метаобъекта, который тянет за собой его заголовок; ни одного процесса тест не
# запускает: TrayMenu про hgs и терминалы не знает вовсе, в этом и смысл шва.
add_executable(test_traymenu test_traymenu.cpp ../src/TrayMenu.cpp ../src/FleetState.cpp
               ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_traymenu PRIVATE ../src)
target_link_libraries(test_traymenu PRIVATE Qt6::Test Qt6::Widgets)
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
    add_executable(test_snitrayicon test_snitrayicon.cpp ../src/SniTrayIcon.cpp)
    target_include_directories(test_snitrayicon PRIVATE ../src)
    target_link_libraries(test_snitrayicon PRIVATE Qt6::Test Qt6::Widgets KF6::StatusNotifierItem)
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
    add_executable(test_macpresence test_macpresence.cpp ../src/MacPresenceSource.cpp ../src/DesktopPresence.cpp)
    target_include_directories(test_macpresence PRIVATE ../src)
    target_link_libraries(test_macpresence PRIVATE Qt6::Test Qt6::Core "-framework CoreGraphics" "-framework CoreFoundation")
    add_test(NAME macpresence COMMAND test_macpresence)
    add_executable(test_macapplicationbadge test_macapplicationbadge.mm)
    target_include_directories(test_macapplicationbadge PRIVATE ../src)
    target_compile_options(test_macapplicationbadge PRIVATE -fobjc-arc)
    target_link_libraries(test_macapplicationbadge PRIVATE Qt6::Test Qt6::Core "-framework AppKit")
    add_test(NAME macapplicationbadge COMMAND test_macapplicationbadge)
    add_executable(test_macappvisibility test_macappvisibility.cpp ../src/MacAppVisibility.mm)
    target_include_directories(test_macappvisibility PRIVATE ../src)
    target_link_libraries(test_macappvisibility PRIVATE Qt6::Test Qt6::Widgets "-framework AppKit")
    add_test(NAME macappvisibility COMMAND test_macappvisibility)
    set_tests_properties(macappvisibility PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_macattention test_macattention.mm ../src/AttentionNotifier.h)
    target_include_directories(test_macattention PRIVATE ../src)
    target_compile_options(test_macattention PRIVATE -fobjc-arc)
    target_link_libraries(test_macattention PRIVATE Qt6::Test Qt6::Core "-framework AppKit" "-framework UserNotifications")
    add_test(NAME macattention COMMAND test_macattention)
    add_executable(test_qttrayicon test_qttrayicon.mm ../src/QtTrayIcon.cpp ../src/MacTrayMenu.mm)
    target_include_directories(test_qttrayicon PRIVATE ../src)
    target_link_libraries(test_qttrayicon PRIVATE Qt6::Test Qt6::Widgets "-framework AppKit")
    add_test(NAME qttrayicon COMMAND test_qttrayicon)
    set_tests_properties(qttrayicon PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_macbackend test_macbackend.cpp ../src/MacBackend.cpp)
    target_include_directories(test_macbackend PRIVATE ../src)
    target_link_libraries(test_macbackend PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME macbackend COMMAND test_macbackend)
endif()

# Декоратор «не дёргать иконку без нужды» (см. QuietTrayIcon.h): проверяется на
# подставной внутренней иконке -- что и сколько раз до неё долетело. QMenu здесь только
# источник aboutToShow/aboutToHide, поэтому Qt6::Widgets и offscreen, как у traymenu.
add_executable(test_quiettrayicon test_quiettrayicon.cpp ../src/QuietTrayIcon.cpp)
target_include_directories(test_quiettrayicon PRIVATE ../src)
target_link_libraries(test_quiettrayicon PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME quiettrayicon COMMAND test_quiettrayicon)
set_tests_properties(quiettrayicon PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Сторож главного потока (см. MainThreadWatchdog.h): тест сам и есть «зависший» главный
# поток -- стоит в msleep и смотрит, дёрнули ли колбэк из сторожевого потока. Пороги
# в сотни миллисекунд против секунды простоя: запас на загруженный бокс, а не точность.
add_executable(test_watchdog test_watchdog.cpp ../src/MainThreadWatchdog.cpp)
target_include_directories(test_watchdog PRIVATE ../src)
target_link_libraries(test_watchdog PRIVATE Qt6::Test Qt6::Core Threads::Threads)
add_test(NAME watchdog COMMAND test_watchdog)

add_executable(test_sessionswindow test_sessionswindow.cpp ../src/SessionsWindow.cpp ../src/SessionSelection.cpp ../src/MachinesPage.cpp ../src/SessionSearch.cpp
               ../src/MachineFilter.cpp ../src/DashboardPage.cpp ../src/AccountsPage.cpp ../src/AccountUsageStore.cpp
               ../src/SessionOrganization.cpp ../src/SessionList.cpp ../src/SwarmController.cpp ../src/SwarmDialog.cpp ../src/SwarmConflictReview.cpp
               ../src/ActivityView.cpp ../src/ProcessView.cpp ../src/NativeUiView.cpp ../src/MessageComposer.cpp ../src/ComposerToolbar.cpp ../src/ComposerDraftStore.cpp ../src/QuestionCard.cpp
               ../src/FleetState.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/DirectoryDialog.cpp
               ../src/NewSessionDialog.cpp ../src/ProjectsDialog.cpp ../resources.qrc)
target_include_directories(test_sessionswindow PRIVATE ../src)
target_compile_definitions(test_sessionswindow PRIVATE HGS_TRAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(test_sessionswindow PRIVATE Qt6::Test Qt6::Widgets Qt6::WebEngineWidgets hgs-terminal hgs-markdown hgs-window-layer)
# The longest suite runs as parallel shards of its test functions.
foreach(shard RANGE 9)
    add_test(NAME sessionswindow-${shard} COMMAND ${CMAKE_COMMAND} -DTEST=$<TARGET_FILE:test_sessionswindow>
        -DSHARD=${shard} -DSHARDS=10 -P ${CMAKE_CURRENT_SOURCE_DIR}/RunQtShard.cmake)
    set_tests_properties(sessionswindow-${shard} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

add_executable(test_nativeuiview test_nativeuiview.cpp ../src/NativeUiView.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_nativeuiview PRIVATE ../src)
target_link_libraries(test_nativeuiview PRIVATE Qt6::Test Qt6::Widgets Qt6::WebEngineWidgets)
add_test(NAME nativeuiview COMMAND test_nativeuiview)
set_tests_properties(nativeuiview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu;QT_QUICK_BACKEND=software")

add_executable(test_activityview test_activityview.cpp ../src/ActivityView.cpp)
target_include_directories(test_activityview PRIVATE ../src)
target_link_libraries(test_activityview PRIVATE Qt6::Test Qt6::Widgets hgs-markdown)
add_test(NAME activityview COMMAND test_activityview)
set_tests_properties(activityview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_messagecomposer test_messagecomposer.cpp ../src/MessageComposer.cpp ../src/ComposerToolbar.cpp ../src/ComposerDraftStore.cpp)
target_include_directories(test_messagecomposer PRIVATE ../src)
target_link_libraries(test_messagecomposer PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME messagecomposer COMMAND test_messagecomposer)
set_tests_properties(messagecomposer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_composertoolbar test_composertoolbar.cpp ../src/ComposerToolbar.cpp)
target_include_directories(test_composertoolbar PRIVATE ../src)
target_link_libraries(test_composertoolbar PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME composertoolbar COMMAND test_composertoolbar)
set_tests_properties(composertoolbar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_questioncard test_questioncard.cpp ../src/QuestionCard.cpp ../src/ComposerDraftStore.cpp)
target_include_directories(test_questioncard PRIVATE ../src)
target_link_libraries(test_questioncard PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME questioncard COMMAND test_questioncard)
set_tests_properties(questioncard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_workspace test_workspace.cpp ../src/SessionOrganization.cpp ../src/SessionList.cpp)
target_include_directories(test_workspace PRIVATE ../src)
target_link_libraries(test_workspace PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME workspace COMMAND test_workspace)
set_tests_properties(workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_swarmbutton test_swarmbutton.cpp ../resources.qrc)
target_include_directories(test_swarmbutton PRIVATE ../src)
target_link_libraries(test_swarmbutton PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME swarmbutton COMMAND test_swarmbutton)
set_tests_properties(swarmbutton PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_machinespage test_machinespage.cpp ../src/MachinesPage.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/FleetState.cpp)
target_include_directories(test_machinespage PRIVATE ../src)
target_link_libraries(test_machinespage PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME machinespage COMMAND test_machinespage)
set_tests_properties(machinespage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_sessionsearch test_sessionsearch.cpp ../src/SessionSearch.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/FleetState.cpp)
target_include_directories(test_sessionsearch PRIVATE ../src)
target_link_libraries(test_sessionsearch PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME sessionsearch COMMAND test_sessionsearch)
set_tests_properties(sessionsearch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_dashboard test_dashboard.cpp ../src/DashboardPage.cpp ../src/SessionList.cpp ../src/FleetState.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_dashboard PRIVATE ../src)
target_link_libraries(test_dashboard PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME dashboard COMMAND test_dashboard)
set_tests_properties(dashboard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_paintcache test_paintcache.cpp ../src/DashboardPage.cpp ../src/SessionList.cpp ../src/FleetState.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_paintcache PRIVATE ../src)
target_link_libraries(test_paintcache PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME paintcache COMMAND test_paintcache)
set_tests_properties(paintcache PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_machinefilter test_machinefilter.cpp ../src/MachineFilter.cpp ../src/FleetState.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_machinefilter PRIVATE ../src)
target_link_libraries(test_machinefilter PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME machinefilter COMMAND test_machinefilter)
set_tests_properties(machinefilter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_accountspage test_accountspage.cpp ../src/SessionOrganization.cpp ../src/AccountsPage.cpp ../src/AccountUsageStore.cpp ../src/NewSessionDialog.cpp ../src/DirectoryDialog.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/FleetState.cpp)
target_include_directories(test_accountspage PRIVATE ../src)
target_link_libraries(test_accountspage PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME accountspage COMMAND test_accountspage)
set_tests_properties(accountspage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_accountusage test_accountusage.cpp ../src/AccountUsageStore.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/FleetState.cpp ../src/ComposerToolbar.cpp)
target_include_directories(test_accountusage PRIVATE ../src)
target_link_libraries(test_accountusage PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME accountusage COMMAND test_accountusage)
set_tests_properties(accountusage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_attachmentviewer test_attachmentviewer.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp)
target_include_directories(test_attachmentviewer PRIVATE ../src)
target_link_libraries(test_attachmentviewer PRIVATE Qt6::Test Qt6::Widgets)
add_test(NAME attachmentviewer COMMAND test_attachmentviewer)
set_tests_properties(attachmentviewer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_worktreepanel test_worktreepanel.cpp ../src/HgsClient.cpp ../src/ProcessRunner.cpp ../src/FleetState.cpp
               ../src/NewSessionDialog.cpp ../src/SessionOrganization.cpp ../src/DirectoryDialog.cpp)
target_include_directories(test_worktreepanel PRIVATE ../src)
target_link_libraries(test_worktreepanel PRIVATE Qt6::Test Qt6::Widgets)
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

add_executable(test_desktoppresence test_desktoppresence.cpp ../src/DesktopPresence.cpp)
target_include_directories(test_desktoppresence PRIVATE ../src)
target_link_libraries(test_desktoppresence PRIVATE Qt6::Test Qt6::Core)
add_test(NAME desktoppresence COMMAND test_desktoppresence)
