# halo-mcp — C++20 MCP server bridging agent-cpp specialists to MCP clients.
#
# Phase 1: links agent_cpp_core (Runtime, Agent, Message), BusBridge routes
#          tools/call → rt.send(). No specialists registered yet — when the
#          agent-cpp executable exposes the specialist factory publicly,
#          phase 1.5 will register the read-only subset here.
# Phase 2+: LemonadeNexusSDK for mesh enrollment, federated tunnel calls.
#
# Build standalone (from mcp/):
#   cmake -B build -G Ninja
#   cmake --build build
#   ./build/halo_mcp
#
# Or (preferred): included via the parent agent-cpp CMakeLists.txt as a
# subdirectory, which reuses the parent's agent_cpp_core target.

cmake_minimum_required(VERSION 3.21)

# If we're the top-level project (standalone build), bootstrap the parent.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(halo_mcp_standalone LANGUAGES CXX)
    add_subdirectory(${CMAKE_SOURCE_DIR}/.. ${CMAKE_BINARY_DIR}/_agent_cpp)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    # Reuse the parent's already-fetched json target if present.
    if(NOT TARGET nlohmann_json::nlohmann_json)
        include(FetchContent)
        FetchContent_Declare(
            json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG v3.11.3
        )
        FetchContent_MakeAvailable(json)
    endif()
endif()

add_executable(halo_mcp
    src/main.cc
    src/stdio_server.cc
    src/tool_registry.cc
    src/bus_bridge.cc
)
target_include_directories(halo_mcp PRIVATE include)
target_link_libraries(halo_mcp PRIVATE
    agent_cpp_core
    agent_cpp_specialists
    nlohmann_json::nlohmann_json
)
target_compile_features(halo_mcp PRIVATE cxx_std_20)
target_compile_options(halo_mcp PRIVATE -Wall -Wextra -Wpedantic)

# Phase 2 will add:
#   find_package(LemonadeNexusSDK REQUIRED)
#   target_link_libraries(halo_mcp PRIVATE lnsdk::LemonadeNexusSDK)

install(TARGETS halo_mcp RUNTIME DESTINATION bin)
