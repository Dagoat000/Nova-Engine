# File: CMakeLists.txt
#
# Shaders are compiled at RUNTIME (via D3DCompiler, see Pipeline.cpp), so
# CMake's job regarding shaders is only to make sure the shaders/ folder
# ends up next to the executable - not to invoke a compiler on them.
#
# EXCEPTION: shaders/RaytracingReflections.hlsl is compiled at runtime via
# DXC instead (see Graphics/Raytracing.cpp's file header comment for why -
# DXR needs DXIL, which the legacy D3DCompiler cannot produce). That needs
# dxcompiler.dll + dxil.dll next to the executable at runtime; this file
# tries to copy them from the Windows SDK automatically below. If that
# copy doesn't find them on your machine, ray-traced reflections will fail
# to initialize at startup and the engine logs a warning and falls back to
# IBL-only reflections - it will NOT refuse to run.

cmake_minimum_required(VERSION 3.20)
project(Engine LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "This engine targets Windows (Win32 + DirectX 12) only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release)

# Group source files in the Visual Studio Solution Explorer to mirror the
# actual folder layout instead of one flat list.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
)

add_executable(Engine WIN32 ${ENGINE_SOURCES})

foreach(_source IN ITEMS ${ENGINE_SOURCES})
    get_filename_component(_source_path "${_source}" PATH)
    file(RELATIVE_PATH _source_path_rel "${CMAKE_SOURCE_DIR}/src" "${_source_path}")
    string(REPLACE "/" "\\" _group_path "${_source_path_rel}")
    source_group("${_group_path}" FILES "${_source}")
endforeach()

target_include_directories(Engine PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(Engine PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    $<$<CONFIG:Debug>:_DEBUG>
)

target_link_libraries(Engine PRIVATE
    d3d12.lib
    dxgi.lib
    dxguid.lib
    d3dcompiler.lib
    windowscodecs.lib
    ole32.lib
    dxcompiler.lib
)

# Release: full optimization, no debug-layer overhead (compiled out via
# _DEBUG not being defined - see Device.cpp / Pipeline.cpp).
target_compile_options(Engine PRIVATE
    $<$<CONFIG:Release>:/O2 /Oi /GL>
)
target_link_options(Engine PRIVATE
    $<$<CONFIG:Release>:/LTCG>
)

# Debug: full debug info, no optimization, so the D3D12 debug layer's
# messages line up with readable source.
target_compile_options(Engine PRIVATE
    $<$<CONFIG:Debug>:/Od /Zi>
)

# Copy shaders + assets next to the executable after every build so
# CompileShader's relative "shaders/GBufferVS.hlsl"-style paths resolve
# regardless of Debug/Release output directory.
add_custom_command(TARGET Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:Engine>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:Engine>/assets"
    COMMENT "Copying shaders/ and assets/ next to the executable"
)

# Best-effort: locate dxcompiler.dll/dxil.dll in the Windows SDK's redist
# folder and copy them next to the executable. Different SDK installs lay
# this out slightly differently across versions, so this searches a few
# known locations and simply skips the copy (with a warning) if none match -
# see the file header comment above for what happens at runtime if so.
find_program(ENGINE_DXCOMPILER_DLL
    NAMES dxcompiler.dll
    PATHS
        "$ENV{WindowsSdkDir}Redist/D3D/x64"
        "$ENV{WindowsSdkDir}Redist/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/ucrt/DLLs/x64"
    PATH_SUFFIXES ""
    NO_DEFAULT_PATH
)
find_program(ENGINE_DXIL_DLL
    NAMES dxil.dll
    PATHS
        "$ENV{WindowsSdkDir}Redist/D3D/x64"
    PATH_SUFFIXES ""
    NO_DEFAULT_PATH
)

if (ENGINE_DXCOMPILER_DLL AND ENGINE_DXIL_DLL)
    add_custom_command(TARGET Engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ENGINE_DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:Engine>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ENGINE_DXIL_DLL}" "$<TARGET_FILE_DIR:Engine>"
        COMMENT "Copying dxcompiler.dll and dxil.dll (DXR shader compiler) next to the executable"
    )
else()
    message(WARNING
        "Could not auto-locate dxcompiler.dll/dxil.dll in the Windows SDK. "
        "Ray-traced reflections need both next to Engine.exe at runtime. "
        "Copy them manually from your Windows SDK's Redist/D3D/x64 folder "
        "(or the DirectXShaderCompiler GitHub releases) into the build output "
        "directory if DXR fails to initialize - see README.md.")
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Engine)
