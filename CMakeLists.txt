# GFOpenIRC host build: the protocol core, the headless CLI and their unit
# tests. NETKIT_DIR is a gfopennetkit checkout (default: next to this one).
# MIT licence, (c) 2026 AmiCradle Team.
cmake_minimum_required(VERSION 3.10)
project(gfopenirc C)
set(CMAKE_C_STANDARD 99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Werror)
endif()

set(NETKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gfopennetkit" CACHE PATH "gfopennetkit checkout")
add_subdirectory(${NETKIT_DIR} netkit)

add_library(gfopenirc_core STATIC src/parse.c src/client.c)
target_include_directories(gfopenirc_core PUBLIC include)
target_link_libraries(gfopenirc_core PUBLIC gfopennetkit)

add_executable(gfopenirc-cli src/cli.c)
target_compile_definitions(gfopenirc-cli PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(gfopenirc-cli gfopenirc_core)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
foreach(t parse client)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} gfopenirc_core)
  target_include_directories(test_${t} PRIVATE ${NETKIT_DIR}/tests)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
if(Python3_FOUND)
  add_test(NAME cli_fake_server
    COMMAND ${Python3_EXECUTABLE} ${NETKIT_DIR}/tests/fakes/fakeserv.py --run irc,socks5 --
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py
            $<TARGET_FILE:gfopenirc-cli>)
endif()
