# Arkitekturtest (cmake -P, körs av ctest): reglerna som bygget inte själv kan upprätthålla.
#
#  1. Tredjepartsbibliotek inkluderas bara i biblioteket som äger dem.
#  2. Ingen fil når in i ett annat biblioteks privata src/ (inga relativa vägar ut ur katalogen).
#  3. Ett biblioteks publika rubriker inkluderar bara sitt eget biblioteks och de deklarerade
#     PUBLIC-beroendenas rubriker (läses ur libs/<namn>/CMakeLists.txt).
cmake_minimum_required(VERSION 3.25)
set(root "${CMAKE_CURRENT_LIST_DIR}/..")

# rubrikprefix -> bibliotek som får använda det
set(owner_fpdf pdf)
set(owner_qpdf pdf)
set(owner_ZXing barcode)
set(owner_zlib pdfwrite)
set(owner_tiffio image)
set(owner_utf8proc core)
set(owner_pcre2 core)

set(errors "")
file(GLOB entries RELATIVE "${root}/libs" "${root}/libs/*")
set(libs "")
foreach(e IN LISTS entries)
  if(IS_DIRECTORY "${root}/libs/${e}")
    list(APPEND libs "${e}")
  endif()
endforeach()
foreach(lib IN LISTS libs)
  # PUBLIC-beroenden ur bibliotekets CMakeLists.txt
  file(READ "${root}/libs/${lib}/CMakeLists.txt" cm)
  string(REGEX MATCH "PUBLIC([^)]*)" pub "${cm}")
  string(REGEX REPLACE "PRIVATE.*" "" pub "${pub}")   # bara PUBLIC-listan
  string(REGEX MATCHALL "pc_([a-z]+)" pub_libs "${pub}")
  string(REPLACE "pc_" "" pub_libs "${pub_libs}")

  file(GLOB_RECURSE files "${root}/libs/${lib}/*.hpp" "${root}/libs/${lib}/*.cpp")
  foreach(f IN LISTS files)
    file(STRINGS "${f}" incs REGEX "^#include ")
    file(RELATIVE_PATH rel "${root}" "${f}")
    set(is_public_header FALSE)
    if(rel MATCHES "/include/printcmp/")
      set(is_public_header TRUE)
    endif()
    foreach(line IN LISTS incs)
      # 1. tredjepart
      foreach(prefix fpdf qpdf ZXing zlib tiffio utf8proc pcre2)
        if(line MATCHES "#include <${prefix}")
          if(NOT lib STREQUAL "${owner_${prefix}}")
            list(APPEND errors "${rel}: ${line} (bara i pc_${owner_${prefix}})")
          endif()
        endif()
      endforeach()
      # 2. relativa vägar ut ur katalogen
      if(line MATCHES "#include \"\\.\\./")
        list(APPEND errors "${rel}: ${line} (relativ väg ut ur biblioteket)")
      endif()
      # 3. publika rubriker: bara eget bibliotek och PUBLIC-beroenden
      if(is_public_header AND line MATCHES "#include \"printcmp/([a-z]+)/")
        set(dep "${CMAKE_MATCH_1}")
        if(NOT dep STREQUAL lib)
          list(FIND pub_libs "${dep}" idx)
          if(idx EQUAL -1)
            list(APPEND errors "${rel}: ${line} (pc_${dep} är inte ett PUBLIC-beroende till pc_${lib})")
          endif()
        endif()
      endif()
    endforeach()
  endforeach()
endforeach()

if(errors)
  list(JOIN errors "\n  " msg)
  message(FATAL_ERROR "Arkitekturregler brutna:\n  ${msg}")
endif()
list(LENGTH libs n)
message(STATUS "arkitektur: ${n} bibliotek kontrollerade, inga brott")
