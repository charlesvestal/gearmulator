#include <unistd.h>
#include <time.h>
#include <cstdlib>
#include <cstdio>
#include <sstream>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

/* JE_ESP_NO_JIT: built for a platform that will not map an executable page
 * (iOS/iPadOS). asmjit and the two emitters are not compiled at all, and
 * ESPOptimizer below becomes a stub -- ESP::step_cores() runs the program
 * instead (see jeEspInterp() in je8086devices.h). */
#ifndef JE_ESP_NO_JIT
#include <asmjit/asmjit.h>
#include <asmjit/a64.h>

#include "esp_jit_x64.h"
#include "esp_jit_arm64.h"
#include "esp_jit_types.h"
#endif

constexpr int PRAM_SIZE = 768;

/* Can this process map an executable page at all? iOS refuses without the
 * dynamic-codesigning entitlement, so a JIT-capable binary still has to be able
 * to fall back -- and asmjit reports the refusal by THROWING out of JitRuntime::
 * add(), which on the boot thread is a crash rather than a fallback. Probe once
 * with a trivial function so the decision is made before any of that.
 *
 * A dev-signed build with a debugger attached gets CS_DEBUGGED and this returns
 * true; the same binary launched normally returns false and interprets. */
inline bool espJitAvailable()
{
	static const bool ok = []
	{
#if defined(__APPLE__)
		/* iOS lets asmjit MAP an executable page -- the mapping succeeds and this
		 * probe used to report the JIT as available -- and then kills the process
		 * with SIGKILL the moment it EXECUTES unsigned code. Measured: the JIT
		 * build dies within seconds of boot, while the same binary with
		 * JE_ESP_INTERP=1 runs indefinitely.
		 *
		 * The probe cannot discover this by trying, because trying is what gets
		 * the process killed. So iOS is opt-in only: a dev-signed build under a
		 * debugger gets CS_DEBUGGED and can execute, and JE_FORCE_JIT=1 says so
		 * explicitly. Everything else interprets. */
		if (TARGET_OS_IPHONE)
			return ::getenv("JE_FORCE_JIT") != nullptr;
#endif
#ifdef JE_ESP_NO_JIT
		/* No emitter was compiled in and asmjit was not even linked, so there
		 * is nothing to probe. Without this the probe below still has to
		 * COMPILE, and a from-clean no-JIT build fails on every asmjit name in
		 * it -- which is the configuration iOS always uses. */
		return false;
#else
		try
		{
			asmjit::JitRuntime rt;
			asmjit::CodeHolder code;
			if (code.init(rt.environment(), rt.cpuFeatures()) != asmjit::kErrorOk)
				return false;
#if defined(__aarch64__) || defined(_M_ARM64)
			asmjit::a64::Assembler a(&code);
			a.ret(asmjit::a64::x30);
#else
			asmjit::x86::Assembler a(&code);
			a.ret();
#endif
			void* fn = nullptr;
			if (rt.add(&fn, &code) != asmjit::kErrorOk || !fn)
				return false;
			rt.release(fn);
			return true;
		}
		catch (...)
		{
			return false;
		}
#endif
	}();
	return ok;
}

/* JE_GENLOG=1 traces JIT recompiles on stderr. Read once: this is consulted
 * from genProgram(), which runs on the render thread. */
inline bool espGenLog() { static const bool on = getenv("JE_GENLOG") != nullptr; return on; }

/* Diagnostics: global counters for JIT activity (all ESP instances). */
inline uint64_t g_esp_genprogram_count = 0;
inline uint64_t g_esp_dirty_count = 0;
inline uint64_t g_esp_updatecoef_count = 0;

struct CoreData {
  int32_t *hostRegPtr;
  int32_t *eramPtr;
  int64_t accs[6];
  int32_t mulcoeffs[8];
  int8_t coefs[PRAM_SIZE];
  int8_t shiftAmounts[PRAM_SIZE];
  // coef << (7 - shiftAmount): (A*coef) >> shiftAmount == (A*coefShifted) >> 7 exactly, so a plain
  // MAC needs one load and an immediate shift. shiftAmount is one of 3,5,6,7 -> fits int16.
  int16_t coefsShifted[PRAM_SIZE];
};

