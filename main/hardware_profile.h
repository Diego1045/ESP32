#ifndef HARDWARE_PROFILE_H
#define HARDWARE_PROFILE_H

/**
 * RC522 en protoboard:
 * Cableado actual (validado): CS=D23 SCK=D26 MOSI=D25 MISO=D27 RST=D22
 * Con este cableado hace falta HW_SPI_SWAP_MOSI_MISO=1 y SPI 100 kHz.
 * swap=0 deja version 0x00 (SPI sin respuesta).
 */
#define HW_HAS_RELAY 0
#define HW_HAS_BOOT_BUTTON 0
#define HW_ACTUATOR_CONSOLE 1

#define RELAY_GPIO 2
#define BOOT_GPIO 0

#define HW_SPI_SWAP_MOSI_MISO 1

#define PIN_NUM_CS 23
#define PIN_NUM_CLK 26
#define PIN_NUM_RST 22

#if HW_SPI_SWAP_MOSI_MISO
#define PIN_NUM_MISO 25
#define PIN_NUM_MOSI 27
#else
#define PIN_NUM_MISO 27
#define PIN_NUM_MOSI 25
#endif

#endif
