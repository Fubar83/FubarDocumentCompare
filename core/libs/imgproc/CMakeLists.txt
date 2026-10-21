# Bildbehandling på områden: filter, FFT, mönstersökning, mätning. Bara OpenCV under.
printcmp_library(imgproc
  PUBLIC opencv_core
  PRIVATE opencv_imgproc opencv_features2d opencv_calib3d)