enum { kNone = 0, kSavesA = 1, kSavesB = 2 };

struct MemAccess
{
	uint8_t save{kNone};
	bool writesAccB{false}, clr{false}, nop{false}; // stage 1, extract these.
	bool accGetsUsed{false}, nomac{false};
	uint16_t writePC{0}; // stage 2, compute this
	int readReg{-1}, srcReg{-1}, destReg{-1}; // stage 3, compute this
};

enum ESPInstrOptType
{
	kNop, kMAC, kStoreIRAM, kStoreGRAM, kMulCoef, kWriteMulCoef, kDMAC,
	kWriteEramVarOffset, kWriteEramWriteLatch, kReadEramReadLatch, kWriteHost,
	kReadGRAM, kStoreIRAMUnsat, kStoreIRAMRect, kSetCondition, kInterp,
	kInterpStorePos, kInterpStoreNeg
};

struct ESPOptInstr
{
	// raw data
	uint32_t coef = 0, shift = 0, mem = 0, op = 0;
	int8_t coefSigned = 0;

	// processed data
	uint8_t shiftAmount = 0;
	bool useImm = false;
	int32_t imm = 0;
	ESPInstrOptType opType = kMAC;

	// from analysis
	MemAccess m_access;
	bool skippablePos = false, skippableNeg = false;

	ESPOptInstr()
	= default;

	ESPOptInstr(uint32_t instr)
	{
		mem = (instr >> 10) & 0xff;
		op = (instr >> 16) & 0x7c;
		coef = instr & 0xff;
		coefSigned = se<8>(coef);
		shift = (instr >> 8) & 3;
		shiftAmount = (0x3567 >> (shift << 2)) & 0xf; // this is shift amount. pick the value 3/5/6/7 using bits 8,9.
		if (op == 0x20 || op == 0x24) shiftAmount = (shift & 1) ? 6 : 7;

		switch (mem)
		{
		case 1: useImm = true;
			imm = 0x10;
			break; // 4
		case 2: useImm = true;
			imm = 0x400;
			break; // 10
		case 3: useImm = true;
			imm = 0x10000;
			break; // 16
		case 4: useImm = true;
			imm = 0x400000;
			break; // 22
		}

		if (op == 0 && mem == 0 && shift == 0 && coef == 0)
		{
			opType = kNop;
		}
		else
		{
			switch (op)
			{
			case 0x08:
			case 0x0c:
			case 0x18:
			case 0x1c:
			case 0x58:
			case 0x5c: opType = kStoreIRAM;
				break;
			case 0x20:
			case 0x24: opType = kReadGRAM;
				break;
			case 0x30: opType = kMulCoef;
				break;

			case 0x34:
				if (mem < 0xa0 || (mem & 0xf0) == 0xb0) printf("Unexpected value for mem (%02x) with opcode 0x34\n",
				                                               mem);
				if (mem >= 0xa0 && mem < 0xb0) opType = kWriteMulCoef;
				if (mem >= 0xc0)
				{
					switch (mem & 0xf)
					{
					// unsupported stuff
					case 0x0:
					case 0x1:
					case 0x2:
					case 0x3:
						printf("jump!\n");
						opType = kNop;
						break;
					case 0x4:
						printf("int pins!\n");
						break;

					case 0x6: opType = kDMAC;
						break;
					case 0x7: opType = kWriteEramVarOffset;
						break;
					case 0xa: opType = kWriteHost;
						break;
					case 0xb: opType = kWriteEramWriteLatch;
						break;
					case 0xc:
					case 0xd:
					case 0xe:
					case 0xf:
						opType = kReadEramReadLatch;
						break;
					default:
						printf("Unknown value for mem (%02x) with opcode 0x34\n", mem);
						break;
					}
				}
				break;

			case 0x38:
			case 0x3c: opType = kStoreGRAM;
				break;
			case 0x40:
			case 0x44: opType = kStoreIRAMUnsat;
				break;
			case 0x48:
			case 0x4c: opType = kStoreIRAMRect;
				break;
			case 0x50: opType = kSetCondition;
				printf("CONDITIONS!\n");
				break;
			case 0x60:
			case 0x64:
			case 0x70:
			case 0x74: opType = kInterp;
				break;
			case 0x68:
			case 0x6c: opType = kInterpStorePos;
				break;
			case 0x78:
			case 0x7c: opType = kInterpStoreNeg;
				break;

			case 0x54:
			case 0x28:
			case 0x2c: printf("Mysterious opcode %02x\n", op);
				break; // TODO: what is this?
			}
		}
	}
};

