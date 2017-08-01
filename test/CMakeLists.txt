# Check for dependencies
assert_library_exists(CppUTest CppUTest)
assert_library_exists(CppUTestExt CppUTestExt)
assert_library_exists(pthread pthread)

# Create test executables
add_executable(latency_unit_test LatencyUnitTest.cpp)

# Link to the appropriate dependencies, using mocks when possible
target_link_libraries(latency_unit_test
    CppUTest
    CppUTestExt
    latency
    pthread
)

# Add unit tests for execution with ctest
add_test(latency_unit_test latency_unit_test)
