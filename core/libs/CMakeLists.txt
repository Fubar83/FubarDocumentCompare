# Beroendegrafen. Varje bibliotek ser bara de publika rubrikerna (include/printcmp/<namn>) hos
# biblioteken det beror på, och tredjepartsbibliotek bara där de används.
#
#   grund:       core
#   tjänster:    image  imgproc  pdf  pdfwrite  braille  barcode  docview
#   analys:      doc  artwork  scan  compare (compare använder även imgproc)
#   tillämpning: report  jobspec
#   testsviter:  testgen  eval  bench
foreach(lib core image imgproc pdf pdfwrite braille barcode docview doc artwork scan compare report jobspec testgen eval bench)
  add_subdirectory(${lib})
endforeach()
