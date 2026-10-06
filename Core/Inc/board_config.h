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
 * every 100 ms in SIG_GEN, so the running firmware can be told apart. */
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

#define FEATURE_UART_TEST   0     /* set to 1 to send "Hello World" on PA2 (USART2 TX, 500 kbaud) */

/* CAN (FDCAN1: PD0 RX / PD1 TX, through an external 3.3 V transceiver, see can_gen.c).
 * On by default in the SIG_GEN role only: PD0/PD1 are scope inputs in the other roles.
 * Edit the macros below, no build flags needed.
 *   CAN_BITRATE_KBPS  125, 250, 500 or 1000 (DroneCAN / ARK Flow normally 1000)
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
