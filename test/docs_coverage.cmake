# Fails when the docs miss something the code has: a route registered in src/SyncApi.cpp that
# docs/openapi.yaml doesn't describe, or an environment variable read in src/Config.cpp that
# docs/configuration.md doesn't list.
#
#   cmake -DSOURCE_DIR=<repo> -P test/docs_coverage.cmake

file(READ "${SOURCE_DIR}/src/SyncApi.cpp" api)
file(READ "${SOURCE_DIR}/docs/openapi.yaml" openapi)
file(READ "${SOURCE_DIR}/src/Config.cpp" config)
file(READ "${SOURCE_DIR}/docs/configuration.md" configuration)

set(missing "")

# "/api/v1/vaults/(.+)" must appear in openapi.yaml as a path starting with "/vaults/{"
string(REGEX MATCHALL "\"/api/v1/[^\"]*\"" routes "${api}")
list(REMOVE_DUPLICATES routes)
foreach(route IN LISTS routes)
    string(REGEX REPLACE "^\"/api/v1|\"$" "" path "${route}")
    string(REPLACE "(.+)" "{" path "${path}")
    string(FIND "${openapi}" "\n  ${path}" found)
    if(found EQUAL -1)
        list(APPEND missing "route ${path} is not in docs/openapi.yaml")
    endif()
endforeach()

string(REGEX MATCHALL "\"SYNC_[A-Z0-9_]+\"" variables "${config}")
list(REMOVE_DUPLICATES variables)
foreach(variable IN LISTS variables)
    string(REPLACE "\"" "" name "${variable}")
    string(FIND "${configuration}" "`${name}`" found)
    if(found EQUAL -1)
        list(APPEND missing "${name} is not in docs/configuration.md")
    endif()
endforeach()

list(LENGTH routes route_count)
list(LENGTH variables variable_count)
if(route_count EQUAL 0 OR variable_count EQUAL 0)
    message(FATAL_ERROR "Found no routes or variables; has the code moved?")
endif()
if(missing)
    list(JOIN missing "\n  " text)
    message(FATAL_ERROR "The docs are missing:\n  ${text}")
endif()
message(STATUS "docs cover ${route_count} routes and ${variable_count} variables")
