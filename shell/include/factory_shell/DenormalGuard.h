#pragma once
//
// factory_shell/DenormalGuard.h — a scoped FTZ/DAZ (flush-to-zero /
// denormals-are-zero) guard the CLAP shell wraps around process().
//
// WHY THIS LIVES IN THE SHELL (not the DSP core): per the chunk-2 contract the
// DSP cores are FP-mode-agnostic — they neither assume nor set the CPU rounding
// mode. Denormal flushing is a HOST-BOUNDARY concern (a resonance feedback tail
// decaying into the 1e-30 range must not fall off the denormal cliff and spike
// CPU), so the wrapper that owns the real-time boundary sets it, exactly as the
// shipping build's scoped-no-denormals guard does inside its processBlock. The
// guard SAVES the caller's control word on construction and RESTORES it on
// destruction, so the host's own FP mode is left untouched outside process().
//
// Real-time safe: construction/destruction are two register reads + writes, no
// allocation, no lock, no syscall.
//
#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
 #include <xmmintrin.h>   // _mm_getcsr / _mm_setcsr (MXCSR)
 #define FACTORY_SHELL_DENORMAL_X86 1
#elif defined(__aarch64__)
 #define FACTORY_SHELL_DENORMAL_AARCH64 1
#endif

namespace factory_shell
{
    // RAII: set FTZ+DAZ for the lifetime of the object, restore the prior mode after.
    class ScopedNoDenormals
    {
    public:
        ScopedNoDenormals() noexcept
        {
#if defined(FACTORY_SHELL_DENORMAL_X86)
            // MXCSR: bit 15 = FTZ (flush-to-zero), bit 6 = DAZ (denormals-are-zero).
            saved = _mm_getcsr();
            _mm_setcsr ((saved & ~0x8040u) | 0x8040u);
#elif defined(FACTORY_SHELL_DENORMAL_AARCH64)
            // FPCR: bit 24 = FZ (flush-to-zero for AArch64 SIMD/FP).
            std::uint64_t fpcr;
            __asm__ __volatile__ ("mrs %0, fpcr" : "=r" (fpcr));
            saved = fpcr;
            fpcr |= (std::uint64_t (1) << 24);
            __asm__ __volatile__ ("msr fpcr, %0" : : "r" (fpcr));
#endif
        }

        ~ScopedNoDenormals() noexcept
        {
#if defined(FACTORY_SHELL_DENORMAL_X86)
            _mm_setcsr (saved);
#elif defined(FACTORY_SHELL_DENORMAL_AARCH64)
            __asm__ __volatile__ ("msr fpcr, %0" : : "r" (saved));
#endif
        }

        ScopedNoDenormals (const ScopedNoDenormals&)            = delete;
        ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

    private:
#if defined(FACTORY_SHELL_DENORMAL_X86)
        unsigned int saved = 0;
#elif defined(FACTORY_SHELL_DENORMAL_AARCH64)
        std::uint64_t saved = 0;
#endif
    };
} // namespace factory_shell
