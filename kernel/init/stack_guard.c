#include <stddef.h>
#include <stdint.h>
#include <tunix/cpu.h>
#include <tunix/random.h>

extern void panic(const char *msg) __attribute__((noreturn));

uint64_t __stack_chk_guard = 0x5D1C7A3E9B20F600ULL;

void __stack_chk_fail(void) __attribute__((noreturn));
void __stack_chk_fail(void) { panic("stack protector: a kernel stack frame was overwritten"); }

__attribute__((no_stack_protector)) void stack_guard_init(void) {
    uint64_t values[18];
    size_t count = arch_entropy_collect(values, sizeof(values) / sizeof(values[0]));
    uint64_t mixed = __stack_chk_guard ^ cpu_counter();
    for (size_t index = 0; index < count; index++) {
        mixed = (mixed ^ values[index]) * 0x9E3779B97F4A7C15ULL;
        mixed ^= mixed >> 29;
    }
    mixed &= ~0xFFULL;
    if (mixed) __stack_chk_guard = mixed;
}
