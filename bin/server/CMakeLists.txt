# Create latency server executable
add_executable(latency_server latency_server.cpp)
target_link_libraries(latency_server
    latency
)

# Install latency server executable
install(TARGETS latency_server DESTINATION bin)
