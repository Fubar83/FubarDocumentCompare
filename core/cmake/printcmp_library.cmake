# printcmp_library(<namn> [PUBLIC deps...] [PRIVATE deps...])
#
# Ett statiskt bibliotek pc_<namn> i libs/<namn>:
#   include/printcmp/<namn>/*.hpp  publikt gränssnitt (det enda andra bibliotek kan inkludera)
#   src/*.cpp, src/*.hpp           implementation och privata rubriker
# PUBLIC = bibliotek som de publika rubrikerna själva använder; PRIVATE = bara implementationen.
function(printcmp_library name)
  cmake_parse_arguments(L "" "" "PUBLIC;PRIVATE" ${ARGN})
  file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
  file(GLOB _hdr CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/printcmp/${name}/*.hpp)
  add_library(pc_${name} STATIC ${_src} ${_hdr})
  add_library(printcmp::${name} ALIAS pc_${name})
  target_include_directories(pc_${name}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_features(pc_${name} PUBLIC cxx_std_20)
  target_link_libraries(pc_${name} PUBLIC ${L_PUBLIC} PRIVATE ${L_PRIVATE})
  set_target_properties(pc_${name} PROPERTIES FOLDER libs)
endfunction()
