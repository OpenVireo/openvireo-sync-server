cmake_minimum_required(VERSION 3.16)
project(keepassxc-sync-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon CONFIG REQUIRED)

add_library(sync_server_core STATIC
    src/Config.cpp
    src/DatabaseStore.cpp
    src/SyncApi.cpp
    src/UserStore.cpp
    src/Util.cpp
)
target_include_directories(sync_server_core PUBLIC src)
target_link_libraries(sync_server_core PUBLIC Drogon::Drogon)

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
