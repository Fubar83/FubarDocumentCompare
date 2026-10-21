# Tester (ctest) och felsökningsverktyg. Varje test länkar bara de bibliotek det använder.
add_executable(printcmp_core_tests test_main.cpp test_core.cpp)
target_link_libraries(printcmp_core_tests PRIVATE pc_core)
add_test(NAME core COMMAND printcmp_core_tests)

# C-API:t, bara mot FubarDocumentCompare.Native.dll (som .NET-bindningen) – inte de statiska biblioteken en gång till
add_executable(printcmp_capi_tests test_main.cpp test_capi.cpp)
target_link_libraries(printcmp_capi_tests PRIVATE fdc nlohmann_json::nlohmann_json)   # json bara för att läsa facit
target_compile_definitions(printcmp_capi_tests PRIVATE FDC_TESTDATA="${CMAKE_SOURCE_DIR}/../testdata")
add_test(NAME capi COMMAND printcmp_capi_tests)

# Hela kedjan: testgeneratorn -> analysen -> utvärdering mot facit
add_executable(printcmp_smoke_tests test_main.cpp test_smoke.cpp)
target_link_libraries(printcmp_smoke_tests PRIVATE pc_testgen pc_jobspec pc_eval pc_compare pc_braille pc_image opencv_imgproc)
add_test(NAME smoke COMMAND printcmp_smoke_tests)

add_executable(probe_barcode probe_barcode.cpp)
target_link_libraries(probe_barcode PRIVATE pc_compare)
add_executable(probe_label probe_label.cpp)
target_link_libraries(probe_label PRIVATE pc_compare pc_testgen)

# Arkitekturregler (tredjepart bara i ägande bibliotek, publika rubriker bara mot PUBLIC-beroenden)
add_test(NAME architecture COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/cmake/check_architecture.cmake)

# Bildbehandling på områden: filter, FFT, mönstersökning, mätning
add_executable(printcmp_imgproc_tests test_main.cpp test_imgproc.cpp)
target_link_libraries(printcmp_imgproc_tests PRIVATE pc_imgproc opencv_imgproc)
add_test(NAME imgproc COMMAND printcmp_imgproc_tests)

# Tillståndslösa PDF-tjänster: metadata, rendering (hela sidor och rutor), varianter, överlägg
add_executable(printcmp_docview_tests test_main.cpp test_docview.cpp)
target_link_libraries(printcmp_docview_tests PRIVATE pc_docview pc_pdfwrite pc_core)
target_compile_definitions(printcmp_docview_tests PRIVATE PC_TESTDATA="${CMAKE_SOURCE_DIR}/../testdata")
add_test(NAME docview COMMAND printcmp_docview_tests)

# Tillhandahållna typsnitt: fontfiler, uppsättningar, namnbyte, rendering och breddkontroll
add_executable(printcmp_fonts_tests test_main.cpp test_fonts.cpp)
target_link_libraries(printcmp_fonts_tests PRIVATE pc_docview pc_pdf pc_core)
target_compile_definitions(printcmp_fonts_tests PRIVATE PC_TESTDATA="${CMAKE_SOURCE_DIR}/../testdata")
add_test(NAME fonts COMMAND printcmp_fonts_tests)
