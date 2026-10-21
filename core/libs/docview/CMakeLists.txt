# Tillståndslösa PDF-tjänster för visning: metadata, varianter (lager/dekorfärger), rendering i
# rutor och överlägg. Cachen är innehållsadresserad (SHA-256) och osynlig för resultaten.
printcmp_library(docview
  PUBLIC pc_core pc_pdf opencv_core
  PRIVATE pc_barcode pc_image opencv_imgproc)
