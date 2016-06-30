project(falloc)
cmake_minimum_required(VERSION 2.8)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

include_directories(.)

if (NOT Boost_system_FOUND)
    find_package(Boost COMPONENTS system REQUIRED)
    include_directories(${Boost_INCLUDE_DIRS})
    link_directories(${Boost_LIBRARY_DIRS})
endif()

add_definitions(-DFALLOC_EXPORTS)

add_library(${PROJECT_NAME}
    falloc/cache.hpp
    falloc/impexp.hpp
    src/cache.cpp
)

target_link_libraries(${PROJECT_NAME} ${Boost_LIBRARIES})
target_link_libraries(${PROJECT_NAME} ${CMAKE_THREAD_LIBS_INIT})

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_EXTENSIONS OFF)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:falloc> ${PROJECT_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}/$<TARGET_FILE_NAME:falloc>
    VERBATIM
)
