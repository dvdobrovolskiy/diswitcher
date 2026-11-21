cmake_minimum_required(VERSION 3.20)

project(DiSwitcher C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(icon_gen
    tools/icon_gen.c
)
target_compile_definitions(icon_gen PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(icon_gen PRIVATE user32 gdi32)

set(GENERATED_ICO "${CMAKE_CURRENT_BINARY_DIR}/diswitcher.ico")
add_custom_command(
    OUTPUT "${GENERATED_ICO}"
    COMMAND icon_gen "${GENERATED_ICO}"
    DEPENDS icon_gen
    VERBATIM
)

set(GENERATED_RC "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
set(ICON_PATH "${GENERATED_ICO}")
configure_file(src/app.rc.in "${GENERATED_RC}" @ONLY)

add_custom_target(generate_icon ALL DEPENDS "${GENERATED_ICO}")

add_executable(Diswitcher WIN32
    src/main.c
    third_party/sqlite/sqlite3.c
    "${GENERATED_RC}"
)
add_dependencies(Diswitcher generate_icon)

target_compile_definitions(Diswitcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(Diswitcher PRIVATE third_party/sqlite)

target_link_libraries(Diswitcher PRIVATE user32 shell32 gdi32 comdlg32)

add_custom_command(TARGET Diswitcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/models/language_model.bin" "$<TARGET_FILE_DIR:Diswitcher>/language_model.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models/licenses" "$<TARGET_FILE_DIR:Diswitcher>/model-licenses"
    VERBATIM
)
