#pragma once

#if defined(__aarch64__) || defined(__ARM_ARCH_8) || defined(_M_ARM64)
#include "asmjit/arm/a64assembler.h"
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(__x86_64) || defined(__amd__64__)
#	define JIT_X64 1
#else
#	define JIT_X64 0
#endif

#if defined(__aarch64__) || defined(__ARM_ARCH_8) || defined(_M_ARM64)
#	define JIT_ARM64 1
#else
#	define JIT_ARM64 0
#endif

#if JIT_X64
#	include "asmjit/x86/x86builder.h"
#	include "asmjit/x86/x86operand.h"
#endif

#if JIT_ARM64
#	include "asmjit/arm/a64builder.h"
#	include "asmjit/arm/a64operand.h"
#endif

namespace esp
{

#if JIT_X64
	using Builder = asmjit::x86::Builder;
	using RegGP = asmjit::x86::Gpq;

	class EspJitX64;
	using EspJit = EspJitX64;
#elif JIT_ARM64
	/* The ESP program is straight-line code (no labels, no node edits), so
	 * the Assembler is a drop-in for the Builder with identical encodings —
	 * and without Builder::finalize(), which re-walked every node and cost as
	 * much as the emit itself (0.21 ms of a 0.45 ms core compile on Move). */
	using Builder = asmjit::a64::Assembler;
	using RegGP = asmjit::a64::GpX;

	class EspJitArm64;
	using EspJit = EspJitArm64;
#endif

}