#ifdef JE_ESP_NO_JIT

/* No-JIT stub. The five entry points the emulator calls, all no-ops: with the
 * interpreter there is no generated code to compile, release or invoke. */
template<int lg2eram_size>
class ESPOptimizer
{
public:
  explicit ESPOptimizer(ESP<lg2eram_size>*) {}
  void genProgram(ESP<lg2eram_size>*, uint32_t = 3) {}
  void genProgramIfDirty() {}
  void setProgramDirty(uint32_t = 3) { ++g_esp_dirty_count; }
  void updateCoef(ESP<lg2eram_size>*) {}
  void callOptimized(ESP<lg2eram_size>*) {}
};

#else

template<int lg2eram_size>
class ESPOptimizer
{
public:
  ESPOptimizer(ESP<lg2eram_size>* esp) : m_esp(esp), logger(fopen("esp_jit.log", "w"))
  {
    data_core0.hostRegPtr = (int32_t*)esp->shared.readback_regs;
    data_core0.eramPtr = &esp->shared.eram.eram[0];
    data_core1.hostRegPtr = (int32_t*)esp->shared.readback_regs;
    data_core1.eramPtr = &esp->shared.eram.eram[0];
  }

  /* `cores` is a bit mask: 1 = core 0, 2 = core 1. A core's generated code
   * depends only on its own program words (the ERAM decode reads core 1's),
   * so a write burst that touched one core recompiles one core. The H8S
   * loads a patch in ~60 bursts per ASIC, each one a full recompile at
   * ~1 ms on the device, and that is the stall at patch change. */
  void genProgram(ESP<lg2eram_size>* esp, uint32_t cores = 3)
  {
    ++g_esp_genprogram_count;
    if ((cores & 1) && runCore0) m_rt.release(runCore0);
    if ((cores & 2) && runCore1) m_rt.release(runCore1);

    if (cores & 2) eramEmitter.init(esp);
    if (cores & 1) coreEmitter0.init(esp, &esp->core0);
    if (cores & 2) coreEmitter1.init(esp, &esp->core1);

    updateCoef(esp);

    struct timespec _t0; clock_gettime(CLOCK_MONOTONIC, &_t0);
    // logger.log("#### CORE 0 ####\n");
    if (cores & 1) genCore(esp, 0, &coreEmitter0, &runCore0);
    
    // logger.log("\n\n\n#### CORE 1 ####\n");
    if (cores & 2) genCore(esp, 1, &coreEmitter1, &runCore1, true);
    if (espGenLog()) {
      struct timespec _t1; clock_gettime(CLOCK_MONOTONIC, &_t1);
      fprintf(stderr, "[genProgram pid=%d] cores=%u %.2f ms (#%llu)\n", (int)getpid(), cores,
              ((_t1.tv_sec-_t0.tv_sec)*1e9 + (_t1.tv_nsec-_t0.tv_nsec))/1e6, (unsigned long long)g_esp_genprogram_count);
    }

    // fflush(logger._file);
    // printf("JITed ESP cores\n");
  }
  
