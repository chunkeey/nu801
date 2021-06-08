project(nu801)

cmake_minimum_required(VERSION 3.8)

if (NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected, default to Release")
    set(CMAKE_BUILD_TYPE "Release")
endif()

add_definitions(-D_GNU_SOURCE)

option(BUILD_STATIC_PROGRAM "Build statically-linked program" OFF)

if (BUILD_STATIC_PROGRAM)
	set(CMAKE_EXE_LINKER_FLAGS "-static")
endif()

add_executable(nu801 nu801.c gpio-utils.c)

install(TARGETS nu801 DESTINATION /usr/sbin)
