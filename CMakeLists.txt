cmake_minimum_required(VERSION 3.15)
project(etw_sqlite LANGUAGES CXX)

# Set C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (WIN32)
    add_definitions(-DUNICODE -D_UNICODE)
endif()

add_subdirectory(third_party)

file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")

add_executable(etw_sqlite ${SOURCES} ${HEADERS})
if (MSVC)
    target_compile_options(etw_sqlite PRIVATE /MT)
endif()
 

target_link_libraries(etw_sqlite PRIVATE
    kernel32.lib
    user32.lib
    gdi32.lib
    advapi32.lib
    d3d12.lib
    dxgi.lib
    d3dcompiler.lib
)

target_link_libraries(etw_sqlite PRIVATE third_party_lib)
target_include_directories(etw_sqlite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
