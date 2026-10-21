# PDFium och qpdf används bara här; resten av kärnan läser PDF genom pc_pdf.
printcmp_library(pdf
  PUBLIC pc_core opencv_core
  PRIVATE pdfium qpdf::libqpdf opencv_imgproc)
