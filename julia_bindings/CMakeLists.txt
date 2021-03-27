##
# Load Julia dependencies, including external JlCxx
##
cmake_minimum_required(VERSION 3.16)

project(libhalide-julia)

set(JLCXX_VER 0.8.2)
find_package(jlcxx ${JLCXX_VER} QUIET)
if (NOT jlcxx_FOUND)
    include(FetchContent)
    FetchContent_Declare(jlcxx
                         GIT_REPOSITORY https://github.com/JuliaInterop/libcxxwrap-julia.git
                         GIT_TAG v${JLCXX_VER})
    FetchContent_MakeAvailable(jlcxx)
endif ()

# ##
# # Add our sources to this sub-tree.
# ##

add_subdirectory(src)

option(WITH_TEST_JULIA "Build Julia tests" ON)
if (WITH_TESTS AND WITH_TEST_JULIA)
endif ()
