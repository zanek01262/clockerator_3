/* =====================================================================
   Clockerator.ino.globals.h  —  build-wide defines (ESP8266 core 3.1+)
   ---------------------------------------------------------------------
   The ESP8266 core force-includes this file (by its exact name) into
   EVERY file it compiles: the core, the libraries and the sketch. That
   is the only way to set a library's compile-time option from the
   sketch folder without editing the library itself.

   SdFat (the copy bundled with the ESP8266 core) picks its SPI driver at
   compile time. 3 = "an external driver derived from SdSpiBaseClass":
   ours, in sdcard.cpp, bit-banged on D0-D3. The hardware SPI pins
   (D5-D7) belong to the I2S microphone.

   If you rename the sketch, rename this file to match
   (<SketchName>.ino.globals.h) or the SD card code will not compile.
   ===================================================================== */
#ifndef CLOCKERATOR_INO_GLOBALS_H
#define CLOCKERATOR_INO_GLOBALS_H

#define SPI_DRIVER_SELECT 3

#endif
