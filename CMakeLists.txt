# Add project information
cmake_minimum_required(VERSION 3.5)
project(latency)

# Add CTest and Valgrind for executing unit tests
find_program(MEMORYCHECK_COMMAND valgrind)
set(MEMORYCHECK_COMMAND_OPTIONS
    "--leak-check=full --trace-children=yes --error-exitcode=1")
include(CTest)

# Project configuration
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

# Function for dependency checking
function(assert_library_exists LIB)
    set(_LIB_NAMES ${ARGV})
    list(REMOVE_AT _LIB_NAMES "0")
    find_library(_LIB NAMES ${_LIB_NAMES})
    if (_LIB)
        unset(_LIB CACHE)
    else()
        message(FATAL_ERROR "Unable to find library - ${LIB}")
    endif()
endfunction()

# Add lib source directory
add_subdirectory(lib)

# Add test source directory
add_subdirectory(test)

# Add bin source directory
add_subdirectory(bin)
