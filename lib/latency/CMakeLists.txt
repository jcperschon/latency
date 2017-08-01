# Check for dependencies
assert_library_exists(pthread pthread)
assert_library_exists(rte rte)

# Create latency library
add_definitions(-fPIC)
file(GLOB_RECURSE _SRCS src/*.c)
file(GLOB_RECURSE _HDRS include/*.h)
add_library(latency SHARED ${_SRCS} ${_HDRS})
target_link_libraries(latency
    pthread
    rte
)

target_include_directories(latency PUBLIC
    src
    include
)

# Install latency library and headers
install(TARGETS latency DESTINATION lib)
install(FILES ${_HDRS} DESTINATION include/latency)
