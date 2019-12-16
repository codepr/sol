cmake_minimum_required(VERSION 2.8)

project(sol)

OPTION(DEBUG "add debug flags" OFF)

if (DEBUG)
    message(STATUS "Configuring build for debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wunused -Werror -Wno-unused-result
    -std=c11 -O3 -D_DEFAULT_SOURCE -pedantic -lpthread -luuid -lssl -lcrypto \
    -lcrypt -ggdb -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -pg")
else (DEBUG)
    message(STATUS "Configuring build for production")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wunused -Werror -Wextra -Wno-unused-result \
    -std=c11 -O3 -D_DEFAULT_SOURCE -pedantic -lpthread -luuid -lssl -lcrypto -lcrypt")
endif (DEBUG)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

file(GLOB SOURCES src/*.c)
file(GLOB TEST src/hashtable.c src/bst.c src/config.c src/list.c src/trie.c
    src/util.c src/iterator.c tests/*.c)

set(AUTHOR "Andrea Giacomo Baldan")
set(LICENSE "BSD2 license")

# Executable
add_executable(sol ${SOURCES})
add_executable(sol_test ${TEST})

add_custom_command(
    TARGET sol_test
    POST_BUILD
    COMMAND ${CMAKE_BINARY_DIR}/sol_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "\n Running unit tests")

# Tests
add_test(test ./sol_test)
