set(PROJECT_NAME eve-settings)

# EVE1G-Settings.exe - the settings window (codec presets, fps, motion blur, shutter, capture source).
add_executable(${PROJECT_NAME} WIN32
        "settings.cpp"
        "../inih/ini.c"
        "../inih/cpp/INIReader.cpp"
        "../voukoder/VoukoderTypeLib_i.c")

use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
set_target_properties(${PROJECT_NAME} PROPERTIES
        TARGET_NAME_DEBUG "EVE1G-Settings"
        TARGET_NAME_MINSIZEREL "EVE1G-Settings"
        TARGET_NAME_RELEASE "EVE1G-Settings"
        TARGET_NAME_RELWITHDEBINFO "EVE1G-Settings")

target_compile_definitions(${PROJECT_NAME} PRIVATE "NDEBUG;UNICODE;_UNICODE;_CRT_SECURE_NO_WARNINGS")
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /EHsc /O2 /utf-8)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../inih" "${CMAKE_CURRENT_SOURCE_DIR}/../inih/cpp"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../voukoder")
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json comctl32 shell32 ole32 user32 gdi32)
