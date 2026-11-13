set(server_dir "${PROJECT_SOURCE_DIR}/server")
add_executable(UpdateServerss "${server_dir}/Server.cpp" "${server_dir}/UpdateServerss.rc" "${server_dir}/res/i3Engine.ico")
target_include_directories(UpdateServerss PRIVATE "${http_source_SOURCE_DIR}" "${server_dir}")
target_link_libraries(UpdateServerss PRIVATE update_protocol)
set_target_properties(UpdateServerss PROPERTIES
  FOLDER "Servidor"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/server"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:UpdateServerss>")

source_group("Recursos" FILES "${server_dir}/UpdateServerss.rc" "${server_dir}/res/i3Engine.ico")
configure_file("${server_dir}/res/i3Engine.ico" "${CMAKE_BINARY_DIR}/server/res/i3Engine.ico" COPYONLY)
launcher_config_files(UpdateServerss "${server_dir}/Config.ini" "${server_dir}/Version.ini")
add_custom_command(TARGET UpdateServerss POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:UpdateServerss>/patch" VERBATIM)
