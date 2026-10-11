# Offline, pinned Markdown parser; see vendor/md4c/UPSTREAM.md.
enable_language(C)
set(HGS_MARKDOWN_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(HGS_MD4C_ROOT "${HGS_MARKDOWN_ROOT}/vendor/md4c")
add_library(hgs-md4c STATIC ${HGS_MD4C_ROOT}/src/md4c.c ${HGS_MD4C_ROOT}/src/entity.c)
set_target_properties(hgs-md4c PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_include_directories(hgs-md4c PUBLIC ${HGS_MD4C_ROOT}/src)
add_library(hgs-markdown STATIC
    ${HGS_MARKDOWN_ROOT}/src/MarkdownHtml.h ${HGS_MARKDOWN_ROOT}/src/MarkdownHtml.cpp
    ${HGS_MARKDOWN_ROOT}/src/MarkdownObjects.h ${HGS_MARKDOWN_ROOT}/src/MarkdownObjects.cpp
    ${HGS_MARKDOWN_ROOT}/src/MarkdownText.h ${HGS_MARKDOWN_ROOT}/src/MarkdownText.cpp)
set_target_properties(hgs-markdown PROPERTIES AUTOMOC ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(hgs-markdown PUBLIC ${HGS_MARKDOWN_ROOT}/src)
target_link_libraries(hgs-markdown PUBLIC Qt6::Widgets hgs-md4c)
