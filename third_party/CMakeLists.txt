cmake_minimum_required(VERSION 3.10)
project(third_party)

file(GLOB_RECURSE THIRD_PARTY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

# Add the third-party library
add_library(third_party_lib STATIC ${THIRD_PARTY_SOURCES})

# Specify include directories for the third-party library
target_include_directories(third_party_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Optionally, set properties for the library
set_target_properties(third_party_lib PROPERTIES OUTPUT_NAME "third_party_lib")

# Suppress warnings for third-party code
if (MSVC)
    target_compile_options(third_party_lib PRIVATE /w /MT)
else()
    target_compile_options(third_party_lib PRIVATE -w)
endif()