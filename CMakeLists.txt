cmake_minimum_required(VERSION 3.0)
project(mymuduo)

set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAG} -g -std=c++11 -fPIC")

aux_source_directory(${PROJECT_SOURCE_DIR} SRC_LIST)
add_library(mymuduo SHARED ${SRC_LIST})