# PDFium as prebuilt binaries (bblanchon/pdfium-binaries, BSD-3/Apache-2.0).
# Pinned to an exact release; bump PRINTCMP_PDFIUM_TAG deliberately.
include(FetchContent)

set(PRINTCMP_PDFIUM_TAG "chromium/8086" CACHE STRING "pdfium-binaries release tag")

if(WIN32)
  set(_pdfium_asset "pdfium-win-x64.tgz")
elseif(APPLE)
  set(_pdfium_asset "pdfium-mac-univ.tgz")
else()
  set(_pdfium_asset "pdfium-linux-x64.tgz")
endif()

FetchContent_Declare(pdfium_bin
  URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PRINTCMP_PDFIUM_TAG}/${_pdfium_asset}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pdfium_bin)

add_library(pdfium SHARED IMPORTED GLOBAL)
target_include_directories(pdfium INTERFACE "${pdfium_bin_SOURCE_DIR}/include")
if(WIN32)
  set_target_properties(pdfium PROPERTIES
    IMPORTED_IMPLIB   "${pdfium_bin_SOURCE_DIR}/lib/pdfium.dll.lib"
    IMPORTED_LOCATION "${pdfium_bin_SOURCE_DIR}/bin/pdfium.dll")
elseif(APPLE)
  set_target_properties(pdfium PROPERTIES IMPORTED_LOCATION "${pdfium_bin_SOURCE_DIR}/lib/libpdfium.dylib")
else()
  set_target_properties(pdfium PROPERTIES IMPORTED_LOCATION "${pdfium_bin_SOURCE_DIR}/lib/libpdfium.so")
endif()
set(PRINTCMP_PDFIUM_RUNTIME "${pdfium_bin_SOURCE_DIR}/bin/pdfium.dll" CACHE INTERNAL "")
