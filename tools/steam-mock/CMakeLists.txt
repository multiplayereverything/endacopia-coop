cmake_minimum_required(VERSION 3.16)
project(mock_steam_api CXX)
# 32-bit, like the game: configure with -A Win32
add_library(mock_steam_api SHARED mock_steam_api.cpp)
set_target_properties(mock_steam_api PROPERTIES CXX_STANDARD 14)
target_compile_definitions(mock_steam_api PRIVATE NOMINMAX)
target_link_libraries(mock_steam_api PRIVATE ws2_32)
