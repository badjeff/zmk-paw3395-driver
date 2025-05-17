zephyr_library()
zephyr_library_sources_ifdef(CONFIG_PAW3395 src/paw3395.c)
zephyr_include_directories(include)
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

add_library(paw3395_lib STATIC IMPORTED)
set_target_properties(paw3395_lib PROPERTIES 
  IMPORTED_LOCATION ${CMAKE_CURRENT_LIST_DIR}/lib/paw3395/libpaw3395.a
)
target_include_directories(paw3395_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_link_libraries(${ZEPHYR_CURRENT_LIBRARY} PUBLIC paw3395_lib)
