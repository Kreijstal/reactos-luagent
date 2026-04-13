cmake_minimum_required(VERSION 3.16)
project(reactos_luagent C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(LUAGENT_BUILD_TESTS "Build host-side test utilities" ON)
find_package(Python3 COMPONENTS Interpreter)

find_path(LIBUV_INCLUDE_DIR uv.h)
find_library(LIBUV_LIBRARY NAMES uv libuv)
find_path(LUA_INCLUDE_DIR lua.h
    PATH_SUFFIXES lua5.4 lua54 lua5.3 lua53 lua)
find_library(LUA_LIBRARY NAMES lua54 lua5.4 lua53 lua5.3 lua)
find_package(Threads REQUIRED)

if (NOT LIBUV_INCLUDE_DIR OR NOT LIBUV_LIBRARY)
    message(FATAL_ERROR "libuv not found")
endif ()

if (NOT LUA_INCLUDE_DIR OR NOT LUA_LIBRARY)
    message(FATAL_ERROR "Lua not found")
endif ()

add_executable(luagent
    src/main.c
    src/server.c
    src/session.c
    src/diag.c
    src/frame.c
    src/protocol.c
    src/transfer.c
    src/proc.c
    src/portfwd.c
    src/lua_engine.c
    src/bindings.c
    src/util.c
)

target_include_directories(luagent PRIVATE
    ${LIBUV_INCLUDE_DIR}
    ${LUA_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(luagent PRIVATE
    ${LIBUV_LIBRARY}
    ${LUA_LIBRARY}
    Threads::Threads
)

target_compile_definitions(luagent PRIVATE _XOPEN_SOURCE=700)

if (WIN32)
    target_link_libraries(luagent PRIVATE ws2_32 iphlpapi userenv)
endif ()

if (LUAGENT_BUILD_TESTS)
    enable_testing()
    add_executable(host_client tests/host_client/client.c)
    target_include_directories(host_client PRIVATE
        ${LIBUV_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(host_client PRIVATE ${LIBUV_LIBRARY})
    target_link_libraries(host_client PRIVATE Threads::Threads)
    target_compile_definitions(host_client PRIVATE _XOPEN_SOURCE=700)
    if (WIN32)
        target_link_libraries(host_client PRIVATE ws2_32)
    endif ()

    if (Python3_Interpreter_FOUND)
        add_test(NAME luagent_integration
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_integration.py
                $<TARGET_FILE:luagent>
        )
        set_tests_properties(luagent_integration PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        if (UNIX)
            add_test(NAME luagent_client_daemon
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_client_daemon.py
                    $<TARGET_FILE:luagent>
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/client_daemon.py
            )
            set_tests_properties(luagent_client_daemon PROPERTIES
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            )
        endif ()
    endif ()
endif ()
