# FubarDocumentCompare.Native.dll / libFubarDocumentCompare.Native.so: det stabila C-API:t (fdc_*).
# Ingenting av de interna biblioteken syns utåt.
file(GLOB _src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(fdc SHARED ${_src})
set_target_properties(fdc PROPERTIES OUTPUT_NAME "FubarDocumentCompare.Native")
target_include_directories(fdc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(fdc PRIVATE FDC_BUILD)
target_link_libraries(fdc PRIVATE pc_compare pc_report pc_image pc_docview pc_imgproc opencv_imgproc)
if(WIN32)   # pdfium.dll bredvid binärerna
  add_custom_command(TARGET fdc POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     "${PRINTCMP_PDFIUM_RUNTIME}" $<TARGET_FILE_DIR:fdc>)
endif()
