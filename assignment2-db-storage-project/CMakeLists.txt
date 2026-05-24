cmake_minimum_required(VERSION 3.15)
project(db_storage_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(db_storage_bench
    src/main.cpp
    src/schema.cpp
    src/record.cpp
    src/page.cpp
    src/storage_engine.cpp
    src/benchmark.cpp
    src/tpch_loader.cpp
)

target_include_directories(db_storage_bench PRIVATE src)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(db_storage_bench PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
