cmake_minimum_required(VERSION 3.16)
project(openvireo-sync-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # The sources are UTF-8; SyncApi.cpp has more sections than an object file holds by default
    add_compile_options(/utf-8 /bigobj)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
endif()

find_package(Drogon CONFIG REQUIRED)
# Ed25519 signatures of account access tokens (JWT). Drogon's TLS already pulls in OpenSSL.
find_package(OpenSSL 1.1.1 REQUIRED)
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
    # DrogonConfig.cmake looks for libpq only when Drogon was built with it: as pg, or as PostgreSQL in vcpkg's port
    if(pg_FOUND OR PostgreSQL_FOUND)
        set(WITH_CLOUD ON)
    else()
        set(WITH_CLOUD OFF)
    endif()
else()
    set(WITH_CLOUD ${SYNC_SERVER_WITH_CLOUD})
endif()
if(WITH_CLOUD AND NOT (pg_FOUND OR PostgreSQL_FOUND))
    message(FATAL_ERROR "SYNC_SERVER_WITH_CLOUD needs a Drogon built with PostgreSQL support")
endif()
message(STATUS "Postgres and S3 storage: ${WITH_CLOUD}")

add_library(sync_server_core STATIC
    src/Config.cpp
    src/DatabaseStore.cpp
    src/Jwt.cpp
    src/Migrate.cpp
    src/Storage.cpp
    src/SyncApi.cpp
    src/UserStore.cpp
    src/Util.cpp
)
target_include_directories(sync_server_core PUBLIC src)
target_link_libraries(sync_server_core PUBLIC Drogon::Drogon OpenSSL::Crypto sync_sqlite3)

if(WITH_CLOUD)
    target_sources(sync_server_core PRIVATE
        src/BlobStore.cpp
        src/Import.cpp
        src/PgDatabaseRepository.cpp
        src/PgUserRepository.cpp
        src/Postgres.cpp
    )
    target_compile_definitions(sync_server_core PUBLIC SYNC_SERVER_WITH_CLOUD)
endif()

add_executable(openvireo-sync-server src/main.cpp)
target_link_libraries(openvireo-sync-server PRIVATE sync_server_core)

# For the self-contained release binaries (Dockerfile.static, binaries-native.yml), which link OpenSSL statically and
# run on systems whose openssl.cnf was written for another OpenSSL
option(SYNC_SERVER_PORTABLE "Ignore the system's openssl.cnf unless OPENSSL_CONF names one" OFF)
if(SYNC_SERVER_PORTABLE)
    target_compile_definitions(openvireo-sync-server PRIVATE SYNC_SERVER_PORTABLE)
endif()

option(BUILD_TESTING "Build the API tests" ON)
if(BUILD_TESTING)
    enable_testing()
    # Plain sockets for the event stream tests (test/TestPlatform.h)
    add_library(sync_test_platform INTERFACE)
    if(WIN32)
        target_link_libraries(sync_test_platform INTERFACE ws2_32)
    endif()

    add_executable(sync_server_test test/ApiTest.cpp)
    target_link_libraries(sync_server_test PRIVATE sync_server_core sync_test_platform)
    add_test(NAME sync_server_test COMMAND sync_server_test)

    # Account access tokens (SYNC_AUTH_MODE both and jwt) against the spec's shared test vectors
    set(SYNC_TEST_VECTORS "${CMAKE_CURRENT_SOURCE_DIR}/test/vectors/test-vectors.json")
    add_executable(sync_server_jwt_test test/JwtTest.cpp)
    target_link_libraries(sync_server_jwt_test PRIVATE sync_server_core sync_test_platform)
    target_compile_definitions(sync_server_jwt_test PRIVATE SYNC_TEST_VECTORS="${SYNC_TEST_VECTORS}")
    add_test(NAME sync_server_jwt_test COMMAND sync_server_jwt_test)

    add_executable(sync_server_jwt_only_test test/JwtTest.cpp)
    target_link_libraries(sync_server_jwt_only_test PRIVATE sync_server_core sync_test_platform)
    target_compile_definitions(sync_server_jwt_only_test PRIVATE SYNC_TEST_VECTORS="${SYNC_TEST_VECTORS}"
                                                                  SYNC_TEST_JWT_ONLY)
    add_test(NAME sync_server_jwt_only_test COMMAND sync_server_jwt_only_test)

    # SYNC_AUTH_MODE both without SYNC_INTERNAL_TOKEN: the internal routes are off
    add_executable(sync_server_jwt_no_internal_test test/JwtTest.cpp)
    target_link_libraries(sync_server_jwt_no_internal_test PRIVATE sync_server_core sync_test_platform)
    target_compile_definitions(sync_server_jwt_no_internal_test PRIVATE SYNC_TEST_VECTORS="${SYNC_TEST_VECTORS}"
                                                                        SYNC_TEST_NO_INTERNAL_TOKEN)
    add_test(NAME sync_server_jwt_no_internal_test COMMAND sync_server_jwt_no_internal_test)

    # Every route and environment variable is documented
    add_test(NAME docs_coverage
             COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -P
                     ${CMAKE_CURRENT_SOURCE_DIR}/test/docs_coverage.cmake)
endif()

install(TARGETS openvireo-sync-server RUNTIME DESTINATION bin)
