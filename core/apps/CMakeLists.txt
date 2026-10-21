# Tunna kommandoradsprogram; logiken finns i biblioteken.
function(printcmp_app name dir)
  add_executable(${name} ${dir}/main.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()

printcmp_app(printcmp-cli cli pc_jobspec pc_report pc_doc)
printcmp_app(printcmp-testgen testgen pc_testgen)
printcmp_app(printcmp-eval eval pc_bench)
printcmp_app(printcmp-img img pc_imgproc pc_core opencv_imgcodecs opencv_imgproc)
