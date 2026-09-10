/*
 * Fault reporter implementation (see fault_reporter.hpp).
 *
 * Both handlers write with polling UART I/O and touch no RTOS state, so they are safe
 * from any context, including a fault taken inside an interrupt or a critical section.
 * The stock pico-sdk behaviour for both cases is a breakpoint instruction, which without
 * a debugger attached just stops the core with no output at all.
 */

#include "fault_reporter.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"

namespace {

void raw_puts(const char* s) {
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

void raw_hex(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[11] = "0x";
    for (int i = 0; i < 8; ++i) {
        buf[2 + i] = digits[(v >> (28 - 4 * i)) & 0xF];
    }
    buf[10] = 0;
    raw_puts(buf);
}

void raw_reg(const char* name, uint32_t v) {
    raw_puts(name);
    raw_puts("=");
    raw_hex(v);
    raw_puts(" ");
}

// Give the UART time to drain, then reboot so a headless unit recovers on its own and
// can be reflashed over USB again; the report is already out on the wire by then.
constexpr uint32_t REBOOT_DELAY_MS = 3000;

[[noreturn]] void halt_then_reboot() {
    raw_puts("[E][Fault] rebooting in 3 s\r\n");
    watchdog_reboot(0, 0, REBOOT_DELAY_MS);
    while (true) {
        tight_loop_contents();
    }
}

}  // namespace

// Called from the naked isr_hardfault below with the exception frame the core pushed:
// r0-r3, r12, lr, pc, xpsr.
extern "C" [[noreturn]] void nsbackend_hardfault_report(const uint32_t* frame, uint32_t exc_return) {
    raw_puts("\r\n[E][Fault] HardFault on core ");
    raw_puts(get_core_num() == 0 ? "0" : "1");
    raw_puts("\r\n  ");
    raw_reg("PC", frame[6]);
    raw_reg("LR", frame[5]);
    raw_reg("xPSR", frame[7]);
    raw_reg("EXC_RETURN", exc_return);
    raw_puts("\r\n  ");
    raw_reg("R0", frame[0]);
    raw_reg("R1", frame[1]);
    raw_reg("R2", frame[2]);
    raw_reg("R3", frame[3]);
    raw_reg("R12", frame[4]);
#if defined(PICO_RP2350) && PICO_RP2350
    // Cortex-M33 fault status registers; the M0+ on RP2040 has none of these
    raw_puts("\r\n  ");
    raw_reg("CFSR", *reinterpret_cast<volatile uint32_t*>(0xE000ED28));
    raw_reg("HFSR", *reinterpret_cast<volatile uint32_t*>(0xE000ED2C));
    raw_reg("MMFAR", *reinterpret_cast<volatile uint32_t*>(0xE000ED34));
    raw_reg("BFAR", *reinterpret_cast<volatile uint32_t*>(0xE000ED38));
#endif
    raw_puts("\r\n");
    halt_then_reboot();
}

// Overrides the weak crt0 vector. Naked so the stack pointer is read before the compiler
// generates any prologue; picks MSP or PSP from EXC_RETURN bit 2. Written in Thumb-1
// compatible form so the same code serves the Cortex-M0+ (RP2040) and M33 (RP2350).
extern "C" __attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov  r1, lr\n"
        "tst  r0, r1\n"
        "beq  1f\n"
        "mrs  r0, psp\n"
        "b    2f\n"
        "1:\n"
        "mrs  r0, msp\n"
        "2:\n"
        "mov  r1, lr\n"
        "ldr  r2, =nsbackend_hardfault_report\n"
        "bx   r2\n");
}

// Installed as the pico-sdk panic() implementation via PICO_PANIC_FUNCTION.
extern "C" [[noreturn]] void nsbackend_panic(const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    raw_puts("\r\n[E][Fault] panic on core ");
    raw_puts(get_core_num() == 0 ? "0" : "1");
    raw_puts(": ");
    raw_puts(buf);
    raw_puts("\r\n");
    halt_then_reboot();
}
