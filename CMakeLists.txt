project(falloc)
cmake_minimum_required(VERSION 2.8)

if(BUILD_SHARED_LIBS)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
endif()

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
endif()

include_directories(.)

add_definitions(-DFALLOC_EXPORTS)

file(GLOB_RECURSE sources_list "falloc/*.hpp" "src/*.cpp")
add_library(${PROJECT_NAME} ${sources_list})

set(CMAKE_SHARED_LINKER_FLAGS ${CMAKE_SHARED_LINKER_FLAGS} "-Wl,--no-undefined")
target_compile_options(${PROJECT_NAME} PUBLIC -fno-exceptions -fno-rtti)
set_source_files_properties(
    src/new_handler.cpp PROPERTIES COMPILE_FLAGS "-fexceptions -frtti")

if (NOT Boost_system_FOUND)
    find_package(Boost COMPONENTS system REQUIRED)
endif()
include_directories(${Boost_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${Boost_LIBRARIES})

target_link_libraries(${PROJECT_NAME} ${CMAKE_THREAD_LIBS_INIT})

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