  void setProgramDirty(uint32_t cores = 3)
  {
	  ++g_esp_dirty_count;
	  if (!m_programDirty) m_firstDirty = m_sampleClock;
	  ++m_dirtyWrites;
	  m_dirtyCores |= cores;
	  m_programDirty = 3;
  }

  uint64_t m_sampleClock = 0;
  void genProgramIfDirty()
  {
      ++m_sampleClock;
      if (m_programDirty > 0)
      {
          if (--m_programDirty == 0) {
              if (espGenLog()) fprintf(stderr, "[gen pid=%d this=%p] at sample %llu, first dirty at %llu, %u writes\n", (int)getpid(), (void*)this, (unsigned long long)m_sampleClock, (unsigned long long)m_firstDirty, m_dirtyWrites);
              m_dirtyWrites = 0;
              const uint32_t cores = m_dirtyCores;
              m_dirtyCores = 0;
              genProgram(m_esp, cores);
          }
	  }
  }
  uint64_t m_firstDirty = 0; uint32_t m_dirtyWrites = 0;

  static void updateCoefEntry(CoreData& data, const uint32_t* pram, size_t i)
  {
    uint32_t instr = pram[i];
    uint32_t op = (instr >> 16) & 0x7c;
    int8_t coef = se<8>(instr & 0xff);
    uint32_t shift = (instr >> 8) & 3;
    uint32_t shiftAmount = (0x3567 >> (shift << 2)) & 0xf;
    if (op == 0x20 || op == 0x24) shiftAmount = (shift & 1) ? 6 : 7;
    data.coefs[i] = coef;
    data.shiftAmounts[i] = shiftAmount;
    data.coefsShifted[i] = static_cast<int16_t>(static_cast<int32_t>(coef) * (1 << (7 - shiftAmount)));
  }

  // Recompute every entry of both cores (after a program write / recompile).
  void updateCoef(ESP<lg2eram_size>* esp)
  {
    ++g_esp_updatecoef_count;
    for (size_t i = 0; i < PRAM_SIZE; i++) updateCoefEntry(data_core0, esp->core0.pram, i);
    for (size_t i = 0; i < PRAM_SIZE; i++) updateCoefEntry(data_core1, esp->core1.pram, i);
  }

  inline void callOptimized(ESP<lg2eram_size>* esp)
  {
    if (runCore0) runCore0(data_core0.coefs, esp->core0.iram, esp->shared.gram, &data_core0, esp->shared.eram.eramPos, esp->core0.iramPos, 0, 0);
    if (runCore1) runCore1(data_core1.coefs, esp->core1.iram, esp->shared.gram, &data_core1, esp->shared.eram.eramPos, esp->core1.iramPos, 0, 0);
  }
  
private:
  class CoreEmitter;

  ESP<lg2eram_size>* m_esp;
  asmjit::JitRuntime m_rt;
  asmjit::FileLogger logger;
  uint32_t m_programDirty = 0;
  uint32_t m_dirtyCores = 0;
  
  typedef void(*RunCore)(int8_t* coefsPtr, int32_t *iramPtr, int32_t *gramPtr, CoreData *varPtr, uint32_t eramPos, uint32_t iramPos, int64_t unused1, int64_t unused2);
  RunCore runCore0 = nullptr, runCore1 = nullptr;

  // State used by jitted code
  CoreData data_core0{0};
  CoreData data_core1{0};

