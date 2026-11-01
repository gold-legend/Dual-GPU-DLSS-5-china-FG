cmake_minimum_required(VERSION 3.21)
project(present_queue_smoke CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(present_queue_smoke
    main.cpp
    "${ROOT}/src/present_queue.cpp"
)
target_include_directories(present_queue_smoke PRIVATE "${ROOT}/src")
target_compile_definitions(present_queue_smoke PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
)
target_link_libraries(present_queue_smoke PRIVATE dxgi d3d12 user32)
