// PulsOx firmware entry point. The whole application is in src/app/app.cpp (see app.h for the
// states); the sensor driver lives in lib/max30102, the signal processing in lib/ppg, the
// screens in src/display/display.cpp, the network in src/cloud/cloud_link.cpp and the sleep in src/board/power.cpp.
#include <Arduino.h>
#include "app/app.h"

/*******************************************************************************
 * @brief       Punto de entrada de Arduino: llama a appSetup().
 *
 * @details     Toda la aplicación vive en src/app/app.cpp.
 *
 * @param       Ninguno.
 *
 * @peripherals Internos: los de appSetup(): GPIO, I2C, SPI, Serial/USB-CDC,
 *              WiFi, tareas y sleep.
 *              Externos: MAX30102, pantalla TFT, botón, LED de estado.
 *******************************************************************************/
void setup() { appSetup(); }

/*******************************************************************************
 * @brief       Bucle de Arduino: llama a appLoop().
 *
 * @details     Toda la aplicación vive en src/app/app.cpp.
 *
 * @param       Ninguno.
 *
 * @peripherals Internos: los de appLoop(): GPIO, I2C, SPI, Serial/USB-CDC,
 *              WiFi, sleep.
 *              Externos: MAX30102, pantalla TFT, botón, LED de estado.
 *******************************************************************************/
void loop() { appLoop(); }
