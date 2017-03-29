project(falloc)
cmake_minimum_required(VERSION 2.8)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(MSVC)
  if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
    string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
  endif()
elseif(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
endif()

include_directories(.)

add_definitions(-DFALLOC_EXPORTS)

add_library(${PROJECT_NAME}
    falloc/cache.hpp
    falloc/impexp.hpp
    src/cache.cpp
    src/new_handler.cpp
)

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
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
