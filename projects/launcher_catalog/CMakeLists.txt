add_library(launcher_catalog "${launcher_src}/Catalog.cpp" "${launcher_src}/Catalog.h")
target_link_libraries(launcher_catalog PUBLIC update_protocol bcrypt)
set_property(TARGET launcher_catalog PROPERTY FOLDER "Bibliotecas")
