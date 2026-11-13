add_executable(ResourceCheck WIN32 "${launcher_src}/Maintenance.cpp" "${launcher_src}/NativeVersion.h")
target_link_libraries(ResourceCheck PRIVATE launcher_catalog shell32)
set_target_properties(ResourceCheck PROPERTIES
  FOLDER "Cliente"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:ResourceCheck>")
