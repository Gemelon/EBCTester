set(SQLITE_VERSION "3530400")
set(SQLITE_ZIP_URL "https://sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION}.zip")
set(SQLITE_ZIP_SHA3 "628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e")
set(SQLITE_SOURCE_DIR "${CMAKE_BINARY_DIR}/sqlite-amalgamation-${SQLITE_VERSION}")
set(SQLITE_AMALGAMATION "${SQLITE_SOURCE_DIR}/sqlite3.c")
set(SQLITE_HEADER "${SQLITE_SOURCE_DIR}/sqlite3.h")

if(NOT EXISTS "${SQLITE_AMALGAMATION}" OR NOT EXISTS "${SQLITE_HEADER}")
    set(sqlite_zip "${CMAKE_BINARY_DIR}/sqlite-amalgamation-${SQLITE_VERSION}.zip")
    file(DOWNLOAD "${SQLITE_ZIP_URL}" "${sqlite_zip}"
         EXPECTED_HASH "SHA3_256=${SQLITE_ZIP_SHA3}" STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
        file(REMOVE "${sqlite_zip}")
        message(FATAL_ERROR "SQLite ${SQLITE_VERSION} download failed: ${download_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${sqlite_zip}" DESTINATION "${CMAKE_BINARY_DIR}")
    file(REMOVE "${sqlite_zip}")
endif()

idf_component_register(
    SRCS "${SQLITE_AMALGAMATION}" "sqlite_vfs.c" "sqlite_esp_mem.c"
    INCLUDE_DIRS "${SQLITE_SOURCE_DIR}" "include"
    REQUIRES esp_timer heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NDEBUG=1
    SQLITE_OS_OTHER=1
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_WAL=1
    SQLITE_MAX_MMAP_SIZE=0
    SQLITE_TEMP_STORE=3
    SQLITE_DEFAULT_PAGE_SIZE=4096
    SQLITE_DEFAULT_CACHE_SIZE=-64
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DQS=0
    SQLITE_LIKE_DOESNT_MATCH_BLOBS=1
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_SHARED_CACHE=1
    SQLITE_OMIT_PROGRESS_CALLBACK=1
    SQLITE_OMIT_DECLTYPE=1
    SQLITE_OMIT_AUTOINIT=1
    SQLITE_SMALL_STACK=1
    SQLITE_USE_ALLOCA=1
)

set_source_files_properties("${SQLITE_AMALGAMATION}" PROPERTIES
    COMPILE_OPTIONS "-w;-include;${CMAKE_CURRENT_LIST_DIR}/assert_off.h"
)
