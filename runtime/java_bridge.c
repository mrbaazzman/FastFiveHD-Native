/*
 * Java-bridge replacements.
 *
 * The Android build obtains a few values from Java. In the PC port the Java layer is replaced
 * by this runtime, so these functions are served directly. Addresses are ELF (the dump is
 * normalized). Return values match what the original Java methods produce on the reference
 * device profile (Samsung I9000, English, version 1.0.9).
 */
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"

extern uint32_t guest_mem_size;

#define VERSION_STRING_ADDR 0x9B0000u

static void put_cstr(uint8_t *mem, uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1;
    if (addr > guest_mem_size || n > guest_mem_size - addr) {
        guest_fault(addr, 1);
        return;
    }
    memcpy(mem + addr, s, n);
}

/* Returns 1 when the address is a Java-bridge function handled here. */
int call_java(Cpu *c, uint8_t *mem, uint32_t target) {
    switch (target) {
    case 0x29a300u:  /* nativeGetVersion -> GLGame.getVersion() */
        put_cstr(mem, VERSION_STRING_ADDR, "1.0.9");
        c->r0 = VERSION_STRING_ADDR;
        return 1;
    case 0x29a064u:  /* nativeGetDeviceID -> GLGame.GetDeviceID() = 9000 (I9000) */
        c->r0 = 9000u;
        return 1;
    case 0x29a540u:  /* nativeDetectDeviceLang -> GLGame.detectLanguage() = English */
        c->r0 = 1u;
        return 1;
    case 0x29a508u:  /* nativePutDeviceToSleep -> Java sleep; no-op on the host */
        return 1;
    case 0x6bd0f0u: { /* ALicenseCheck::getIMEI(char *out, int size) */
        static const char imei[] = "000000000000000";
        uint32_t dst = c->r0, n = c->r1, k = 0;
        if (n && dst < guest_mem_size) {
            while (k + 1 < n && imei[k]) { mem[dst + k] = (uint8_t)imei[k]; k++; }
            mem[dst + k] = 0;
        }
        return 1;
    }
    case 0x6be364u:  /* ALicenseCheck_ValidateLicense: replaced by the host runtime */
        return 1;
    case 0x6be258u:  /* ValidateServer: no online license service in the port */
        return 1;
    default:
        return 0;
    }
}
