#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* One firmware, three roles. Pick one with a CMake option (-DSCOPE_ONLY=ON or
 * -DSIG_GEN=ON), which overrides the defaults below, or change the defaults. A
 * build with no option uses the defaults set below, so check them before
 * flashing. The two roles are mutually exclusive.
 *
 *   both 0        scope + signal generators: captures and sends bursts to the PC,
 *                 and also drives the test PWM and the OLED counter
 *   SCOPE_ONLY=1  scope + the built-in test signal; no I2C/OLED, no UART output.
 *                 Flash this on the board that does the probing.
 *   SIG_GEN=1     signal generator only: test PWM, OLED counter on I2C4 (and
 *                 optionally UART). No acquisition and USB is never started, so
 *                 nothing is sent to the PC. Use it as the target board that is
 *                 probed by a SCOPE_ONLY board.
 *
 * The PB3-PB10 test signal (1 MHz) is generated in every role.
 *
 * LED: toggles every 500 ms when the board has USB (scope roles),
 * every 100 ms in SIG_GEN, so the running firmware can be told apart.
 *
 * Pin map (3.3 V logic; each is described again next to its feature below):
 *   Capture inputs D0-D7   PD0-PD7                    scope roles
 *   Built-in test signal   PB3-PB10 (D0=PB3 ... D7=PB10)   every role
 *   USB to the PC          PA11 (D-), PA12 (D+)       scope roles
 *   UART test output       PA2  (USART2 TX)           FEATURE_UART_TEST
 *   I2C OLED               PD12 (SCL), PD13 (SDA)     FEATURE_OLED_I2C
 *   SPI TFT                PA5 SCK, PA7 MOSI, PA4 CS, PC4 DC, PC5 RES   FEATURE_TFT_SPI
 *   CAN                    PD1 (TX), PD0 (RX)         FEATURE_CAN_TEST
 *   LED PC13, DMA debug PC8, SWD PA13/PA14
 * PD0/PD1 are capture inputs in the scope roles and the CAN pins in SIG_GEN. */

#ifndef SCOPE_ONLY
#define SCOPE_ONLY 0
#endif
#ifndef SIG_GEN
#define SIG_GEN 1
#endif

#if SCOPE_ONLY && SIG_GEN
#error "SCOPE_ONLY and SIG_GEN are mutually exclusive"
#endif

#if SIG_GEN
#define FEATURE_ACQUISITION 0     /* TIM2 + DMA sampling of PD0-PD7, trigger, burst upload */
#define FEATURE_USB         0     /* USB device: not started, nothing is sent to the PC */
#define FEATURE_OLED_I2C    1     /* SSD1306 on I2C4 (PD12 SCL / PD13 SDA) */
#elif SCOPE_ONLY
#define FEATURE_ACQUISITION 1
#define FEATURE_USB         1
#define FEATURE_OLED_I2C    0
#else
#define FEATURE_ACQUISITION 1
#define FEATURE_USB         1
#define FEATURE_OLED_I2C    1
#endif

#define FEATURE_UART_TEST   1     /* set to 1 to send "Hello World" on PA2 (USART2 TX, 500 kbaud) */

/* CAN (FDCAN1: PD0 RX / PD1 TX, through an external 3.3 V transceiver, see can_gen.c).
 * On by default in the SIG_GEN role only: PD0/PD1 are scope inputs in the other roles.
 * Edit the macros below, no build flags needed.
 *   CAN_BITRATE_KBPS  125, 250, 500 or 1000 (DroneCAN normally uses 1000)
 *   CAN_LOOPBACK      0 = normal node on the bus (ACKs frames from other nodes)
 *                     1 = external loopback, no other node needed
 *   CAN_GEN_TX        1 = also send the test frames, 0 = listen only (still ACKs)
 *   CAN_DNA_SERVER    1 = run a DroneCAN dynamic node ID server (node 1): a sensor that
 *                     asks for an ID gets node 20, then starts publishing normally */
#define FEATURE_CAN_TEST    SIG_GEN
#define CAN_BITRATE_KBPS    1000
#define CAN_LOOPBACK        0
#define CAN_GEN_TX          0
#define CAN_DNA_SERVER      1     /* answer DroneCAN node ID requests (gives the sensor an ID) */

/* ST7789V 240x320 SPI TFT on SPI1 (see tft.h for the pins), signal generator role only.
 * It produces known SPI traffic (commands with the DC line low, then data) for the logic
 * analyser. TFT_SPI_KHZ: 250, 500, 1000, 2000, 4000, 8000, 16000 or 32000 (64 MHz / 2^n).
 * The ST7789V datasheet allows about 15 MHz writes; 16000 is at that limit and 32000 is
 * beyond it (many modules still work). Start low and step up.
 *   TFT_INVERT  1 = colour inversion on (most IPS modules), 0 = off
 *   TFT_BGR     1 = swap red and blue, if the colour bars show the wrong order */
#define FEATURE_TFT_SPI     SIG_GEN
#define TFT_SPI_KHZ         1000
#define TFT_INVERT          1
#define TFT_BGR             0

/* OLED I2C bus speed in kHz: 100, 400 or 1000.
 *   100   standard mode
 *   400   fast mode
 *   1000  fast-mode plus (stronger SCL/SDA drive is enabled). The SSD1306 is only
 *         rated for 400 kHz, so a module may not work at 1000.
 * The timing registers are worked out for the 120 MHz I2C4 kernel clock; the real
 * rate also depends on the bus pull-ups and wiring, so check SCL on the scope.
 * Can also be set on the CMake command line: -DI2C_SPEED_KHZ=400 */
#ifndef I2C_SPEED_KHZ
#define I2C_SPEED_KHZ 1000
#endif
#if I2C_SPEED_KHZ != 100 && I2C_SPEED_KHZ != 400 && I2C_SPEED_KHZ != 1000
#error "I2C_SPEED_KHZ must be 100, 400 or 1000"
#endif

#endif
