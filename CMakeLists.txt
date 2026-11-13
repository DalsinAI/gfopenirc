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

add_library(gfopenirc_core STATIC src/parse.c src/client.c src/ui_model.c)
target_include_directories(gfopenirc_core PUBLIC include)
target_link_libraries(gfopenirc_core PUBLIC gfopennetkit)

# The chat window needs GFOpenLayout's UI (olui) built for the host: its
# sources from a gfopenlayout checkout (default: next to this one).
set(GFOPENLAYOUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gfopenlayout" CACHE PATH "gfopenlayout checkout")
if(EXISTS ${GFOPENLAYOUT_DIR}/include/gfopenlayout_ui.h)
  set(OL ${GFOPENLAYOUT_DIR})
  add_library(olui_host STATIC
    ${OL}/src/openlayout.c ${OL}/src/gfcss_parse.c ${OL}/src/gfcss_values.c ${OL}/src/gfcss_select.c
    ${OL}/src/openlayout_css.c ${OL}/ui/olui.c ${OL}/ui/olui_css.c ${OL}/ui/olui_markup.c ${OL}/ui/olui_theme_none.c)
  target_include_directories(olui_host PUBLIC ${OL}/include PRIVATE ${OL}/ui ${OL}/src)
  add_library(gfopenirc_ui STATIC src/ui_window.c)
  target_link_libraries(gfopenirc_ui PUBLIC gfopenirc_core olui_host)
  set(HAVE_OLUI 1)
else()
  message(STATUS "gfopenlayout not found at ${GFOPENLAYOUT_DIR}: the chat window and its test are left out")
endif()

add_executable(gfopenirc-cli src/cli.c)
target_compile_definitions(gfopenirc-cli PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(gfopenirc-cli gfopenirc_core)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
foreach(t parse client ui_model)
  add_executable(test_${t} tests/test_${t}.c)
  target_link_libraries(test_${t} gfopenirc_core)
  target_include_directories(test_${t} PRIVATE ${NETKIT_DIR}/tests)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
if(HAVE_OLUI)
  add_executable(test_ui_window tests/test_ui_window.c)
  target_link_libraries(test_ui_window gfopenirc_ui)
  target_include_directories(test_ui_window PRIVATE ${NETKIT_DIR}/tests)
  add_test(NAME ui_window COMMAND test_ui_window)
endif()
if(Python3_FOUND)
  add_test(NAME cli_fake_server
    COMMAND ${Python3_EXECUTABLE} ${NETKIT_DIR}/tests/fakes/fakeserv.py --run irc,socks5 --
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py
            $<TARGET_FILE:gfopenirc-cli>)
endif()
