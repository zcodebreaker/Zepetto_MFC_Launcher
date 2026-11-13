if(NOT EXISTS "${dst}")
  file(COPY_FILE "${src}" "${dst}")
endif()
