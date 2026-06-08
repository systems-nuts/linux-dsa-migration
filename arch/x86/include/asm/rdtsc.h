// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Karim Manaouil - k.manaouil@gmail.com
 * Edinburgh
 * 2026
 */
#ifndef __RDTSC_H
#define __RDTSC_H

#include <linux/types.h>
#include <linux/math64.h>

static inline u64 cycles_to_ns_mhz(u64 cycles, u64 freq_mhz)
{
    /*
     * freq_mhz is CPU/TSC frequency in MHz.
     *
     * ns = cycles * 1,000 / freq_mhz
     *
     * Example:
     *   1750 MHz -> ns = cycles * 1000 / 1750
     *            -> ns = cycles * 4 / 7
     */
    return div64_u64(cycles * 1000ULL, freq_mhz);
}

static inline u64 cycles_to_ns_1750mhz(u64 cycles)
{
       return cycles_to_ns_mhz(cycles, 1750);
}

static inline u64 cycles_to_ns_1800mhz(u64 cycles)
{
       return cycles_to_ns_mhz(cycles, 1800);
}

#define cycles_to_ns(cycles) cycles_to_ns_1750mhz(cycles)

static inline u64 rdtsc_begin(void)
{
    u32 cycles_low, cycles_high;

    asm volatile (
        "CPUID\n\t"
        "RDTSC\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        : "=r" (cycles_high), "=r" (cycles_low)
        :
        : "%rax", "%rbx", "%rcx", "%rdx"
    );

    return ((u64)cycles_high << 32) | cycles_low;
}

static inline u64 rdtsc_end(void)
{
    u32 cycles_low, cycles_high;

    asm volatile (
        "RDTSCP\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        "CPUID\n\t"
        : "=r" (cycles_high), "=r" (cycles_low)
        :
        : "%rax", "%rbx", "%rcx", "%rdx"
    );

    return ((u64)cycles_high << 32) | cycles_low;
}

#endif
