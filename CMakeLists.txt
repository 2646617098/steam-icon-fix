cmake_minimum_required(VERSION 3.24)
project(SteamIconFix VERSION 1.0.0 LANGUAGES CXX RC)

add_executable(SteamIconFix WIN32
    src/main.cpp
)

target_sources(SteamIconFix PRIVATE resources/app.rc)

target_compile_features(SteamIconFix PRIVATE cxx_std_20)
target_compile_definitions(SteamIconFix PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SteamIconFix PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_options(SteamIconFix PRIVATE /SUBSYSTEM:WINDOWS)
set_property(TARGET SteamIconFix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(SteamIconFix PRIVATE comctl32 shell32 ole32 uuid advapi32 shlwapi)
