cmake_minimum_required(VERSION 3.21)
project(agent_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- Core (stable, don't churn) --------------------------------------------
add_library(agent_cpp_core STATIC
    src/runtime.cpp)
target_include_directories(agent_cpp_core PUBLIC include)
target_compile_options(agent_cpp_core PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)  # scribe hash-chain; herald HTTPS; sentinel WSS
target_link_libraries(agent_cpp_core PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

# Header-only HTTP + JSON — specialists use these to talk to the
# rocm-cpp OpenAI-compat endpoint and parse/emit JSON messages.
# Runtime stays dependency-free beyond libc + pthreads after build.
include(FetchContent)
FetchContent_Declare(cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3)
# usearch — single-header ANN index (MIT, Apache-2 dual, zero-dep)
FetchContent_Declare(usearch
    GIT_REPOSITORY https://github.com/unum-cloud/usearch.git
    GIT_TAG        v2.20.1)
set(USEARCH_BUILD_TEST_CPP   OFF CACHE INTERNAL "")
set(USEARCH_BUILD_BENCH_CPP  OFF CACHE INTERNAL "")
set(USEARCH_BUILD_LIB_C      OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(cpp_httplib nlohmann_json usearch)
target_link_libraries(agent_cpp_core PUBLIC
    httplib::httplib nlohmann_json::nlohmann_json usearch::usearch)

# ---- Specialists (snap in, snap out) ---------------------------------------
# Each new specialist adds ONE line here. If yours needs more (deps, flags),
# factor it out into specialists/your_name/CMakeLists.txt and add_subdirectory.
add_library(agent_cpp_specialists STATIC
    specialists/muse.cpp
    specialists/stdout_sink.cpp
    # — core cognition —
    specialists/scribe.cpp
    specialists/cartograph.cpp   # usearch-backed memory (real)
    specialists/forge.cpp
    specialists/warden.cpp
    specialists/planner.cpp
    specialists/sommelier.cpp
    # — voice I/O —
    specialists/echo_ear.cpp
    specialists/echo_mouth.cpp
    # — Discord (read + write) —
    specialists/sentinel.cpp
    specialists/herald.cpp
    # — GitHub (read + write) —
    specialists/quartermaster.cpp
    specialists/magistrate.cpp
    specialists/librarian.cpp
    # — ops / support —
    specialists/anvil.cpp
    specialists/carpenter.cpp
    specialists/gateway.cpp)
target_link_libraries(agent_cpp_specialists PUBLIC agent_cpp_core)
target_compile_options(agent_cpp_specialists PRIVATE -Wall -Wextra)

# ---- Demo binary -----------------------------------------------------------
add_executable(agent_cpp src/main.cpp)
target_link_libraries(agent_cpp PRIVATE agent_cpp_core agent_cpp_specialists)

# ---- Tests (enabled via -DAGENT_CPP_TESTS=ON) ------------------------------
option(AGENT_CPP_TESTS "Build tests" OFF)
if(AGENT_CPP_TESTS)
    enable_testing()
    add_executable(test_runtime tests/test_runtime.cpp)
    target_link_libraries(test_runtime PRIVATE agent_cpp_core agent_cpp_specialists)
    add_test(NAME runtime COMMAND test_runtime)
endif()
