cmake_minimum_required(VERSION 3.16)
project(keepassxc-sync-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon CONFIG REQUIRED)
# Users, devices and pairing codes in the data directory. Found by hand: Drogon puts its own FindSQLite3.cmake
# on the module path, which hides CMake's and defines other targets.
find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY sqlite3)
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite 3 not found (e.g. apt install libsqlite3-dev)")
endif()
add_library(sync_sqlite3 INTERFACE)
target_include_directories(sync_sqlite3 INTERFACE ${SQLITE3_INCLUDE_DIR})
target_link_libraries(sync_sqlite3 INTERFACE ${SQLITE3_LIBRARY})

# Postgres and S3 storage (SYNC_DATABASE_URL) needs a Drogon built with PostgreSQL; the data directory works
# with any Drogon. AUTO turns it on when Drogon has PostgreSQL.
set(SYNC_SERVER_WITH_CLOUD AUTO CACHE STRING "Build Postgres and S3 storage (ON, OFF or AUTO)")
if(SYNC_SERVER_WITH_CLOUD STREQUAL "AUTO")
    # DrogonConfig.cmake looks for libpq only when Drogon was built with it
    if(pg_FOUND)
        set(WITH_CLOUD ON)
    else()
        set(WITH_CLOUD OFF)
    endif()
else()
    set(WITH_CLOUD ${SYNC_SERVER_WITH_CLOUD})
endif()
if(WITH_CLOUD AND NOT pg_FOUND)
    message(FATAL_ERROR "SYNC_SERVER_WITH_CLOUD needs a Drogon built with PostgreSQL support")
endif()
message(STATUS "Postgres and S3 storage: ${WITH_CLOUD}")

add_library(sync_server_core STATIC
    src/Config.cpp
    src/DatabaseStore.cpp
    src/Storage.cpp
    src/SyncApi.cpp
    src/UserStore.cpp
    src/Util.cpp
)
target_include_directories(sync_server_core PUBLIC src)
target_link_libraries(sync_server_core PUBLIC Drogon::Drogon sync_sqlite3)

if(WITH_CLOUD)
    find_package(OpenSSL REQUIRED)
    target_sources(sync_server_core PRIVATE
        src/BlobStore.cpp
        src/Import.cpp
        src/PgDatabaseRepository.cpp
        src/PgUserRepository.cpp
        src/Postgres.cpp
    )
    target_compile_definitions(sync_server_core PUBLIC SYNC_SERVER_WITH_CLOUD)
    target_link_libraries(sync_server_core PUBLIC OpenSSL::Crypto)
endif()

add_executable(keepassxc-sync-server src/main.cpp)
target_link_libraries(keepassxc-sync-server PRIVATE sync_server_core)

option(BUILD_TESTING "Build the API tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_executable(sync_server_test test/ApiTest.cpp)
    target_link_libraries(sync_server_test PRIVATE sync_server_core)
    add_test(NAME sync_server_test COMMAND sync_server_test)
endif()

install(TARGETS keepassxc-sync-server RUNTIME DESTINATION bin)
