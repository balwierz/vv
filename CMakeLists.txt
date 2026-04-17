cmake_minimum_required(VERSION 3.16)
project(parquet_viewer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-O2 -march=x86-64 -ffunction-sections -fdata-sections)

# ── Build mimalloc as a static lib from source ───────────────────────────────
include(FetchContent)
FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG        v2.1.9
    GIT_SHALLOW    TRUE
)
set(MI_BUILD_SHARED      OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT      OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(MI_INSTALL_TOPLEVEL  OFF CACHE BOOL "" FORCE)
# Don't override malloc/free — Arrow uses mi_* prefixed API
set(MI_OVERRIDE          OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mimalloc)

# ── Arrow / Parquet ───────────────────────────────────────────────────────────
find_package(Arrow   REQUIRED)
find_package(Parquet REQUIRED)

# Prefer static targets if present, fall back to shared (system-dependent)
if(TARGET Arrow::arrow_static)
  set(ARROW_TARGET Arrow::arrow_static)
else()
  set(ARROW_TARGET Arrow::arrow_shared)
endif()

if(TARGET Parquet::parquet_static)
  set(PARQUET_TARGET Parquet::parquet_static)
else()
  set(PARQUET_TARGET Parquet::parquet_shared)
endif()

get_target_property(ARROW_INCLUDE_DIRS   ${ARROW_TARGET}   INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(PARQUET_INCLUDE_DIRS ${PARQUET_TARGET} INTERFACE_INCLUDE_DIRECTORIES)

# ── ncurses ───────────────────────────────────────────────────────────────────
find_package(Curses REQUIRED)

# ── Target ────────────────────────────────────────────────────────────────────
add_executable(parquet_viewer main.cpp)

target_include_directories(parquet_viewer PRIVATE
    ${ARROW_INCLUDE_DIRS}
    ${PARQUET_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
)

target_link_libraries(parquet_viewer PRIVATE
    ${PARQUET_TARGET}
    ${ARROW_TARGET}
    mimalloc-static
    ${CURSES_LIBRARIES}
)

target_link_options(parquet_viewer PRIVATE -Wl,--gc-sections)

# Strip in Release
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_custom_command(TARGET parquet_viewer POST_BUILD
      COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:parquet_viewer>
      COMMENT "Stripping binary")
endif()

message(STATUS "Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "Arrow target: ${ARROW_TARGET}")
