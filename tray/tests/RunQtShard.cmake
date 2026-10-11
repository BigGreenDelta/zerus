# Run the QtTest functions of TEST whose name hash falls into SHARD of SHARDS,
# so that a long suite runs as several parallel CTest tests. Hashing the name
# keeps every function in the same shard as other functions are added.
# Usage: cmake -DTEST=<binary> -DSHARD=<0..SHARDS-1> -DSHARDS=<n> -P RunQtShard.cmake

# How a run ended, for the failure message. CMake reports crashes as text
# ("Segmentation fault", "Access violation", "Exit code 0xc0000409") and other
# Windows status codes as large or negative numbers, shown here as hex NTSTATUS.
function(describe_exit status out)
    string(STRIP "${status}" status)
    if(status MATCHES "^-[0-9]+$")
        math(EXPR status "${status} + 0x100000000" OUTPUT_FORMAT HEXADECIMAL)
    elseif(status MATCHES "^[0-9]+$" AND status GREATER 255)
        math(EXPR status "${status}" OUTPUT_FORMAT HEXADECIMAL)
    endif()
    set(${out} "${status}" PARENT_SCOPE)
endfunction()

execute_process(COMMAND "${TEST}" -functions OUTPUT_VARIABLE listing RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    describe_exit("${status}" exit)
    message(FATAL_ERROR "Could not list the test functions of ${TEST} (exit status: ${exit})")
endif()
string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*\\(\\)" functions "${listing}")
set(selected)
foreach(function IN LISTS functions)
    string(REPLACE "()" "" name "${function}")
    string(MD5 hash "${name}")
    string(SUBSTRING "${hash}" 0 6 prefix)
    math(EXPR slot "0x${prefix} % ${SHARDS}")
    if(slot EQUAL SHARD)
        list(APPEND selected "${name}")
    endif()
endforeach()
if(selected)
    execute_process(COMMAND "${TEST}" ${selected} RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        describe_exit("${status}" exit)
        message(FATAL_ERROR "${TEST} shard ${SHARD} of ${SHARDS} failed (exit status: ${exit})")
    endif()
endif()
