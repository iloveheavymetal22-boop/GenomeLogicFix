cmake_minimum_required(VERSION 3.16...3.24)

project(
  Script_GenomeLogicFix
  VERSION 1.1.1.0
  DESCRIPTION "Genome Logic Fix: strict equip requirements and arena judging for Gothic 3"
  LANGUAGES CXX)

# Windows resource compiler: version.rc (VERSIONINFO) is compiled into the DLL by MSVC (rc.exe).
enable_language(RC)

# Use C++ 17 (required by the Gothic 3 SDK).
set(CMAKE_CXX_STANDARD 17)

# Gothic 3 is a 32-bit game, the SDK only supports x86. Configure with:
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
#   cmake --build build --config Release
#
# Path to the Gothic 3 SDK (vendor library style integration, see the SDK README).
set(G3SDK_DIR "G:/Gothic 3 SDK/gothic3sdk-master" CACHE PATH "Path to the Gothic 3 SDK")

if(NOT EXISTS "${G3SDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Gothic 3 SDK not found in '${G3SDK_DIR}'. Set -DG3SDK_DIR=<path to gothic3sdk>.")
endif()

add_subdirectory("${G3SDK_DIR}" "${CMAKE_BINARY_DIR}/gothic3sdk")
include("${G3SDK_DIR}/cmake/Setup.cmake")

# The game loads every Script_*.dll from its 'scripts' folder, so the name has to start with "Script_".
add_library(Script_GenomeLogicFix SHARED)

target_link_libraries(Script_GenomeLogicFix PRIVATE g3sdk::SDK)

# Compiler / linker options required for Script_*.dll plugins (stdcall, static CRT, ...).
target_gothic_script_options(Script_GenomeLogicFix)

target_include_directories(Script_GenomeLogicFix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Optional behaviour switches, see RequirementsCheck.h:
#   RC_PREFER_PLAYER_MEMORY=1   read attribute values from gCPlayerMemory_PS instead of gCStats
#   RC_REQUIRE_GAME_RUNNING=0   also check while gCSession::IsGameRunning() is false
#   RC_MAX_PASSES=64            maximum number of unequip passes per check
# Example: target_compile_definitions(Script_GenomeLogicFix PRIVATE RC_PREFER_PLAYER_MEMORY=1)

target_tree_sources(Script_GenomeLogicFix
  PRIVATE
    RequirementsCheck.h
    RequirementsCheck.cpp
    ArenaJudge.cpp
    FreezeFix.h
    FreezeFix.cpp
    CrashDiag.h
    CrashDiag.cpp
    version.rc
)