  void genCore(ESP<lg2eram_size>* esp, uint32_t core, CoreEmitter* emitter, RunCore *dest, bool withEram = false)
  {
    // TODO: do we need a new CodeHolder each time?
    asmjit::CodeHolder code;
    code.init(m_rt.environment());

  	logger.addFlags(asmjit::FormatFlags::kHexImms | /*asmjit::FormatFlags::kHexOffsets |*/ asmjit::FormatFlags::kMachineCode);

//	code.setLogger(&logger);
    
    esp::Builder m_asm(&code);

	CoreData& coreData = core ? data_core1 : data_core0;
	ESPCore<lg2eram_size>& espCore = core ? esp->core1 : esp->core0;

    esp::JitInputData jitData;
	jitData.coreData = &coreData;
	jitData.iram = espCore.iram;
	jitData.gram = esp->shared.gram;

  	jitData.eramPos = &esp->shared.eram.eramPos;
	jitData.iramPos = &espCore.iramPos;

	jitData.eramEffectiveAddr = &esp->shared.eram.eramEffectiveAddr;
	jitData.eramWriteLatchNext = &esp->shared.eram.eramWriteLatchNext;

  	jitData.eramReadLatch = &esp->shared.eram.eramReadLatch;
	jitData.eramWriteLatch = &esp->shared.eram.eramWriteLatch;
	jitData.eramVarOffset = &esp->shared.eram.eramVarOffset;
	jitData.last_mulInputA_24 = &espCore.last_mulInputA_24;
	jitData.last_mulInputB_24 = &espCore.last_mulInputB_24;

	esp::EspJit jit(m_asm, jitData);

    // m_asm.addDiagnosticOptions(asmjit::DiagnosticOptions::kValidateAssembler);
    // m_asm.addDiagnosticOptions(asmjit::DiagnosticOptions::kValidateIntermediate);

	struct timespec _g0, _g1, _g2, _g3; clock_gettime(CLOCK_MONOTONIC, &_g0);
	jit.jitEnter();

    for (size_t pc = 0; pc < PRAM_SIZE; pc++)
    {
      if (withEram)
        eramEmitter.emit(pc, jit, m_asm);
      emitter->emit(pc, jit, m_asm);
    }

    // logger.log("---- ending ----\n");
    emitter->emitEnd(m_asm);

	jit.jitExit();
	clock_gettime(CLOCK_MONOTONIC, &_g1);

#if JIT_X64
    m_asm.finalize();
#endif
	clock_gettime(CLOCK_MONOTONIC, &_g2);
    
    const auto err = m_rt.add(dest, &code);
	clock_gettime(CLOCK_MONOTONIC, &_g3);
	if (espGenLog()) {
		auto ms = [](const timespec& a, const timespec& b){ return ((b.tv_sec-a.tv_sec)*1e9 + (b.tv_nsec-a.tv_nsec))/1e6; };
		fprintf(stderr, "[genCore %u] emit %.3f finalize %.3f add %.3f ms, %zu bytes\n", core, ms(_g0,_g1), ms(_g1,_g2), ms(_g2,_g3), code.codeSize());
	}
    if (err)
    {
      const auto* const errString = asmjit::DebugUtils::errorAsString(err);
      std::stringstream ss;
      ss << "JIT failed: " << err << " - " << errString;
      const std::string msg(ss.str());
      throw std::runtime_error(msg);
    }
  }

  class ERAMEmitter
  {
  public:
    void init(ESP<lg2eram_size>* _esp)
    {
      esp = _esp;

      eramPCCommit = 0, eramPCStartNext = 0;
      eramModeCurrent = 0, eramModeNext = 0;
      eramImmOffsetAccNext = 0;
      eramActiveCurrent = false, eramActiveNext = false;
      highOffset = false;
    }

