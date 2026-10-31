cmake_minimum_required(VERSION 3.22)
project(ftrez_modelswap LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "ModelSwap requires the Android arm64-v8a toolchain")
endif()

set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/Dobby EXCLUDE_FROM_ALL)

add_library(ModelSwap SHARED
    native_entry.cpp config_install.cpp
    il2cpp/il2cpp_api.cpp il2cpp/method_invoke.cpp
    il2cpp/frame_hook.cpp
    sdk/model_bindings.cpp sdk/own_player.cpp
    settings/native_settings.cpp
    self_model/model_directory.cpp self_model/model_glb.cpp
    self_model/model_format.cpp self_model/model_runtime.cpp
    self_model/model_texture_decode.cpp self_model/lobby_render.cpp
    self_model/lobby_render.S self_model/model_toon.S)
target_compile_features(ModelSwap PRIVATE cxx_std_20)
target_include_directories(ModelSwap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(ModelSwap SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Dobby/include")
target_compile_options(ModelSwap PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-O2;-Wall;-Wextra;-Werror;-fvisibility=hidden>)
target_link_options(ModelSwap PRIVATE
    "-Wl,-z,max-page-size=16384" "-Wl,--exclude-libs,ALL" "-Wl,--no-undefined")
target_link_libraries(ModelSwap PRIVATE dobby log dl)

get_filename_component(MODELSWAP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
find_program(MODELSWAP_PYTHON NAMES python python3 REQUIRED NO_CMAKE_FIND_ROOT_PATH)
add_custom_target(verify_lobby_resources
    COMMAND "${MODELSWAP_PYTHON}" "${MODELSWAP_ROOT}/tools/verify_lobby_resources.py"
    COMMENT "Verifying bundled lobby helper sources, artifacts and API" VERBATIM)
add_dependencies(ModelSwap verify_lobby_resources)

set(LOBBY_CODE "${CMAKE_CURRENT_SOURCE_DIR}/self_model/lobby_model.dll.bytes")
set(LOBBY_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/self_model/lobby_model.bundle")
set(TOON_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/self_model/ftrez-toon.bundle")
foreach(resource IN ITEMS "${LOBBY_CODE}" "${LOBBY_SHADER}" "${TOON_SHADER}")
    if(NOT EXISTS "${resource}")
        message(FATAL_ERROR "Missing embedded model resource: ${resource}")
    endif()
endforeach()
set_source_files_properties(self_model/lobby_render.S PROPERTIES
    COMPILE_DEFINITIONS "FT_LOBBY_CODE_PATH=\"${LOBBY_CODE}\";FT_LOBBY_SHADER_PATH=\"${LOBBY_SHADER}\""
    OBJECT_DEPENDS "${LOBBY_CODE};${LOBBY_SHADER}")
set_source_files_properties(self_model/model_toon.S PROPERTIES
    COMPILE_DEFINITIONS "FT_MODEL_TOON_PATH=\"${TOON_SHADER}\""
    OBJECT_DEPENDS "${TOON_SHADER}")
target_compile_definitions(ModelSwap PRIVATE FT_MODEL_TOON_BUNDLE=1)
