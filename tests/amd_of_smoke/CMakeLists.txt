cmake_minimum_required(VERSION 3.21)
project(amd_of_smoke CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(FFX "${ROOT}/../ext_src/fsr3sdk/FidelityFX-SDK-main/Kits/FidelityFX")
add_executable(amd_of_smoke
    main.cpp
    "${ROOT}/src/amd_of.cpp"
    "${FFX}/api/internal/ffx_assert.cpp"
    "${FFX}/api/internal/ffx_message.cpp"
    "${FFX}/api/internal/ffx_object_management.cpp"
    "${FFX}/backend/dx12/ffx_backends_dx12.cpp"
    "${FFX}/backend/dx12/ffx_dx12.cpp"
    "${FFX}/framegeneration/fsr3/internal/ffx_opticalflow.cpp"
    "${FFX}/framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.cpp"
)
target_include_directories(amd_of_smoke PRIVATE
    "${ROOT}/src"
    "${FFX}/api/include"
    "${FFX}/api/internal"
    "${FFX}/backend/dx12"
    "${FFX}/framegeneration/fsr3/include"
    "${FFX}/framegeneration/fsr3/internal"
    "${ROOT}/generated/ffx_opticalflow"
    "${ROOT}/../external/magpie-experimental/src/Magpie.Core/include"
)
target_compile_definitions(amd_of_smoke PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
)
target_link_libraries(amd_of_smoke PRIVATE dxgi d3d12)
