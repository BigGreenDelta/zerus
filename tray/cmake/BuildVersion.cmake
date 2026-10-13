# A Dev base names the next planned stable release; CI supplies its sequence.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../DEV_VERSION" ZERUS_DEV_BASE_VERSION)
string(STRIP "${ZERUS_DEV_BASE_VERSION}" ZERUS_DEV_BASE_VERSION)
if(NOT ZERUS_DEV_BASE_VERSION MATCHES "^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$"
   OR NOT ZERUS_DEV_BASE_VERSION VERSION_GREATER PROJECT_VERSION)
    message(FATAL_ERROR "DEV_VERSION must name a release newer than the stable product version")
endif()
set(ZERUS_DEV_BUILD_NUMBER "$ENV{ZERUS_DEV_BUILD_NUMBER}" CACHE STRING "Verified main CI build sequence")
if(NOT ZERUS_DEV_BUILD_NUMBER STREQUAL "")
    string(LENGTH "${ZERUS_DEV_BUILD_NUMBER}" ZERUS_DEV_NUMBER_LENGTH)
    if(NOT ZERUS_DEV_BUILD_NUMBER MATCHES "^[1-9][0-9]*$" OR ZERUS_DEV_NUMBER_LENGTH GREATER 15)
        message(FATAL_ERROR "ZERUS_DEV_BUILD_NUMBER must be a positive integer of at most 15 digits")
    endif()
    if(NOT ZERUS_CLEAN_SOURCE)
        message(FATAL_ERROR "A numbered Dev build requires a clean Git source tree")
    endif()
    target_compile_definitions(hgs-updates PRIVATE
        ZERUS_DEV_BASE_VERSION="${ZERUS_DEV_BASE_VERSION}"
        ZERUS_DEV_BUILD_NUMBER=${ZERUS_DEV_BUILD_NUMBER})
endif()
