/*
 * Fault reporter for nsbackend-pico.
 *
 * Makes crashes visible instead of silent: a HardFault on either core and any pico-sdk
 * panic() print a diagnostic to UART0 (GP12 TX, 115200 baud), then reboot the board a
 * few seconds later so a headless unit recovers and stays reflashable over USB.
 * Nothing needs to be called; the handlers are installed by being linked in
 * (isr_hardfault overrides the weak crt0 vector, panic() is redirected through
 * PICO_PANIC_FUNCTION in CMakeLists.txt).
 */

#pragma once
