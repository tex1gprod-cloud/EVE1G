set(PROJECT_NAME gta5-extended-video-export)

################################################################################
# Resource fix
################################################################################
foreach(LANG  C CXX RC)
    set(CMAKE_${LANG}_STANDARD_INCLUDE_DIRECTORIES ${CUSTOM_INCLUDE_DIRECTORIES})
endforeach()

################################################################################
# Source groups
################################################################################
source_group("" FILES ${no_group_source_files})

set(Header_Files
        "config.h"
        "depth_pass.h"
        "hfr_pass.h"
        "passes.h"
        "export_osd.h"
        "re_core.h"
        "encoder.h"
        "logger.h"
        "MFUtility.h"
        "SafeQueue.h"
        "script.h"
        "stdafx.h"
        "targetver.h"
        "util.h"
        "yara-helper.h"
        "yara-patterns.h"
        "hooks.h"
        "hookdefs.h")

set(Source_Files
        "config.cpp"
        "depth_pass.cpp"
        "hfr_pass.cpp"
        "passes.cpp"
        "export_osd.cpp"
        "dllmain.cpp"
        "encoder.cpp"
        "logger.cpp"
        "script.cpp"
        "yara-helper.cpp"
        "resource.rc"
        "util.cpp"
        )

set(VK_Source_Files
        "../voukoder/VoukoderTypeLib_i.c")

source_group("Header Files" FILES ${Header_Files})
source_group("Source Files" FILES ${Source_Files} ${VK_Source_Files})

set(ALL_FILES
        ${no_group_source_files}
        ${Header_Files}
        ${Source_Files}
        ${VK_Source_Files}
        )
################################################################################
# Target
################################################################################
add_library(${PROJECT_NAME} SHARED ${ALL_FILES})

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set(ROOT_NAMESPACE gta5-extended-video-export)

set_target_properties(${PROJECT_NAME} PROPERTIES
        VS_GLOBAL_KEYWORD "Win32Proj"
        )

################################################################################
# Target name
################################################################################
set_target_properties(${PROJECT_NAME} PROPERTIES
        TARGET_NAME_DEBUG "EVE1G"
        TARGET_NAME_MINSIZEREL "EVE1G"
        TARGET_NAME_RELEASE "EVE1G"
        TARGET_NAME_RELWITHDEBINFO "EVE1G"
        SUFFIX ".asi"
        )

# Build HLSL shaders
add_custom_target(shaders)

set(HLSL_SHADER_FILES "shaders/PSAccumulate.hlsl" "shaders/PSDivide.hlsl" "shaders/VSFullScreen.hlsl")

set_source_files_properties("shaders/VSFullScreen.hlsl" PROPERTIES ShaderType "vs")
set_source_files_properties("shaders/PSAccumulate.hlsl" PROPERTIES ShaderType "ps")
set_source_files_properties("shaders/PSDivide.hlsl" PROPERTIES ShaderType "ps")
set_source_files_properties(${HLSL_SHADER_FILES} PROPERTIES ShaderModel "4_0")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")

add_custom_command(TARGET shaders
                   COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders")

foreach(FILE ${HLSL_SHADER_FILES})
  get_filename_component(FILE_WE ${FILE} NAME_WE)
  get_source_file_property(shadertype ${FILE} ShaderType)
  get_source_file_property(shadermodel ${FILE} ShaderModel)
  add_custom_command(TARGET shaders
                     COMMAND fxc.exe /nologo /Emain /T${shadertype}_${shadermodel} $<IF:$<CONFIG:DEBUG>,/Od,/O1> /Fh ${CMAKE_BINARY_DIR}/shaders/${FILE_WE}.h ${FILE}
                     MAIN_DEPENDENCY ${FILE}
                     COMMENT "HLSL ${FILE}"
                     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                     VERBATIM)
endforeach(FILE)

add_dependencies(${PROJECT_NAME} shaders)
################################################################################
# Compile definitions
################################################################################
target_compile_definitions(${PROJECT_NAME} PRIVATE
        "$<$<CONFIG:Debug>:"
        "_DEBUG;"
        "_SCL_SECURE_NO_WARNINGS"
        ">"
        "$<$<CONFIG:MinSizeRel>:"
        "NDEBUG;"
        "_CRT_NONSTDC_NO_WARNINGS"
        ">"
        "$<$<CONFIG:Release>:"
        "NDEBUG;"
        "_CRT_NONSTDC_NO_WARNINGS"
        ">"
        "$<$<CONFIG:RelWithDebInfo>:"
        "_DEBUG;"
        "_SCL_SECURE_NO_WARNINGS"
        ">"
        "_WINDOWS;"
        "_USRDLL;"
        "GTA5EXTENDEDVIDEOEXPORT_EXPORTS;"
        "TARGET_NAME=\"EVE1G\";"
        "_CRT_SECURE_NO_WARNINGS;"
        "_WIN32_WINNT=0x0601"
        )

################################################################################
# Compile and link options
################################################################################
target_link_libraries(${PROJECT_NAME} PRIVATE delayimp)
target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:MinSizeRel>:
        /Y-
        >
        $<$<CONFIG:Release>:
        /Y-
        >
        ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
        ${DEFAULT_CXX_EXCEPTION_HANDLING}
        )
target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:
        /DEBUG:FULL
        >
        $<$<CONFIG:MinSizeRel>:
        >
        $<$<CONFIG:Release>:
        >
        $<$<CONFIG:RelWithDebInfo>:
        /DEBUG:FULL
        >
        /SUBSYSTEM:WINDOWS
        /PDBALTPATH:%_PDB%
        )

################################################################################
# Pre build events
################################################################################
add_custom_command_if(
        TARGET ${PROJECT_NAME}
        PRE_BUILD
        COMMANDS
)
################################################################################
# Post build events - EVE1G: everything is linked statically, so only the config files are copied
################################################################################
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/deploy" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        VERBATIM)

################################################################################
# Dependencies
################################################################################

# Shaders
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/shaders")

# ScriptHookV - EVE1G: no longer needed, the mod runs as a ReShade add-on

# reshade
find_path(RESHADE_INCLUDE_DIR reshade.hpp REQUIRED HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../reshade/include")
target_include_directories(${PROJECT_NAME} PRIVATE ${RESHADE_INCLUDE_DIR})
# Dear ImGui 1.90 headers (only export_osd.cpp: the on-screen export progress; ReShade supplies the functions)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/imgui")

# inih - EVE1G: built from source (inih/), the pinned vcpkg port needs an msys2 download that no longer exists
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../inih/ini.c" "${CMAKE_CURRENT_SOURCE_DIR}/../inih/cpp/INIReader.cpp")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../inih" "${CMAKE_CURRENT_SOURCE_DIR}/../inih/cpp")

# PolyHook2
find_package(PolyHook_2 CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE PolyHook_2::PolyHook_2)

# OpenEXR
find_package(OpenEXR CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenEXR::OpenEXR)

# Voukoder
find_path(VOUKODER_INCLUDE_DIR VoukoderTypeLib_h.h REQUIRED HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../voukoder")
target_include_directories(${PROJECT_NAME} PRIVATE ${VOUKODER_INCLUDE_DIR})

# DirectXTex
find_package(directxtex CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Microsoft::DirectXTex)