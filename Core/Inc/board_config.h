#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* One firmware, three roles. Pick one with a CMake option (-DSCOPE_ONLY=ON or
 * -DSIG_GEN=ON), which overrides the defaults below, or change the defaults. A
 * build with no option uses the defaults (currently SCOPE_ONLY). The two roles
 * are mutually exclusive.
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
#define SCOPE_ONLY 1
#endif
#ifndef SIG_GEN
#define SIG_GEN 0
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

#endif
