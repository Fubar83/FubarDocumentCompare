cmake_minimum_required(VERSION 3.25)
project(printcmp VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /EHsc /bigobj /permissive- /W3 /wd4244 /wd4267 /wd4996 /wd4305)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc imgcodecs video calib3d photo features2d)
find_package(ZXing CONFIG REQUIRED)
find_package(qpdf CONFIG REQUIRED)
find_package(unofficial-utf8proc CONFIG REQUIRED)
find_package(pcre2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(TIFF REQUIRED)
find_package(ZLIB REQUIRED)
include(cmake/pdfium.cmake)
include(cmake/printcmp_library.cmake)

add_subdirectory(libs)    # isolerade bibliotek, se libs/CMakeLists.txt för beroendegrafen
add_subdirectory(capi)    # FubarDocumentCompare.Native.dll (C-API, fdc_*)
add_subdirectory(apps)    # printcmp-cli, printcmp-testgen, printcmp-eval

enable_testing()
add_subdirectory(tests)
