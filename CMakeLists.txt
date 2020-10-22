cmake_minimum_required(VERSION 2.8)

project(sol)
set (VERSION 0.17.1)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

OPTION(DEBUG "add debug flags" OFF)

add_definitions("-D_DEFAULT_SOURCE")
find_package(OpenSSL REQUIRED)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

include_directories(include)
file(GLOB SOURCES src/*.c)
file(GLOB TEST src/hashtable.c src/bst.c src/config.c src/list.c src/trie.c
    src/util.c src/iterator.c src/logging.c src/memory.c tests/*.c)

set(AUTHOR "Andrea Giacomo Baldan")
set(LICENSE "BSD2 license")

# Executable
add_executable(sol ${SOURCES} include/uthash.h include/ref.h)
add_executable(sol_test ${TEST})

if (DEBUG)
    message(STATUS "Configuring build for debug")
    TARGET_LINK_LIBRARIES(sol pthread ssl crypto crypt)
    TARGET_LINK_LIBRARIES(sol_test pthread ssl crypto crypt)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wunused -Werror -pedantic \
    -Wno-unused-result -std=c11 -ggdb -fsanitize=address \
    -fsanitize=undefined -fno-omit-frame-pointer -pg")
else (DEBUG)
    message(STATUS "Configuring build for production")
    TARGET_LINK_LIBRARIES(sol pthread ssl crypto crypt)
    TARGET_LINK_LIBRARIES(sol_test pthread ssl crypto crypt)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wunused -Werror -pedantic \
    -Wno-unused-result -std=c11 -O3")
endif (DEBUG)

add_custom_command(
    TARGET sol_test
    POST_BUILD
    COMMAND ${CMAKE_BINARY_DIR}/sol_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "\n Running unit tests")

# Tests
add_test(test ./sol_test)
