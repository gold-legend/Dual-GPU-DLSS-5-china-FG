cmake_minimum_required(VERSION 3.20)
project(nrcheck CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the check must run on a machine with no VC++ runtime installed.
# A missing runtime was the first failure on one of the two reports, and a
# check that needs the thing it might be diagnosing proves nothing.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The DLL that calls NGX. Its FILE NAME must contain "nvngx.dll" - see the
# comment at the top of nrcheck_core.cpp. OUTPUT_NAME gives it that name at
# build time, so no rename step is needed.
add_library(nrcheck_core SHARED src/nrcheck_core.cpp)
set_target_properties(nrcheck_core PROPERTIES OUTPUT_NAME "nvngx.dll_nrcheck")
target_include_directories(nrcheck_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../ext/ngx")
target_link_libraries(nrcheck_core PRIVATE d3d12 dxgi advapi32)

add_executable(nrcheck src/nrcheck_main.cpp)
target_link_libraries(nrcheck PRIVATE dxgi version)

# Independent pixel-contract check; none of this is linked into the game bridge.
add_library(nrmaskcheck_core SHARED src/nrmaskcheck_core.cpp)
set_target_properties(nrmaskcheck_core PROPERTIES OUTPUT_NAME "nvngx.dll_nrmaskcheck")
target_include_directories(nrmaskcheck_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../ext/ngx")
target_link_libraries(nrmaskcheck_core PRIVATE d3d12 dxgi advapi32)
add_executable(nrmaskcheck src/nrmaskcheck_main.cpp)
target_link_libraries(nrmaskcheck PRIVATE dxgi)
add_dependencies(nrmaskcheck nrmaskcheck_core)
