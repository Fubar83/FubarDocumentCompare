# Hämtar verkliga tryckfiler för testet "realworld" till DEST (byggkatalogen) – de checkas aldrig in:
# filerna saknar villkor för vidarespridning. Kontrolleras mot SHA-256; redan hämtade filer hämtas inte igen.
#
#   cmake -D DEST=<katalog> -P fetch.cmake
#
# Källor (offentliga, kostnadsfria nedladdningar):
#   Ghent Workgroup (gwg.org): Processing Steps Test Suite 1.0 (ISO 19593-lager för förpackningar) och
#     Ghent PDF Output Suite 5.0, testsidorna (PDF/X-4, dekorfärger, överprint).
#   Avery (avery.com): mallar för etiketter på rulle, med DIELINE/BLEEDLINE/SAFELINE som dekorfärger.
if(NOT DEST)
  message(FATAL_ERROR "ange -D DEST=<katalog>")
endif()

set(files
  "gwg-steps|https://gwg.org/wp-content/uploads/2026/09/Processing-Steps-Test-Suite-V1.0-1.zip|49a2da6c37a0f433596be826d9fe9190f69033023395589de824d89b6ed7e34b|zip"
  "gwg-output|https://gwg.org/wp-content/uploads/2026/05/Ghent_PDF_Output_Suite_V50_Testpages.zip|52d271ddd97dcb5778d1da2bdb3b86a66b96083b7f57ee3fd8ed2b5e5a5d7d29|zip"
  "avery|https://www.avery.com/custom-printing/resources/templates/download/roll/AveryWePrint1.000x1.000RoundLabelsonRolls.pdf|15b0ad1aa1f991d0d1760ec32cacc98473a247f96108385f8ef7afa2aae6a8b8|pdf"
  "avery|https://www.avery.com/custom-printing/resources/templates/download/roll/AveryWePrint1.000x1.000SquareLabelsonRolls.pdf|62e6e3a3c558d8d9f42bef20d1ebe5e3378b30d0bdaf6421445b0b69ab107e70|pdf"
  "avery|https://www.avery.com/custom-printing/resources/templates/download/roll/AveryWePrint1.000x2.000OvalLabelsonRolls.pdf|31706f8b15941869ec4ca28fcb4f9fbc0ee5122415785c3f3e5454a13b4b4aed|pdf"
  "avery|https://www.avery.com/custom-printing/resources/templates/download/roll/AveryWePrint1.500x2.000RectangleLabelsonRolls.pdf|e7c68e07cbd4b37e763f8e7d296b6150ac56d54ffadde3f03688664077f4584b|pdf"
)

foreach(entry IN LISTS files)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 dir)
  list(GET parts 1 url)
  list(GET parts 2 sha)
  list(GET parts 3 kind)
  get_filename_component(name "${url}" NAME)
  set(target "${DEST}/${dir}/${name}")
  if(EXISTS "${target}")
    file(SHA256 "${target}" have)
    if(have STREQUAL sha)
      continue()
    endif()
  endif()
  message(STATUS "hämtar ${url}")
  file(DOWNLOAD "${url}" "${target}" EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON STATUS st
       HTTPHEADER "User-Agent: Mozilla/5.0 (FubarDocumentCompare realworld tests)")
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE "${target}")
    message(FATAL_ERROR "kunde inte hämta ${url}: ${st}")
  endif()
  if(kind STREQUAL "zip")
    file(ARCHIVE_EXTRACT INPUT "${target}" DESTINATION "${DEST}/${dir}")
  endif()
endforeach()
message(STATUS "verkliga tryckfiler i ${DEST}")