    void emit(int pc, esp::EspJit& _jit, esp::Builder& m_asm)
    {
      if (lg2eram_size == 0) return;

      uint32_t *decode = (uint32_t*)(&esp->intmem[0x1000]);
      uint32_t eramCtrl = (decode[pc] >> 23) & 0x1f;
      int stage1 = pc - eramPCStartNext;

      // Transaction start
      if (!eramActiveNext && ((eramCtrl & 0x18) != 0)) {
        eramActiveNext = true;
        eramModeNext = eramCtrl;
        eramPCStartNext = pc;
        eramImmOffsetAccNext = 0;
        stage1 = 0;
        if (eramModeNext & 0x7) printf("wtf %03x at pc=%04x\n", eramCtrl, pc);
      }

      // Accumulate immediates
      else if (eramActiveNext && stage1 <= 4 && stage1 > 0) {
        eramImmOffsetAccNext += eramCtrl << ((stage1 - 1) * 5);
      }

      // Is it time to commit?
      if (eramActiveCurrent && (pc == eramPCCommit)) {
        if (eramModeCurrent == 0x10) emitWrite(_jit, m_asm);
        else emitRead(_jit, m_asm);
        eramActiveCurrent = false; // done
      }

      // Next stage
      if (eramActiveNext && stage1 == 5) { // FIXME: stage1 should be 4, but there are some problems with latching
        if (eramActiveCurrent) printf("ERAM transaction already active at pc %03x\n", pc);
        eramActiveCurrent = true;
        eramModeCurrent = eramModeNext;
        eramPCCommit = eramPCStartNext + ERAM_COMMIT_STAGE;
        eramActiveNext = false;

        // Addr computation
        uint32_t immOffset = eramImmOffsetAccNext;
        bool shouldUseVarOffset = false;
        if (eramModeNext == 0x18)
        {
          immOffset = (eramImmOffsetAccNext >> 1) & 1;
          highOffset = eramImmOffsetAccNext & 0x100;
          shouldUseVarOffset = true;
        }

        emitComputeAddr(_jit, m_asm, immOffset, highOffset, shouldUseVarOffset);
      }
    }

  private:
    // WARNING: shares regs as a core

    void emitWrite(esp::EspJit& _jit, esp::Builder& m_asm)
    {
		_jit.eramWrite(ERAM_MASK);
    }

    void emitRead(esp::EspJit& _jit, esp::Builder& m_asm)
    {
		_jit.eramRead(ERAM_MASK);
    }

    void emitComputeAddr(esp::EspJit& _jit, esp::Builder& m_asm, uint32_t immOffset, bool highOffset, bool shouldUseVarOffset)
    {
		_jit.eramComputeAddr(immOffset, highOffset, shouldUseVarOffset);
    }
  
    uint16_t eramPCCommit = 0, eramPCStartNext = 0;
    uint8_t eramModeCurrent = 0, eramModeNext = 0;
    uint32_t eramImmOffsetAccNext = 0;
    bool eramActiveCurrent = false, eramActiveNext = false;
    bool highOffset = false;

    ESP<lg2eram_size>* esp;
    static constexpr int64_t ERAM_COMMIT_STAGE = 10, ERAM_MASK_FULL = (1 << 19) - 1;
		enum {eram_size = 1 << lg2eram_size, ERAM_MASK = eram_size - 1};
  };
  ERAMEmitter eramEmitter;

  class CoreEmitter
  {
  public:
    void init(ESP<lg2eram_size>* _esp, ESPCore<lg2eram_size>* _core)
    {
      esp = _esp;
      core = _core;

      pre_optimize();
    }

    void emit(int pc, esp::EspJit& _jit, esp::Builder& m_asm)
    {
		const ESPOptInstr &instr = pram_opt[pc];

    	if (instr.opType == kNop) return;

		/* The scan wraps: with the entry/exit state persisted, the "next emitted
		 * op" of the LAST op in a program is the FIRST op of the next call, so a
		 * program whose first op is a DMAC still needs the final last_mul values
		 * written back. */
		bool nextIsDmac = false, foundNext = false;
		for (int i = pc + 1; i < PRAM_SIZE; i++)
		{
			if (pram_opt[i].opType == kNop) continue;
			nextIsDmac = pram_opt[i].opType == kDMAC;
			foundNext = true;
			break;
		}
		if (!foundNext)
		{
			for (int i = 0; i < PRAM_SIZE; i++)
			{
				if (pram_opt[i].opType == kNop) continue;
				nextIsDmac = pram_opt[i].opType == kDMAC;
				break;
			}
		}

		_jit.emitOp(pc, instr, lastMul30, nextIsDmac);

		lastMul30 = (instr.op == 0x30);
    }

