# Offline, pinned terminal implementation; see vendor/libvterm/UPSTREAM.md.
enable_language(C)
set(HGS_TERMINAL_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(HGS_VTERM_ROOT "${HGS_TERMINAL_ROOT}/vendor/libvterm")
add_library(hgs-vterm STATIC
    ${HGS_VTERM_ROOT}/src/encoding.c ${HGS_VTERM_ROOT}/src/keyboard.c
    ${HGS_VTERM_ROOT}/src/mouse.c ${HGS_VTERM_ROOT}/src/parser.c
    ${HGS_VTERM_ROOT}/src/pen.c ${HGS_VTERM_ROOT}/src/screen.c
    ${HGS_VTERM_ROOT}/src/state.c ${HGS_VTERM_ROOT}/src/unicode.c
    ${HGS_VTERM_ROOT}/src/vterm.c)
set_target_properties(hgs-vterm PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_include_directories(hgs-vterm PUBLIC ${HGS_VTERM_ROOT}/include PRIVATE ${HGS_VTERM_ROOT}/src)
add_library(hgs-terminal STATIC
    ${HGS_TERMINAL_ROOT}/src/TerminalView.h ${HGS_TERMINAL_ROOT}/src/TerminalView.cpp
    ${HGS_TERMINAL_ROOT}/src/TerminalScreen.h ${HGS_TERMINAL_ROOT}/src/TerminalScreen.cpp
    ${HGS_TERMINAL_ROOT}/src/PtyProcess.h)
# forkpty on Unix, a pseudoconsole (ConPTY) on Windows; one public API.
if(WIN32)
    target_sources(hgs-terminal PRIVATE ${HGS_TERMINAL_ROOT}/src/PtyProcess_win.cpp
        ${HGS_TERMINAL_ROOT}/src/WindowsCommandLine.h)
else()
    target_sources(hgs-terminal PRIVATE ${HGS_TERMINAL_ROOT}/src/PtyProcess.cpp)
endif()
set_target_properties(hgs-terminal PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(hgs-terminal PUBLIC ${HGS_TERMINAL_ROOT}/src)
target_link_libraries(hgs-terminal PUBLIC Qt6::Widgets hgs-vterm Threads::Threads)
if(UNIX AND NOT APPLE)
    find_library(HGS_UTIL_LIBRARY util)
    if(HGS_UTIL_LIBRARY)
        target_link_libraries(hgs-terminal PRIVATE ${HGS_UTIL_LIBRARY})
    endif()
endif()

# The caller enables tests after loading this fragment.
function(hgs_add_terminal_tests)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(test_terminalview ${HGS_TERMINAL_ROOT}/tests/test_terminalview.cpp)
    set_target_properties(test_terminalview PROPERTIES AUTOMOC ON)
    target_link_libraries(test_terminalview PRIVATE hgs-terminal Qt6::Test)
    add_test(NAME test_terminalview COMMAND test_terminalview)
    set_tests_properties(test_terminalview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()
