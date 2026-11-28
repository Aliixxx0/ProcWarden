cmake_minimum_required(VERSION 3.24)
project(ProcWarden VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# C++ standards from the brief: warnings are errors.
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: OS wrappers, pure logic, IPC contracts.
# Sources are globbed so the tree builds at every phase.
# ---------------------------------------------------------------------------
file(GLOB PW_CORE_SOURCES CONFIGURE_DEPENDS
  src/common/*.cpp
  src/os/*.cpp
  src/core/*.cpp
  src/ipc/*.cpp
)

add_library(pw_core STATIC ${PW_CORE_SOURCES})
target_include_directories(pw_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(pw_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Binaries (added when their sources exist)
# ---------------------------------------------------------------------------
file(GLOB DAEMON_SOURCES CONFIGURE_DEPENDS src/daemon/*.cpp)
if(DAEMON_SOURCES)
  add_executable(procwardend ${DAEMON_SOURCES})
  target_link_libraries(procwardend PRIVATE pw_core)
endif()

file(GLOB CLI_SOURCES CONFIGURE_DEPENDS src/cli/*.cpp)
if(CLI_SOURCES)
  find_package(Curses REQUIRED)
  add_executable(procwarden ${CLI_SOURCES})
  target_include_directories(procwarden PRIVATE ${CURSES_INCLUDE_DIRS})
  target_link_libraries(procwarden PRIVATE pw_core ${CURSES_LIBRARIES})
endif()

file(GLOB SIM_SOURCES CONFIGURE_DEPENDS src/sim/*.cpp)
if(SIM_SOURCES)
  add_executable(pw-sim ${SIM_SOURCES})
  target_link_libraries(pw-sim PRIVATE pw_core)
endif()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/unit/*.cpp)
if(TEST_SOURCES)
  add_executable(pw-tests ${TEST_SOURCES})
  target_link_libraries(pw-tests PRIVATE pw_core)

  add_custom_target(check
    COMMAND ${CMAKE_BINARY_DIR}/bin/pw-tests
    DEPENDS pw-tests
    COMMENT "Run unit tests")
endif()
