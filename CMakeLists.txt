cmake_minimum_required(VERSION 3.20)
project(Entanglement
    VERSION 0.1.0
    DESCRIPTION "A new network protocol"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Simulated packet loss (testing only, OFF by default)
option(ENTANGLEMENT_SIMULATE_LOSS "Enable simulated packet loss for testing" OFF)

# Collect sources in src/ (protocol library)
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_library(entanglement_core STATIC ${LIB_SOURCES})
target_include_directories(entanglement_core PUBLIC src include)

if(ENTANGLEMENT_SIMULATE_LOSS)
    target_compile_definitions(entanglement_core PUBLIC ENTANGLEMENT_SIMULATE_LOSS)
endif()

# Platform-specific libraries
if(WIN32)
    target_link_libraries(entanglement_core PUBLIC ws2_32)
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(entanglement_core PUBLIC Threads::Threads)
    # _GNU_SOURCE needed for recvmmsg / mmsghdr on Linux
    if(NOT APPLE)
        target_compile_definitions(entanglement_core PUBLIC _GNU_SOURCE)
    endif()
endif()

# Executables
add_executable(EntanglementServer apps/server_main.cpp)
target_link_libraries(EntanglementServer PRIVATE entanglement_core)

add_executable(EntanglementClient apps/client_main.cpp)
target_link_libraries(EntanglementClient PRIVATE entanglement_core)

add_executable(EntanglementTests apps/test_main.cpp)
target_link_libraries(EntanglementTests PRIVATE entanglement_core)

add_executable(EntanglementChurnTest apps/churn_test_main.cpp)
target_link_libraries(EntanglementChurnTest PRIVATE entanglement_core)

add_executable(EntanglementStress apps/stress_test_main.cpp)
target_link_libraries(EntanglementStress PRIVATE entanglement_core)

add_executable(EntanglementNetBench apps/net_bench.cpp)
target_link_libraries(EntanglementNetBench PRIVATE entanglement_core)

# Create log directory at build time
add_custom_target(create_log_dir ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/log"
    COMMENT "Creating log directory"
)
