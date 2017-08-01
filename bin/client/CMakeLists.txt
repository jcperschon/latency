# Create latency client executable
add_executable(latency_client latency_client.cpp)
target_link_libraries(latency_client
    latency
)

# Install latency client executable
install(TARGETS latency_client DESTINATION bin)