    void emitEnd(esp::Builder& m_asm)
    {
      // Store back accumulators
      // m_asm.mov(x8, uint64_t(&acc[0]));
      // m_asm.str(x0, ptr(x8));
      // m_asm.str(x1, ptr(x8, 1 << 3));
      // m_asm.str(x2, ptr(x8, 2 << 3));
      // m_asm.str(x3, ptr(x8, 3 << 3));
      // m_asm.str(x4, ptr(x8, 4 << 3));
      // m_asm.str(x5, ptr(x8, 5 << 3));

			// not done for now, should not be necessary
    }
  
    bool lastMul30 = false;

		void pre_optimize()
		{
			for (int i = 0; i < PRAM_SIZE; i++)
				pram_opt[i] = ESPOptInstr(core->pram[i]);

			/* Decided limitation: op 0x34 with mem & 0xcc == 0xc0 (the jump operations)
			 * is not supported. The whole emitter treats the program as straight-line
			 * code -- pc runs 0..PRAM_SIZE once and the dense ARM64 path also decides
			 * dead stores from the next *statically* emitted op, which is only the next
			 * executed op when there are no branches. This used to be an assert, so it
			 * vanished under NDEBUG in every -Ofast build: a program containing a jump
			 * would then be silently mis-compiled. Report it instead. */
			for (int pc = 0; pc < PRAM_SIZE; pc++)
			{
				if (pram_opt[pc].op == 0xd && (pram_opt[pc].mem & 0xcc) == 0xc0)
				{
					/* Report once and keep going: the compiled program will be wrong,
					 * but aborting the render thread would take the host down with it. */
					static bool reported = false;
					if (!reported)
					{
						reported = true;
						fprintf(stderr, "esp: jump op at pram[%d] (op=%02x mem=%02x); "
						                "the JIT cannot compile jumps, output past here is undefined\n",
						        pc, pram_opt[pc].op, pram_opt[pc].mem);
					}
					break;
				}
			}

			const MemAccess wA = {kNone, false}, wB = {kNone, true}, swA = {kSavesA, false}, swB = {kSavesB, true}, sBwA = {kSavesB, false}, sAwB = {kSavesA, true};

			// Decode DSP instructions and figure out memory accesses
			for (int pc = 0; pc < PRAM_SIZE; pc++)
			{
				ESPOptInstr &o = pram_opt[pc];
				MemAccess a = wA;
				switch (o.op)
				{
				case 0x00: case 0x04: case 0x28: case 0x2c: case 0x50: case 0x54: case 0x60: case 0x64: case 0x70: case 0x74: a = wA; break;
				case 0x08: case 0x38: case 0x40: case 0x44: case 0x48: case 0x4c: case 0x58: case 0x68: case 0x6c: case 0x78: case 0x7c: a = swA; break;
				case 0x0c: case 0x3c: a = sBwA; break;
				case 0x10: case 0x14: a = wB; break;
				case 0x18: a = sAwB; break;
				case 0x1c: case 0x5c: a = swB; break;
				case 0x20: case 0x24: a = (o.shift & 2) ? wB : wA; break;

				case 0x30: if (o.coef & 4) a = (o.coef & 2) ? swB : swA; else a = (o.coef & 2) ? wB : wA; break;
				case 0x34:
					if (o.mem >= 0xa0 && o.mem < 0xb0) a = (o.mem & 1) ? sBwA : swA;
					if (o.mem >= 0xc0) {
						switch (o.mem & 0xf)
						{
							case 0x7: case 0xa: case 0xb: case 0xc: case 0xd: case 0xe: case 0xf: a = (o.mem & 0x20) ? swB : swA; break;
							default: a = (o.mem & 0x20) ? wB : wA; break;
						}
					}
					break;
				default: break;
				}
				bool clr = false;
				switch (o.op)
				{
					case 0x04: case 0x08: case 0x0c: case 0x14: case 0x18: case 0x1c: case 0x24: case 0x44:
					case 0x4c: case 0x50: case 0x64: case 0x6c: case 0x74: case 0x7c: clr = true; break;
					case 0x30: clr = !(o.coef & 1); break;
					case 0x34: if (o.mem >= 0xc0) clr = (o.mem & 0x10); break;
					default: break;
				}
				a.clr = clr;
				if (!o.mem && !o.shift && !o.op && !o.coef) a.nop = true;
				if (o.op == 0x50) a.accGetsUsed = true;
				o.m_access = a;
			}
			
			for (int pc = 0; pc < PRAM_SIZE; pc++)
			{
				if (pram_opt[pc].m_access.nop || !pram_opt[pc].m_access.save) continue;
				// this op saves a value. who generated it?
				bool savesB = (pram_opt[pc].m_access.save == kSavesB);
				int i = pc - 3;
				while (i >= 0) // find the instruction that generated this value.
				{
					if (pram_opt[i].m_access.nop || pram_opt[i].m_access.writesAccB != savesB) {i--; continue;} // this writes to the wrong accumulator. skip.
					pram_opt[i].m_access.accGetsUsed = true; // mark this write as being used.
					pram_opt[pc].m_access.writePC = i;
					break;
				}
			}

			for (int pc = 0; pc < PRAM_SIZE; pc++) // does our mac achieve anything, in theory?
			{
				if (pram_opt[pc].m_access.accGetsUsed) continue; // yes.
				if (pram_opt[pc].m_access.nop) {pram_opt[pc].m_access.nomac = true; continue;} // no.
				int i = pc;
				while (++i < PRAM_SIZE)
				{
					if (pram_opt[i].m_access.nop) continue; // dont care
					if (pram_opt[i].m_access.writesAccB != pram_opt[pc].m_access.writesAccB) continue; // doesn't apply to our acc. skip
					if (pram_opt[i].m_access.clr) {pram_opt[pc].m_access.nomac = true; break;} // we get overwritten
					if (pram_opt[i].m_access.accGetsUsed) break; // the result gets saved.
				}
			}
			
			int rega = 0, regb = 0;
			bool usedrega = false, usedregb = false;
			for (int pc = 0; pc < PRAM_SIZE; pc++) // hand out register numbers
			{
				MemAccess &a = pram_opt[pc].m_access;
				if (pram_opt[pc].m_access.nop) continue;
				if (a.save) a.readReg = pram_opt[a.writePC].m_access.destReg;

				a.srcReg = (a.writesAccB) ? regb : rega;
				
				if (a.writesAccB && usedregb) {regb = (regb + 1) % 3; usedregb = false;}
				if (!a.writesAccB && usedrega) {rega = (rega + 1) % 3; usedrega = false;}

				a.destReg = (a.writesAccB) ? regb : rega;
				if (a.accGetsUsed) (a.writesAccB ? usedregb : usedrega) = true;
			}

			for (int pc = 0; pc < PRAM_SIZE; pc++) // flatten accA and accB into single array
			{
				MemAccess &a = pram_opt[pc].m_access;
				if (a.writesAccB) { a.srcReg += 3; a.destReg += 3; }
				if (a.save == kSavesB) a.readReg += 3;
			}

			int32_t skipfieldPos = 0;
			int32_t skipfieldNeg = 0;
			for (int pc = 0; pc < PRAM_SIZE; pc++) // decode op50 skip (unused for now)
			{
				pram_opt[pc].skippablePos = skipfieldPos & 1;
				pram_opt[pc].skippableNeg = skipfieldNeg & 1;

				skipfieldPos >>= 1;
				skipfieldNeg >>= 1;
				
				if (pram_opt[pc].op == 0x50)
				{
					skipfieldPos |= 0x30;
					skipfieldNeg |= 0x3c0;
				}
			}
		}

    ESP<lg2eram_size>* esp;
    ESPCore<lg2eram_size>* core;

    ESPOptInstr pram_opt[PRAM_SIZE] {};
  };
  CoreEmitter coreEmitter0, coreEmitter1;
};

#endif // JE_ESP_NO_JIT
