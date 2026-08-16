cmake_minimum_required(VERSION 3.20)
project(DeFlawless LANGUAGES CXX)

add_library(DeFlawless SHARED src/DeFlawless.cpp)
target_compile_features(DeFlawless PRIVATE cxx_std_17)
target_compile_definitions(DeFlawless PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if (MINGW)
    target_compile_options(DeFlawless PRIVATE -m32)
    target_link_options(DeFlawless PRIVATE -m32)
endif()
set_target_properties(DeFlawless PROPERTIES
    OUTPUT_NAME "DeFlawless"
    PREFIX ""
    SUFFIX ".asi"
)
