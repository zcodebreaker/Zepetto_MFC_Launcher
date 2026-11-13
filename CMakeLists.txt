cmake_minimum_required(VERSION 3.24)
project(LauncherMFC LANGUAGES C CXX RC)
if(NOT MSVC)
  message(FATAL_ERROR "Use Visual Studio 2022 com C++ e MFC.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")
include(FetchContent)
FetchContent_Declare(webview2 URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg
  URL_HASH SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4 DOWNLOAD_NAME webview2.zip)
FetchContent_Declare(http_source URL https://codeload.github.com/yhirose/cpp-httplib/zip/refs/tags/v0.20.0
  URL_HASH SHA256=0bf6bae6d95c5b0ea34ebd9be81005412009b56785dbca1978872090fd769d36 SOURCE_SUBDIR headers_only)
FetchContent_Declare(miniz URL https://codeload.github.com/richgel999/miniz/zip/refs/tags/3.0.2
  URL_HASH SHA256=1b4fac7144a64d8d26c4642c25bd5b237599071eaf7c12fc6eaff294af31e931)
FetchContent_MakeAvailable(webview2 http_source miniz)
set_property(TARGET miniz PROPERTY FOLDER "Terceiros")

set(LAUNCHER_NATIVE_VERSION 20171214 CACHE STRING "Versão do componente Launcher no Launcher.svl")
set(launcher_src "${PROJECT_SOURCE_DIR}/src")

function(launcher_config_files target)
  foreach(config IN LISTS ARGN)
    get_filename_component(name "${config}" NAME)
    target_sources(${target} PRIVATE "${config}")
    source_group("Configurações" FILES "${config}")
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
      "-Dsrc=${config}" "-Ddst=$<TARGET_FILE_DIR:${target}>/${name}"
      -P "${PROJECT_SOURCE_DIR}/CopyDefault.cmake" VERBATIM)
  endforeach()
endfunction()

add_subdirectory(projects/update_protocol)
add_subdirectory(projects/launcher_catalog)
add_subdirectory(projects/ResourceCheck)
add_subdirectory(projects/PBLauncher)
add_subdirectory(projects/UpdateServerss)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT PBLauncher)

