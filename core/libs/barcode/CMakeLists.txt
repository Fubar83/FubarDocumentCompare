# zxing-cpp används bara här (avkodning och kodning).
printcmp_library(barcode
  PUBLIC pc_core opencv_core
  PRIVATE ZXing::ZXing opencv_imgproc)
