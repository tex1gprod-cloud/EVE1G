set(PROJECT_NAME gta5-extended-video-export-config)

# EVE1G-Encoder.exe - opens Voukoder's encoder window (ProRes, DNxHR, x264/x265, NVENC, MKV/MOV/MP4...)
# and saves the choice to preset.json next to it. Put it in plugins\EVE and run it from there.
add_executable(${PROJECT_NAME} WIN32
        "gta5-extended-video-export-config.cpp"
        "../voukoder/VoukoderTypeLib_i.c")

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")

set_target_properties(${PROJECT_NAME} PROPERTIES
        TARGET_NAME_DEBUG "EVE1G-Encoder"
        TARGET_NAME_MINSIZEREL "EVE1G-Encoder"
        TARGET_NAME_RELEASE "EVE1G-Encoder"
        TARGET_NAME_RELWITHDEBINFO "EVE1G-Encoder")

target_compile_definitions(${PROJECT_NAME} PRIVATE "NDEBUG;_CRT_SECURE_NO_WARNINGS;_CRT_NONSTDC_NO_WARNINGS")
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /EHsc /O2)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../voukoder")
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json ole32 user32)
