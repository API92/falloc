project(falloc)
cmake_minimum_required(VERSION 3.0)

option(FALLOC_STDAPI "Build with malloc, calloc, realloc, aligned_alloc, free." ON)

if(BUILD_SHARED_LIBS)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")

file(GLOB_RECURSE sources_list "falloc/*.hpp" "src/*.cpp")
add_library(${PROJECT_NAME} ${sources_list})

set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
target_compile_options(${PROJECT_NAME} PUBLIC -fno-exceptions -fno-rtti -ftls-model=initial-exec)
set_source_files_properties(src/new_handler.cpp
    PROPERTIES COMPILE_FLAGS "-fexceptions -frtti")
if(FALLOC_STDAPI)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FALLOC_STDAPI)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE .)

if(BUILD_SHARED_LIBS)
    set(CMAKE_SHARED_LINKER_FLAGS ${CMAKE_SHARED_LINKER_FLAGS} "-Wl,--no-undefined")
    target_link_libraries(${PROJECT_NAME} ${CMAKE_THREAD_LIBS_INIT})
endif()

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
