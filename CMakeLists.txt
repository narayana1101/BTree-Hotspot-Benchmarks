cmake_minimum_required(VERSION 3.15)
project(index_microbench)

set(CMAKE_CXX_STANDARD 11)
#SET(CMAKE_CXX_COMPILER /opt/local/bin/g++-mp-5)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -g -pthread")
include_directories(/usr/local/include)
include_directories(.)

#include_directories(btree-rtm)
include_directories(BTreeOLC)

add_executable(index_microbench
#        btree-rtm/btree.c
#        btree-rtm/btree.h
        BTreeOLC/BTreeOLC.h
        BTreeOLC/BTreeOLC_child_layout.h
        allocatortracker.h
        index.h
        indexkey.h
        microbench.h
        papi_util.cpp
        util.h
        workload.cpp)
