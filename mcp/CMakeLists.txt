# halo-mcp — C++20 MCP server that bridges agent-cpp specialists to MCP clients.
#
# Phase 0: standalone stdio server with a canned handler. No bus yet.
# Phase 1+: links libagent_cpp, wires tools/call → Runtime::send, adds CVG gate.
# Phase 2+: links LemonadeNexusSDK, enrolls as a mesh peer, advertises tools,
#           accepts remote calls over tunnel-bound HTTPS. See
#           halo-ai-core/docs/mcp-nexus-design.md.

cmake_minimum_required(VERSION 3.20)

find_package(nlohmann_json CONFIG)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(json)
endif()

add_executable(halo_mcp
    src/main.cc
    src/stdio_server.cc
    src/tool_registry.cc
)
target_include_directories(halo_mcp PRIVATE include)
target_link_libraries(halo_mcp PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(halo_mcp PRIVATE cxx_std_20)
target_compile_options(halo_mcp PRIVATE -Wall -Wextra -Wpedantic)

# Phase 1 will add:
#   target_link_libraries(halo_mcp PRIVATE agent_cpp_static)
#   target_include_directories(halo_mcp PRIVATE ${CMAKE_SOURCE_DIR}/include)
#
# Phase 2 will add:
#   find_package(LemonadeNexusSDK REQUIRED)
#   target_link_libraries(halo_mcp PRIVATE lnsdk::LemonadeNexusSDK)

install(TARGETS halo_mcp RUNTIME DESTINATION bin)
