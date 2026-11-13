add_executable(PBLauncher WIN32
  "${launcher_src}/App.cpp"
  "${launcher_src}/Engine.cpp" "${launcher_src}/Engine.h"
  "${launcher_src}/PatchIO.cpp" "${launcher_src}/PatchIO.h"
  "${launcher_src}/LauncherError.h" "${launcher_src}/NativeVersion.h"
  "${launcher_src}/Launcher.rc")
add_dependencies(PBLauncher ResourceCheck)
set_target_properties(PBLauncher PROPERTIES
  FOLDER "Cliente"
  VS_GLOBAL_UseOfMfc Static
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client"
  VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PBLauncher>")

target_compile_definitions(PBLauncher PRIVATE LAUNCHER_NATIVE_VERSION=${LAUNCHER_NATIVE_VERSION}L)
target_include_directories(PBLauncher PRIVATE "${webview2_SOURCE_DIR}/build/native/include" "${PROJECT_SOURCE_DIR}")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(wv_arch x64)
else()
  set(wv_arch x86)
endif()
target_link_libraries(PBLauncher PRIVATE launcher_catalog miniz winhttp xmllite shlwapi version
  "${webview2_SOURCE_DIR}/build/native/${wv_arch}/WebView2LoaderStatic.lib")

file(GLOB launcher_images CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/assets/*.bmp" "${PROJECT_SOURCE_DIR}/assets/*.ico" "${PROJECT_SOURCE_DIR}/assets/*.png")
target_sources(PBLauncher PRIVATE ${launcher_images})
source_group("Recursos" FILES "${launcher_src}/Launcher.rc")
source_group("Recursos\\Imagens" FILES ${launcher_images})
launcher_config_files(PBLauncher "${PROJECT_SOURCE_DIR}/engine/config.zpt" "${PROJECT_SOURCE_DIR}/engine/Launcher.svl"
  "${PROJECT_SOURCE_DIR}/engine/Messages.xml" "${PROJECT_SOURCE_DIR}/engine/UI.xml")
