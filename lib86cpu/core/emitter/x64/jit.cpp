/*
 * x86-64 emitter
 *
 * ergo720                Copyright (c) 2022
 */

#include "jit.h"
#include "support.h"
#include "instructions.h"
#include <assert.h>

#ifdef LIB86CPU_X64_EMITTER

// The emitted code assumes that host pointers are 8 bytes
static_assert(sizeof(uint8_t *) == 8, "Pointers must be 8 bytes");
// This is assumed in mov dr/reg, reg/dr
static_assert(ZYDIS_REGISTER_DR0 - ZYDIS_REGISTER_DR0 == 0);
static_assert(ZYDIS_REGISTER_DR1 - ZYDIS_REGISTER_DR0 == 1);
static_assert(ZYDIS_REGISTER_DR2 - ZYDIS_REGISTER_DR0 == 2);
static_assert(ZYDIS_REGISTER_DR3 - ZYDIS_REGISTER_DR0 == 3);
static_assert(ZYDIS_REGISTER_DR4 - ZYDIS_REGISTER_DR0 == 4);
static_assert(ZYDIS_REGISTER_DR5 - ZYDIS_REGISTER_DR0 == 5);
static_assert(ZYDIS_REGISTER_DR6 - ZYDIS_REGISTER_DR0 == 6);
static_assert(ZYDIS_REGISTER_DR7 - ZYDIS_REGISTER_DR0 == 7);

#define BAD LIB86CPU_ABORT_msg("Encountered unimplemented instruction %s", log_instr(m_cpu->virt_pc, instr).c_str())

// all regs available on x64
#define AH x86::ah
#define CH x86::ch
#define DH x86::dh
#define BH x86::bh
#define AL x86::al
#define CL x86::cl
#define DL x86::dl
#define BL x86::bl
#define AX x86::ax
#define CX x86::cx
#define DX x86::dx
#define BX x86::bx
#define SP x86::sp
#define BP x86::bp
#define SI x86::si
#define DI x86::di
#define EAX x86::eax
#define ECX x86::ecx
#define EDX x86::edx
#define EBX x86::ebx
#define ESP x86::esp
#define EBP x86::ebp
#define ESI x86::esi
#define EDI x86::edi
#define RAX x86::rax
#define RCX x86::rcx
#define RDX x86::rdx
#define RBX x86::rbx
#define RSP x86::rsp
#define RBP x86::rbp
#define RSI x86::rsi
#define RDI x86::rdi
#define R8B  x86::r8b
#define R9B  x86::r9b
#define R10B x86::r10b
#define R11B x86::r11b
#define R12B x86::r12b
#define R13B x86::r13b
#define R14B x86::r14b
#define R15B x86::r15b
#define R8W  x86::r8w
#define R9W  x86::r9w
#define R10W x86::r10w
#define R11W x86::r11w
#define R12W x86::r12w
#define R13W x86::r13w
#define R14W x86::r14w
#define R15W x86::r15w
#define R8D  x86::r8d
#define R9D  x86::r9d
#define R10D x86::r10d
#define R11D x86::r11d
#define R12D x86::r12d
#define R13D x86::r13d
#define R14D x86::r14d
#define R15D x86::r15d
#define R8  x86::r8
#define R9  x86::r9
#define R10 x86::r10
#define R11 x86::r11
#define R12 x86::r12
#define R13 x86::r13
#define R14 x86::r14
#define R15 x86::r15

// all x64 regs that can actually be used in the main jitted function
enum class x64 : uint32_t {
	rax = 0,
	rcx,
	rdx,
	rdi,
	r8,
	r9,
	r10,
	r11,
	max = r11,
};

constexpr x64
operator|(x64 reg, uint32_t size)
{
	return static_cast<x64>(static_cast<uint32_t>(reg) | (size << static_cast<uint32_t>(x64::max)));
}

static const std::unordered_map<x64, x86::Gp> reg_to_sized_reg = {
	{ x64::rax | SIZE8,   AL   },
	{ x64::rax | SIZE16,  AX   },
	{ x64::rax | SIZE32,  EAX  },
	{ x64::rcx | SIZE8,   CL   },
	{ x64::rcx | SIZE16,  CX   },
	{ x64::rcx | SIZE32,  ECX  },
	{ x64::rdx | SIZE8,   DL   },
	{ x64::rdx | SIZE16,  DX   },
	{ x64::rdx | SIZE32,  EDX  },
	{ x64::rdi | SIZE16,  DI   },
	{ x64::rdi | SIZE32,  EDI  },
	{ x64::r8  | SIZE8,   R8B  },
	{ x64::r8  | SIZE16,  R8W  },
	{ x64::r8  | SIZE32,  R8D  },
	{ x64::r9  | SIZE8,   R9B  },
	{ x64::r9  | SIZE16,  R9W  },
	{ x64::r9  | SIZE32,  R9D  },
	{ x64::r10 | SIZE8,   R10B },
	{ x64::r10 | SIZE16,  R10W },
	{ x64::r10 | SIZE32,  R10D },
	{ x64::r11 | SIZE8,   R11B },
	{ x64::r11 | SIZE16,  R11W },
	{ x64::r11 | SIZE32,  R11D },
};

template<size_t idx>
size_t
get_local_var_offset()
{
	if (idx > (get_jit_local_vars_size() / 8 - 1)) {
		LIB86CPU_ABORT_msg("Attempted to use a local variable for which not enough stack was allocated for");
	}
	else {
		return idx * 8 + get_jit_reg_args_size() + get_jit_stack_args_size();
	}
}

#define LOCAL_VARS_off(idx) get_local_var_offset<idx>()
#define STACK_ARGS_off get_jit_reg_args_size()
#define RCX_HOME_off 8
#define RDX_HOME_off 16
#define R8_HOME_off 24
#define R9_HOME_off 32

// [reg]
#define MEM8(reg)  x86::byte_ptr(reg)
#define MEM16(reg) x86::word_ptr(reg)
#define MEM32(reg) x86::dword_ptr(reg)
#define MEM64(reg) x86::qword_ptr(reg)
#define MEM(reg, size) x86::Mem(reg, size)
// [reg + disp]
#define MEMD8(reg, disp)  x86::byte_ptr(reg, disp)
#define MEMD16(reg, disp) x86::word_ptr(reg, disp)
#define MEMD32(reg, disp) x86::dword_ptr(reg, disp)
#define MEMD64(reg, disp) x86::qword_ptr(reg, disp)
#define MEMD(reg, disp, size) x86::Mem(reg, disp, size)
// [reg + idx * scale], scale specified as 1 << n; e.g. scale = 8 -> n = 3
#define MEMS8(reg, idx, scale)  x86::byte_ptr(reg, idx, scale)
#define MEMS16(reg, idx, scale) x86::word_ptr(reg, idx, scale)
#define MEMS32(reg, idx, scale) x86::dword_ptr(reg, idx, scale)
#define MEMS64(reg, idx, scale) x86::qword_ptr(reg, idx, scale)
#define MEMS(reg, idx, scale, size) x86::Mem(reg, idx, scale, size)
// [idx * scale + disp], scale specified as 1 << n; e.g. scale = 8 -> n = 3
#define MEMSb8(idx, scale, disp)  x86::byte_ptr(disp, idx, scale)
#define MEMSb16(idx, scale, disp) x86::word_ptr(disp, idx, scale)
#define MEMSb32(idx, scale, disp) x86::dword_ptr(disp, idx, scale)
#define MEMSb64(idx, scale, disp) x86::qword_ptr(disp, idx, scale)
#define MEMSb(idx, scale, disp, size) x86::Mem(disp, idx, scale, size)
// [reg + idx * scale + disp], scale specified as 1 << n; e.g. scale = 8 -> n = 3
#define MEMSD8(reg, idx, scale, disp)  x86::byte_ptr(reg, idx, scale, disp)
#define MEMSD16(reg, idx, scale, disp) x86::word_ptr(reg, idx, scale, disp)
#define MEMSD32(reg, idx, scale, disp) x86::dword_ptr(reg, idx, scale, disp)
#define MEMSD64(reg, idx, scale, disp) x86::qword_ptr(reg, idx, scale, disp)
#define MEMSD(reg, idx, scale, disp, size) x86::Mem(reg, idx, scale, disp, size)

#define MOV(dst, src) m_a.mov(dst, src)
#define MOVZX(dst, src) m_a.movzx(dst, src)
#define MOVSX(dst, src) m_a.movsx(dst, src)
#define LEA(dst, src) m_a.lea(dst, src)
#define AND(dst, src) m_a.and_(dst, src)
#define OR(dst, src) m_a.or_(dst, src)
#define XOR(dst, src) m_a.xor_(dst, src)
#define SHL(dst, src) m_a.shl(dst, src)
#define SHR(dst, src) m_a.shr(dst, src)
#define NOT(dst) m_a.not_(dst)
#define TEST(dst, src) m_a.test(dst, src)
#define ADD(dst, src) m_a.add(dst, src)
#define SUB(dst, src) m_a.sub(dst, src)
#define CMP(dst, src) m_a.cmp(dst, src)
#define CALL(addr) m_a.call(addr)
#define RET() m_a.ret()

#define PUSH(dst) m_a.push(dst)
#define POP(dst) m_a.pop(dst)

#define BR_UNCOND(dst) m_a.jmp(dst)
#define BR_EQl(label) Label label ## _taken = m_a.newLabel(); m_a.je(label ## _taken)
#define BR_NEl(label) Label label ## _taken = m_a.newLabel(); m_a.jne(label ## _taken)
#define BR_EQ(label) m_a.je(label ## _taken)
#define BR_NE(label) m_a.jne(label ## _taken)
#define BR_UGTl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.ja(label ## _taken)
#define BR_UGEl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.jae(label ## _taken)
#define BR_ULTl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.jb(label ## _taken)
#define BR_ULEl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.jbe(label ## _taken)
#define BR_SGTl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.jg(label ## _taken)
#define BR_SGEl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.jge(label ## _taken)
#define BR_SLTl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.jl(label ## _taken)
#define BR_SLEl(dst, src, label) Label label ## _taken = m_a.newLabel(); m_a.cmp(dst, src); m_a.jle(label ## _taken)
#define BR_UGT(dst, src, label) m_a.cmp(dst, src); m_a.ja(label ## _taken)
#define BR_UGE(dst, src, label) m_a.cmp(dst, src); m_a.jae(label ## _taken)
#define BR_ULT(dst, src, label) m_a.cmp(dst, src); m_a.jb(label ## _taken)
#define BR_ULE(dst, src, label) m_a.cmp(dst, src); m_a.jbe(label ## _taken)
#define BR_SGT(dst, src, label) m_a.cmp(dst, src); m_a.jg(label ## _taken)
#define BR_SGE(dst, src, label) m_a.cmp(dst, src); m_a.jge(label ## _taken)
#define BR_SLT(dst, src, label) m_a.cmp(dst, src); m_a.jl(label ## _taken)
#define BR_SLE(dst, src, label) m_a.cmp(dst, src); m_a.jle(label ## _taken)

#define CMOV_EQ(dst, src) m_a.cmove(dst, src)
#define CMOV_NE(dst, src) m_a.cmovne(dst, src)

#define LD_R16(dst, reg_offset) MOV(dst, MEMD16(RCX, reg_offset))
#define LD_R32(dst, reg_offset) MOV(dst, MEMD32(RCX, reg_offset))
#define LD_REG_val(dst, reg_offset, size) load_reg(dst, reg_offset, size)
#define LD_SEG(dst, seg_offset) MOV(dst, MEMD16(RCX, seg_offset))
#define LD_SEG_BASE(dst, seg_offset) MOV(dst, MEMD32(RCX, seg_offset + seg_base_offset))
#define LD_SEG_LIMIT(dst, seg_offset) MOV(dst, MEMD32(RCX, seg_offset + seg_limit_offset))
#define ST_R16(reg_offset, src) MOV(MEMD16(RCX, reg_offset), src)
#define ST_R32(reg_offset, src) MOV(MEMD32(RCX, reg_offset), src)
#define ST_REG_val(val, reg_offset, size) store_reg(val, reg_offset, size)
#define ST_SEG(seg_offset, val) MOV(MEMD16(RCX, seg_offset), val)
#define ST_SEG_BASE(seg_offset, val) MOV(MEMD32(RCX, seg_offset + seg_base_offset), val)

#define LD_MEM() load_mem(m_cpu->size_mode, 0)
#define LD_MEMs(size) load_mem(size, 0)
#define ST_MEM(val) store_mem(val, m_cpu->size_mode, 0)
#define ST_MEMs(val, size) store_mem(val, size, 0)

#define ST_IO() store_io(m_cpu->size_mode)

#define LD_CF(dst) MOV(dst, MEMD32(RCX, CPU_CTX_EFLAGS_AUX)); AND(dst, 0x80000000)
#define LD_OF(dst, aux) ld_of(dst, aux)
#define LD_ZF(dst) MOV(dst, MEMD32(RCX, CPU_CTX_EFLAGS_RES))
#define LD_SF(res_dst, aux) ld_sf(res_dst, aux)
#define LD_PF(dst, res, aux) ld_pf(dst, res, aux)
#define LD_AF(dst) MOV(dst, MEMD32(RCX, CPU_CTX_EFLAGS_AUX)); AND(dst, 8)

#define RAISEin_no_param_t() raise_exp_inline_emit<true>()
#define RAISEin_no_param_f() raise_exp_inline_emit<false>()
#define RAISEin_t(addr, code, idx, eip) raise_exp_inline_emit<true>(addr, code, idx, eip)
#define RAISEin_f(addr, code, idx, eip) raise_exp_inline_emit<false>(addr, code, idx, eip)
#define RAISEin0_t(idx) raise_exp_inline_emit<true>(0, 0, idx, m_cpu->instr_eip)
#define RAISEin0_f(idx) raise_exp_inline_emit<false>(0, 0, idx, m_cpu->instr_eip)

#define SIZED_REG(reg, size) reg_to_sized_reg.find(reg | size)->second
#define GET_REG(op) get_register_op(instr, op)
#define GET_OP(op) get_operand(instr, op)
#define GET_IMM() get_immediate_op(instr, OPNUM_SRC)


lc86_jit::lc86_jit(cpu_t *cpu)
{
	m_cpu = cpu;
	_environment = Environment::host();
	_environment.setObjectFormat(ObjectFormat::kJIT);
	gen_int_fn();
}

void
lc86_jit::start_new_session()
{
	m_code.reset();
	m_code.init(_environment);
	m_code.attach(m_a.as<BaseEmitter>());
}

void
lc86_jit::gen_code_block(translated_code_t *tc)
{
	if (auto err = m_code.flatten()) {
		std::string err_str("Asmjit failed at flatten() with the error ");
		err_str += DebugUtils::errorAsString(err);
		throw lc86_exp_abort(err_str, lc86_status::internal_error);
	}

	if (auto err = m_code.resolveUnresolvedLinks()) {
		std::string err_str("Asmjit failed at resolveUnresolvedLinks() with the error ");
		err_str += DebugUtils::errorAsString(err);
		throw lc86_exp_abort(err_str, lc86_status::internal_error);
	}

	size_t estimated_code_size = m_code.codeSize();
	if (estimated_code_size == 0) {
		throw lc86_exp_abort("The generated code has a zero size", lc86_status::internal_error);
	}

#if defined(_WIN64)
	// Increase estimated_code_size by 12 + 12, to accomodate the .pdata and .xdata sections required to unwind the function
	// when an exception is thrown. Note that the sections need to be DWORD aligned
	estimated_code_size += 24;
	estimated_code_size = (estimated_code_size + 3) & ~3;
#endif

	// Increase estimated_code_size by 11, to accomodate the exit function that terminates the execution of this tc.
	// Note that this function should be 16 byte aligned
	estimated_code_size += 11;
	estimated_code_size = (estimated_code_size + 15) & ~15;

	auto block = m_mem.allocate_sys_mem(estimated_code_size);
	if (!block.addr) {
		throw lc86_exp_abort("Failed to allocate memory for the generated code", lc86_status::no_memory);
	}

	if (auto err = m_code.relocateToBase(reinterpret_cast<uintptr_t>(block.addr))) {
		std::string err_str("Asmjit failed at relocateToBase() with the error ");
		err_str += DebugUtils::errorAsString(err);
		throw lc86_exp_abort(err_str, lc86_status::internal_error);
	}

	// NOTE: there should only be a single .text section
	assert(m_code.sectionCount() == 1);

	Section *section = m_code.textSection();
	size_t offset = static_cast<size_t>(section->offset()); // should be zero for the first section
	size_t buff_size = static_cast<size_t>(section->bufferSize());

	assert(offset + buff_size <= estimated_code_size);
	uint8_t *main_offset = static_cast<uint8_t *>(block.addr) + offset;
	std::memcpy(main_offset, section->data(), buff_size);

#if defined(_WIN64)
	// According to asmjit's source code, the code size can decrease after the relocation above, so we need to query it again
	uint8_t *exit_offset = gen_exception_info(main_offset, m_code.codeSize());
#else
	uint8_t *exit_offset = static_cast<uint8_t *>(block.addr) + offset + buff_size;
#endif

	exit_offset = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(exit_offset) + 15) & ~15);


	// Now generate the exit() function. Since it's a leaf function, it doesn't need an exception table on WIN64

	static constexpr uint8_t exit_buff[] = {
		0x48, // rex prefix
		0xB8, // movabs rax, imm64
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0xC3, // ret
	};

	std::memcpy(exit_offset, exit_buff, sizeof(exit_buff));
	*reinterpret_cast<uint64_t *>(exit_offset + 2) = reinterpret_cast<uintptr_t>(tc);

	// This code block is complete, so protect and flush the instruction cache now
	m_mem.protect_sys_mem(block, MEM_READ | MEM_EXEC);

	tc->ptr_code = reinterpret_cast<entry_t>(main_offset);
	tc->jmp_offset[0] = tc->jmp_offset[1] = tc->jmp_offset[2] = reinterpret_cast<entry_t>(exit_offset);
}

void
lc86_jit::gen_int_fn()
{
	// The interrupt function is a leaf function, so it doesn't need an exception table on WIN64

	start_new_session();

	MOV(MEMD8(RCX, CPU_CTX_INT), DL);
	RET();

	if (auto err = m_code.flatten()) {
		std::string err_str("Asmjit failed at flatten() with the error ");
		err_str += DebugUtils::errorAsString(err);
		throw lc86_exp_abort(err_str, lc86_status::internal_error);
	}

	if (auto err = m_code.resolveUnresolvedLinks()) {
		std::string err_str("Asmjit failed at resolveUnresolvedLinks() with the error ");
		err_str += DebugUtils::errorAsString(err);
		throw lc86_exp_abort(err_str, lc86_status::internal_error);
	}

	size_t estimated_code_size = m_code.codeSize();
	if (estimated_code_size == 0) {
		throw lc86_exp_abort("The generated code has a zero size", lc86_status::internal_error);
	}

	auto block = m_mem.allocate_sys_mem(estimated_code_size);
	if (!block.addr) {
		throw lc86_exp_abort("Failed to allocate memory for the generated code", lc86_status::no_memory);
	}

	if (auto err = m_code.relocateToBase(reinterpret_cast<uintptr_t>(block.addr))) {
		std::string err_str("Asmjit failed at relocateToBase() with the error ");
		err_str += DebugUtils::errorAsString(err);
		throw lc86_exp_abort(err_str, lc86_status::internal_error);
	}

	assert(m_code.sectionCount() == 1);

	Section *section = m_code.textSection();
	size_t offset = static_cast<size_t>(section->offset());
	size_t buff_size = static_cast<size_t>(section->bufferSize());

	assert(offset + buff_size <= estimated_code_size);
	std::memcpy(static_cast<uint8_t *>(block.addr) + offset, section->data(), buff_size);

	m_mem.protect_sys_mem(block, MEM_READ | MEM_EXEC);

	m_cpu->int_fn = reinterpret_cast<raise_int_t>(static_cast<uint8_t *>(block.addr) + offset);
}

void
lc86_jit::gen_prologue_main()
{
	// Prolog of our main() function:
	// push rdi
	// sub rsp, 0x20 + sizeof(stack args) + sizeof(local vars)
	// mov [rsp + sizeof(tot stack) + 8 + 8], rcx
	//
	// NOTE1: we don't know yet how much stack we'll need for the function, so we need to patch the correct amount later
	// NOTE2: for sub, always use the 0x81 opcode, since the opcode 0x83 only accepts imm8, and thus can only represents sizes up to 127
	// 48 83 ec 7f             sub    rsp,0x7f
	// 48 81 ec 80 00 00 00    sub    rsp,0x80
	//
	// RCX always holds the cpu_ctx arg, and should never be changed. Prologue and epilog always push and pop RDI, so it's volatile too.
	// Prefer using RAX, RDX, RDI over R8, R9, R10 and R11 to reduce the code size. Offsets from cpu_ctx can be calculated with displacements,
	// to avoid having to use additional add instructions. Local variables on the stack are always allocated at a fixed offset computed at compile time,
	// and the shadow area to spill registers is available too (always allocated by the caller of the jitted function)

	PUSH(RDI);
	m_prolog_patch_offset = m_a.offset();
	m_a.long_().sub(RSP, 0);
	MOV(MEMD64(RSP, get_jit_stack_required() + 8 + RCX_HOME_off), RCX);

	m_needs_epilogue = true;
}

void
lc86_jit::gen_epilogue_main()
{
	ADD(RSP, get_jit_stack_required());
	POP(RDI);
	RET();
}

void
lc86_jit::gen_tail_call(x86::Gp addr)
{
	ADD(RSP, get_jit_stack_required());
	POP(RDI);
	BR_UNCOND(addr);
}

void
lc86_jit::gen_tc_epilogue()
{
	// update the eip if we stopped decoding without a terminating instr
	if (m_cpu->translate_next == 1) {
		assert((DISAS_FLG_PAGE_CROSS | DISAS_FLG_ONE_INSTR) != 0);
		MOV(MEMD32(RCX, CPU_CTX_EIP), m_cpu->virt_pc - m_cpu->cpu_ctx.regs.cs_hidden.base);
	}

	// TC_FLG_INDIRECT, TC_FLG_DIRECT and TC_FLG_DST_ONLY already check for rf/single step, so we only need to check them here with
	// TC_FLG_COND_DST_ONLY or if no linking code was emitted
	if ((m_cpu->tc->flags & TC_FLG_COND_DST_ONLY) || ((m_cpu->tc->flags & TC_FLG_LINK_MASK) == 0)) {
		check_rf_single_step_emit();
	}

	if (m_needs_epilogue) {
		gen_epilogue_main();
	}

	m_a.setOffset(m_prolog_patch_offset);
	m_a.long_().sub(RSP, get_jit_stack_required());
}

template<bool terminates, typename T1, typename T2, typename T3, typename T4>
void lc86_jit::raise_exp_inline_emit(T1 fault_addr, T2 code, T3 idx, T4 eip)
{
	if constexpr (terminates) {
		m_needs_epilogue = false;
		m_cpu->translate_next = 0;
	}

	MOV(MEMD32(RCX, CPU_EXP_ADDR), fault_addr);
	MOV(MEMD16(RCX, CPU_EXP_CODE), code);
	MOV(MEMD16(RCX, CPU_EXP_IDX), idx);
	MOV(MEMD32(RCX, CPU_EXP_EIP), eip);
	MOV(RAX, &cpu_raise_exception<>);
	CALL(RAX);
	gen_epilogue_main();
}

template<bool terminates>
void lc86_jit::raise_exp_inline_emit()
{
	if constexpr (terminates) {
		m_needs_epilogue = false;
		m_cpu->translate_next = 0;
	}

	MOV(RAX, &cpu_raise_exception<>);
	CALL(RAX);
	gen_epilogue_main();
}

void
lc86_jit::raise_exp_inline_emit(uint32_t fault_addr, uint16_t code, uint16_t idx, uint32_t eip)
{
	raise_exp_inline_emit<true>(fault_addr, code, idx, eip);
}

void
lc86_jit::check_int_emit()
{
	MOV(DL, MEMD8(RCX, CPU_CTX_INT));
	MOVZX(EAX, DL);
	MOV(RDI, &m_cpu->tc->jmp_offset[TC_JMP_INT_OFFSET]);
	LEA(RDX, MEMS64(RDI, RAX, 3));
	MOV(RAX, MEM64(RDX));
	CALL(RAX);
}

bool
lc86_jit::check_rf_single_step_emit()
{
	if ((m_cpu->cpu_ctx.regs.eflags & (RF_MASK | TF_MASK)) | (m_cpu->cpu_flags & CPU_SINGLE_STEP)) {

		if (m_cpu->cpu_ctx.regs.eflags & (RF_MASK | TF_MASK)) {
			m_cpu->cpu_flags |= CPU_FORCE_INSERT;
		}

		if (m_cpu->cpu_ctx.regs.eflags & RF_MASK) {
			// clear rf if it is set. This happens in the one-instr tc that contains the instr that originally caused the instr breakpoint. This must be done at runtime
			// because otherwise tc_cache_insert will register rf as clear, when it was set at the beginning of this tc
			MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS));
			AND(EDX, ~RF_MASK);
			MOV(MEMD32(RCX, CPU_CTX_EFLAGS), EDX);
		}

		if ((m_cpu->cpu_ctx.regs.eflags & TF_MASK) | (m_cpu->cpu_flags & CPU_SINGLE_STEP)) {
			// NOTE: if this instr also has a watchpoint, the other DB exp won't be generated
			MOV(EDX, MEMD32(RCX, CPU_CTX_DR6));
			OR(EDX, DR6_BS_MASK);
			MOV(MEMD32(RCX, CPU_CTX_DR6), EDX);
			MOV(EDX, MEMD32(RCX, CPU_CTX_EIP));
			RAISEin_f(0, 0, EXP_DB, EDX);
			return true;
		}
	}

	return false;
}

template<typename T>
void lc86_jit::link_direct_emit(addr_t dst_pc, addr_t *next_pc, T target_pc)
{
	// dst_pc: destination pc, next_pc: pc of next instr, target_addr: pc where instr jumps to at runtime
	// If target_pc is an integral type, then we know already where the instr will jump, and so we can perform the comparisons at compile time
	// and only emit the taken code path. If it's in a reg, it should not be eax, edx or edi

	m_needs_epilogue = false;

	if (check_rf_single_step_emit()) {
		return;
	}

	// make sure we check for interrupts before jumping to the next tc
	check_int_emit();

	// vec_addr: instr_pc, dst_pc, next_pc
	addr_t page_addr = m_cpu->virt_pc & ~PAGE_MASK;
	uint32_t n, dst = (dst_pc & ~PAGE_MASK) == page_addr;
	if (next_pc) {
		n = dst + ((*next_pc & ~PAGE_MASK) == page_addr);
	}
	else {
		n = dst;
	}
	m_cpu->tc->flags |= (n & TC_FLG_NUM_JMP);

	switch (n)
	{
	case 0:
		gen_epilogue_main();
		return;

	case 1: {
		if (next_pc) { // if(dst_pc) -> cond jmp dst_pc; if(next_pc) -> cond jmp next_pc
			if (dst) {
				MOV(RDX, &m_cpu->tc->flags);
				MOV(EDI, MEM32(RDX));
				MOV(EAX, ~TC_FLG_JMP_TAKEN);
				AND(EAX, EDI);
				if constexpr (std::is_integral_v<T>) {
					if (target_pc == dst_pc) {
						MOV(MEM32(RDX), EAX);
						MOV(RDX, &m_cpu->tc->jmp_offset[0]);
						MOV(RAX, MEM64(RDX));
						gen_tail_call(RAX);
					}
					else {
						OR(EAX, TC_JMP_RET << 4);
						MOV(MEM32(RDX), EAX);
						gen_epilogue_main();
					}
				}
				else {
					CMP(target_pc, dst_pc);
					BR_NEl(ret);
					MOV(MEM32(RDX), EAX);
					MOV(RDX, &m_cpu->tc->jmp_offset[0]);
					MOV(RAX, MEM64(RDX));
					gen_tail_call(RAX);
					m_a.bind(ret_taken);
					OR(EAX, TC_JMP_RET << 4);
					MOV(MEM32(RDX), EAX);
					gen_epilogue_main();
				}
			}
			else {
				MOV(RDX, &m_cpu->tc->flags);
				MOV(EDI, MEM32(RDX));
				MOV(EAX, ~TC_FLG_JMP_TAKEN);
				AND(EAX, EDI);
				if constexpr (std::is_integral_v<T>) {
					if (target_pc == *next_pc) {
						OR(EAX, TC_JMP_NEXT_PC << 4);
						MOV(MEM32(RDX), EAX);
						MOV(RDX, &m_cpu->tc->jmp_offset[1]);
						MOV(RAX, MEM64(RDX));
						gen_tail_call(RAX);
					}
					else {
						OR(EAX, TC_JMP_RET << 4);
						MOV(MEM32(RDX), EAX);
						gen_epilogue_main();
					}
				}
				else {
					CMP(target_pc, *next_pc);
					BR_NEl(ret);
					OR(EAX, TC_JMP_NEXT_PC << 4);
					MOV(MEM32(RDX), EAX);
					MOV(RDX, &m_cpu->tc->jmp_offset[1]);
					MOV(RAX, MEM64(RDX));
					gen_tail_call(RAX);
					m_a.bind(ret_taken);
					OR(EAX, TC_JMP_RET << 4);
					MOV(MEM32(RDX), EAX);
					gen_epilogue_main();
				}
			}
		}
		else { // uncond jmp dst_pc
			MOV(RDX, &m_cpu->tc->jmp_offset[0]);
			MOV(RAX, MEM64(RDX));
			gen_tail_call(RAX);
		}
	}
	break;

	case 2: { // cond jmp next_pc + uncond jmp dst_pc
		MOV(RDX, &m_cpu->tc->flags);
		MOV(EDI, MEM32(RDX));
		MOV(EAX, ~TC_FLG_JMP_TAKEN);
		AND(EAX, EDI);
		if constexpr (std::is_integral_v<T>) {
			if (target_pc == *next_pc) {
				OR(EAX, TC_JMP_NEXT_PC << 4);
				MOV(MEM32(RDX), EAX);
				MOV(RDX, &m_cpu->tc->jmp_offset[1]);
				MOV(RAX, MEM64(RDX));
				gen_tail_call(RAX);
			}
			else {
				MOV(MEM32(RDX), EAX);
				MOV(RDX, &m_cpu->tc->jmp_offset[0]);
				MOV(RAX, MEM64(RDX));
				gen_tail_call(RAX);
			}
		}
		else {
			CMP(target_pc, *next_pc);
			BR_NEl(ret);
			OR(EAX, TC_JMP_NEXT_PC << 4);
			MOV(MEM32(RDX), EAX);
			MOV(RDX, &m_cpu->tc->jmp_offset[1]);
			MOV(RAX, MEM64(RDX));
			gen_tail_call(RAX);
			m_a.bind(ret_taken);
			MOV(MEM32(RDX), EAX);
			MOV(RDX, &m_cpu->tc->jmp_offset[0]);
			MOV(RAX, MEM64(RDX));
			gen_tail_call(RAX);
		}
	}
	break;

	default:
		LIB86CPU_ABORT();
	}
}

void
lc86_jit::link_dst_only_emit()
{
	m_needs_epilogue = false;

	if (check_rf_single_step_emit()) {
		return;
	}

	// make sure we check for interrupts before jumping to the next tc
	check_int_emit();

	m_cpu->tc->flags |= (1 & TC_FLG_NUM_JMP);

	MOV(RDX, &m_cpu->tc->jmp_offset[0]);
	MOV(RAX, MEM64(RDX));
	gen_tail_call(RAX);
}

void
lc86_jit::link_indirect_emit()
{
	m_needs_epilogue = false;

	if (check_rf_single_step_emit()) {
		return;
	}

	// make sure we check for interrupts before jumping to the next tc
	check_int_emit();

	MOV(RDX, m_cpu->tc);
	MOV(RAX, &link_indirect_handler);
	CALL(RAX);
	gen_tail_call(RAX);
}

op_info
lc86_jit::get_operand(ZydisDecodedInstruction *instr, const unsigned opnum)
{
	ZydisDecodedOperand *operand = &instr->operands[opnum];

	switch (operand->type)
	{
	case ZYDIS_OPERAND_TYPE_MEMORY: // final 32 bit addr in edx
	{
		switch (operand->encoding)
		{
		case ZYDIS_OPERAND_ENCODING_DISP16_32_64:
			LD_SEG_BASE(EDX, REG_off(operand->mem.segment));
			ADD(EDX, operand->mem.disp.value);
			return {};

		case ZYDIS_OPERAND_ENCODING_MODRM_RM: {
			if (instr->address_width == 32) {
				if (operand->mem.base != ZYDIS_REGISTER_NONE) {
					LD_R32(EAX, REG_off(operand->mem.base));
				}
				else {
					XOR(EAX, EAX);
				}

				if (operand->mem.scale != 0) {
					// asmjit wants the scale expressed as indexed value scale = 1 << n, so don't use operand->mem.scale
					LD_R32(EDI, REG_off(operand->mem.index));
					LEA(EAX, MEMS32(EAX, EDI, instr->raw.sib.scale));
				}

				if (operand->mem.disp.has_displacement) {
					if (instr->raw.modrm.mod == 1) {
						MOV(EDX, static_cast<int32_t>(static_cast<int8_t>(operand->mem.disp.value)));
					}
					else {
						MOV(EDX, operand->mem.disp.value);
					}

					LD_SEG_BASE(EDI, REG_off(operand->mem.segment));
					LEA(EDX, MEMS32(EDX, EAX, 0));
					ADD(EDX, EDI);
					return {};
				}

				LD_SEG_BASE(EDX, REG_off(operand->mem.segment));
				ADD(EDX, EAX);
				return {};
			}
			else {
				XOR(EAX, EAX);
				if (operand->mem.base != ZYDIS_REGISTER_NONE) {
					LD_R16(AX, REG_off(operand->mem.base));
				}

				if (operand->mem.scale != 0) {
					// asmjit wants the scale expressed as indexed value scale = 1 << n, so don't use operand->mem.scale
					MOVZX(EDI, MEMD16(RCX, REG_off(operand->mem.index)));
					LEA(AX, MEMS16(EAX, EDI, instr->raw.sib.scale));
				}

				if (operand->mem.disp.has_displacement) {
					if (instr->raw.modrm.mod == 1) {
						MOV(EDX, static_cast<int16_t>(static_cast<int8_t>(operand->mem.disp.value)));
					}
					else {
						MOV(EDX, operand->mem.disp.value);
					}

					LD_SEG_BASE(EDI, REG_off(operand->mem.segment));
					LEA(EDX, MEMS32(EDX, EAX, 0));
					ADD(EDX, EDI);
					return {};
				}

				LD_SEG_BASE(EDX, REG_off(operand->mem.segment));
				ADD(EDX, EAX);
				return {};
			}
		}
		break;

		default:
			LIB86CPU_ABORT_msg("Unhandled mem operand encoding %d in %s", operand->encoding, __func__);
		}
	}
	break;

	case ZYDIS_OPERAND_TYPE_REGISTER: { // op_info with reg offset and bit size
		size_t offset = REG_off(operand->reg.value);
		switch (operand->size)
		{
		case 8:
			return { offset, SIZE8 };

		case 16:
			return { offset, SIZE16 };

		case 32:
			return { offset, SIZE32 };

		default:
			LIB86CPU_ABORT();
		}
	}
	break;

	case ZYDIS_OPERAND_TYPE_POINTER:
		LIB86CPU_ABORT_msg("Segment and offset of pointer type operand should be read directly by the translator instead of from %s", __func__);
		break;

	case ZYDIS_OPERAND_TYPE_IMMEDIATE: { // op_info with imm value and bit size
		switch (operand->encoding)
		{
		case ZYDIS_OPERAND_ENCODING_UIMM16:
			return { operand->imm.value.u, SIZE16 };

		case ZYDIS_OPERAND_ENCODING_UIMM8:
		case ZYDIS_OPERAND_ENCODING_JIMM8:
			return { operand->imm.value.u, SIZE8 };

		case ZYDIS_OPERAND_ENCODING_JIMM16_32_32:
			if (operand->size == 32) {
				return { operand->imm.value.u, SIZE32 };
			}
			else {
				return { operand->imm.value.u, SIZE16 };
			}

		default:
			LIB86CPU_ABORT_msg("Unhandled imm operand encoding %d in %s", operand->encoding, __func__);
		}
	}

	default:
		LIB86CPU_ABORT_msg("Unhandled operand type specified");
	}
}

op_info
lc86_jit::get_register_op(ZydisDecodedInstruction *instr, const unsigned opnum)
{
	assert(instr->operands[opnum].type == ZYDIS_OPERAND_TYPE_REGISTER);
	return get_operand(instr, opnum);
}

uint32_t
lc86_jit::get_immediate_op(ZydisDecodedInstruction *instr, const unsigned opnum)
{
	assert(instr->operands[opnum].type == ZYDIS_OPERAND_TYPE_IMMEDIATE);
	return instr->operands[opnum].imm.value.u;
}

template<unsigned opnum, typename T1, typename T2>
auto lc86_jit::get_rm(ZydisDecodedInstruction *instr, T1 &&reg, T2 &&mem)
{
	const op_info rm = GET_OP(opnum);
	switch (instr->operands[opnum].type)
	{
	case ZYDIS_OPERAND_TYPE_REGISTER:
		return reg(rm);

	case ZYDIS_OPERAND_TYPE_MEMORY:
		return mem(rm);

	default:
		LIB86CPU_ABORT_msg("Invalid operand type used in %s!", __func__); \
	}
}

template<unsigned size, typename T>
void lc86_jit::gen_sum_vec16_8(x86::Gp a, T b, x86::Gp sum)
{
	// a: cx/cl, b: dx/dl or imm16/8, sum: r8w/r8b

	MOVZX(R9D, a);
	if constexpr (std::is_integral_v<T>) {
		MOV(EAX, b);
	}
	else {
		MOVZX(EAX, b);
	}
	OR(R9D, EAX);
	MOVZX(ECX, a);
	MOVZX(EAX, sum);
	NOT(EAX);
	AND(R9D, EAX);
	if constexpr (std::is_integral_v<T>) {
		MOV(EAX, b);
	}
	else {
		MOVZX(EAX, b);
	}
	AND(ECX, EAX);
	OR(R9D, ECX);
	MOV(EAX, R9D);
	SHL(EAX, size == SIZE16 ? 16 : 24);
	OR(EAX, R9D);
	AND(EAX, 0xC0000008);
}

template<typename T>
void lc86_jit::gen_sum_vec32(T b)
{
	// a: ecx, b: edx or imm32, sum: r8d

	MOV(EAX, ECX);
	NOT(R8D);
	OR(EAX, b);
	AND(ECX, b);
	AND(EAX, R8D);
	OR(EAX, ECX);
	AND(EAX, 0xC0000008);
}

template<unsigned size, typename T>
void lc86_jit::gen_sub_vec16_8(x86::Gp a, T b, x86::Gp sub)
{
	// a: cx/cl, b: dx/dl or imm16/8, sub: r8w/r8b

	MOVZX(R9D, a);
	if constexpr (std::is_integral_v<T>) {
		MOV(EAX, b);
	}
	else {
		MOVZX(EAX, b);
	}
	XOR(R9D, EAX);
	MOVZX(ECX, a);
	NOT(R9D);
	MOVZX(EAX, sub);
	AND(R9D, EAX);
	NOT(ECX);
	if constexpr (std::is_integral_v<T>) {
		MOV(EAX, b);
	}
	else {
		MOVZX(EAX, b);
	}
	AND(ECX, EAX);
	OR(R9D, ECX);
	MOV(EAX, R9D);
	SHL(EAX, size == SIZE16 ? 16 : 24);
	OR(EAX, R9D);
	AND(EAX, 0xC0000008);
}

template<typename T>
void lc86_jit::gen_sub_vec32(T b)
{
	// a: ecx, b: edx or imm32, sub: r8d

	MOV(EAX, ECX);
	NOT(ECX);
	XOR(EAX, b);
	AND(ECX, b);
	NOT(EAX);
	AND(EAX, R8D);
	OR(EAX, ECX);
	AND(EAX, 0xC0000008);
}

template<typename T>
void lc86_jit::set_flags_sum(x86::Gp a, T b, x86::Gp sum)
{
	// a: reg, b: edx/dx/dl or imm32/16/8, sum: r8d/r8w/r8b

	MOV(R10, RCX);

	switch (m_cpu->size_mode)
	{
	case SIZE8:
		MOV(CL, a);
		gen_sum_vec16_8<SIZE8>(CL, b, sum);
		MOVSX(R8D, sum);
		break;

	case SIZE16:
		MOV(CX, a);
		gen_sum_vec16_8<SIZE16>(CX, b, sum);
		MOVSX(R8D, sum);
		break;

	case SIZE32:
		MOV(ECX, a);
		gen_sum_vec32(b);
		break;

	default:
		LIB86CPU_ABORT_msg("Invalid size_mode \"%c\" used in %s", m_cpu->size_mode, __func__);
	}

	MOV(MEMD32(R10, CPU_CTX_EFLAGS_RES), R8D);
	MOV(MEMD32(R10, CPU_CTX_EFLAGS_AUX), EAX);
	MOV(RCX, R10);
}

template<typename T>
void lc86_jit::set_flags_sub(x86::Gp a, T b, x86::Gp sub)
{
	// a: reg, b: edx/dx/dl or imm32/16/8, sub: r8d/r8w/r8b

	MOV(R10, RCX);

	switch (m_cpu->size_mode)
	{
	case SIZE8:
		MOV(CL, a);
		gen_sub_vec16_8<SIZE8>(CL, b, sub);
		MOVSX(R8D, sub);
		break;

	case SIZE16:
		MOV(CX, a);
		gen_sub_vec16_8<SIZE16>(CX, b, sub);
		MOVSX(R8D, sub);
		break;

	case SIZE32:
		MOV(ECX, a);
		gen_sub_vec32(b);
		break;

	default:
		LIB86CPU_ABORT_msg("Invalid size_mode \"%c\" used in %s", m_cpu->size_mode, __func__);
	}

	MOV(MEMD32(R10, CPU_CTX_EFLAGS_RES), R8D);
	MOV(MEMD32(R10, CPU_CTX_EFLAGS_AUX), EAX);
	MOV(RCX, R10);
}

template<x86::Gp res_32reg, typename T1, typename T2>
void lc86_jit::set_flags(T1 res, T2 aux, size_t size)
{
	if (size != SIZE32) {
		if constexpr (std::is_integral_v<T1>) {
			int32_t res1 = static_cast<int32_t>(res);
			MOV(MEMD32(RCX, CPU_CTX_EFLAGS_RES), res1);
		}
		else {
			MOVSX(res_32reg, res);
			MOV(MEMD32(RCX, CPU_CTX_EFLAGS_RES), res);
		}
	}
	else {
		MOV(MEMD32(RCX, CPU_CTX_EFLAGS_RES), res);
	}

	MOV(MEMD32(RCX, CPU_CTX_EFLAGS_AUX), aux);
}

void
lc86_jit::ld_of(x86::Gp dst, x86::Gp aux)
{
	LEA(dst, MEMS32(aux, aux, 0));
	XOR(dst, aux);
	AND(dst, 0x80000000);
}

void
lc86_jit::ld_sf(x86::Gp res_dst, x86::Gp aux)
{
	SHR(res_dst, 0x1F);
	AND(aux, 1);
	XOR(res_dst, aux);
}

void
lc86_jit::ld_pf(x86::Gp dst, x86::Gp res, x86::Gp aux)
{
	MOV(dst, res);
	MOV(R8D, aux);
	SHR(R8, 8);
	LEA(res.r64(), MEMD64(RCX, CPU_CTX_EFLAGS_PAR));
	XOR(R8, dst.r64());
	MOVZX(dst, R8B);
	MOVZX(dst, MEMS8(dst.r64(), res.r64(), 0));
}

void
lc86_jit::load_reg(x86::Gp dst, size_t reg_offset, size_t size)
{
	switch (size)
	{
	case SIZE8:
		MOV(dst, MEMD8(RCX, reg_offset));
		break;

	case SIZE16:
		MOV(dst, MEMD16(RCX, reg_offset));
		break;

	case SIZE32:
		MOV(dst, MEMD32(RCX, reg_offset));
		break;

	default:
		LIB86CPU_ABORT();
	}
}

template<typename T>
void lc86_jit::store_reg(T val, size_t reg_offset, size_t size)
{
	switch (size)
	{
	case SIZE8:
		MOV(MEMD8(RCX, reg_offset), val);
		break;

	case SIZE16:
		MOV(MEMD16(RCX, reg_offset), val);
		break;

	case SIZE32:
		MOV(MEMD32(RCX, reg_offset), val);
		break;

	default:
		LIB86CPU_ABORT();
	}
}

void
lc86_jit::load_mem(uint8_t size, uint8_t is_priv)
{
	// RCX: cpu_ctx, EDX: addr, R8: instr_eip, R9B: is_priv

	MOV(R9B, is_priv);
	MOV(R8D, m_cpu->instr_eip);

	switch (size)
	{
	case SIZE32:
		MOV(RAX, &mem_read_helper<uint32_t>);
		break;

	case SIZE16:
		MOV(RAX, &mem_read_helper<uint16_t>);
		break;

	case SIZE8:
		MOV(RAX, &mem_read_helper<uint8_t>);
		break;

	default:
		LIB86CPU_ABORT();
	}

	CALL(RAX);
	MOV(RCX, MEMD64(RSP, get_jit_stack_required() + 8 + RCX_HOME_off));
}

template<typename T>
void lc86_jit::store_mem(T val, uint8_t size, uint8_t is_priv)
{
	// RCX: cpu_ctx, EDX: addr, R8B/R8W/R8D: val, R9D: instr_eip, stack: is_priv

	MOV(MEMD32(RSP, STACK_ARGS_off), is_priv);
	MOV(R9D, m_cpu->instr_eip);

	switch (size)
	{
	case SIZE32:
		MOV(R8D, val);
		MOV(RAX, &mem_write_helper<uint32_t>);
		break;

	case SIZE16:
		MOV(R8W, val);
		MOV(RAX, &mem_write_helper<uint16_t>);
		break;

	case SIZE8:
		MOV(R8B, val);
		MOV(RAX, &mem_write_helper<uint8_t>);
		break;

	default:
		LIB86CPU_ABORT();
	}

	CALL(RAX);
	MOV(RCX, MEMD64(RSP, get_jit_stack_required() + 8 + RCX_HOME_off));
}

void
lc86_jit::store_io(uint8_t size_mode)
{
	// RCX: cpu_ctx, EDX: port, R8B/R8W/R8D: val
	// register val, should have been placed in EAX/AX/AL by load_reg or something else

	switch (size_mode)
	{
	case SIZE32:
		MOV(R8D, EAX);
		MOV(RAX, &io_write_helper<uint32_t>);
		break;

	case SIZE16:
		MOV(R8W, AX);
		MOV(RAX, &io_write_helper<uint16_t>);
		break;

	case SIZE8:
		MOV(R8B, AL);
		MOV(RAX, &io_write_helper<uint8_t>);
		break;

	default:
		LIB86CPU_ABORT();
	}

	CALL(RAX);
	MOV(RCX, MEMD64(RSP, get_jit_stack_required() + 8 + RCX_HOME_off));
}

template<typename T>
bool lc86_jit::check_io_priv_emit(T port)
{
	// port is either an immediate or in EDX

	static const uint8_t op_size_to_mem_size[3] = { 4, 2, 1 };

	if ((m_cpu->cpu_ctx.hflags & HFLG_PE_MODE) && ((m_cpu->cpu_ctx.hflags & HFLG_CPL) > ((m_cpu->cpu_ctx.regs.eflags & IOPL_MASK) >> 12))) {
		LD_SEG_BASE(R10D, TR_idx);
		LD_SEG_LIMIT(R11D, TR_idx);
		BR_ULTl(R11D, 103, exp);
		if constexpr (!std::is_integral_v<T>) {
			MOV(MEMD32(RSP, LOCAL_VARS_off(0)), EDX);
		}
		ADD(R10D, 102);
		MOV(EDX, R10D);
		LD_MEMs(SIZE16);
		MOVZX(EAX, AX);
		if constexpr (std::is_integral_v<T>) {
			ADD(EAX, (port >> 3) + 1);
		}
		else {
			MOV(EDX, MEMD32(RSP, LOCAL_VARS_off(0)));
			SHR(EDX, 3);
			LEA(EAX, MEMSD32(EAX, EDX, 0, 1));
		}
		BR_UGT(EAX, R11D, exp);
		ADD(EAX, R10D);
		MOV(EDX, EAX);
		LD_MEMs(SIZE16);
		MOVZX(EAX, AX);
		if constexpr (std::is_integral_v<T>) {
			SHR(EAX, port & 7);
		}
		else {
			MOV(EDX, MEMD32(RSP, LOCAL_VARS_off(0)));
			AND(EDX, 7);
			MOV(RDI, RCX);
			MOV(ECX, EDX);
			SHR(EAX, CL);
			MOV(RCX, RDI);
		}
		AND(EAX, (1 << op_size_to_mem_size[m_cpu->size_mode]) - 1);
		BR_NE(exp);
		Label ok = m_a.newLabel();
		BR_UNCOND(ok);
		m_a.bind(exp_taken);
		RAISEin0_f(EXP_GP);
		m_a.bind(ok);
		return true;
	}

	return false;
}

void
lc86_jit::cli(ZydisDecodedInstruction *instr)
{
	assert(instr->opcode == 0xFA);

	if (m_cpu->cpu_ctx.hflags & HFLG_PE_MODE) {

		// we don't support virtual 8086 mode, so we don't need to check for it
		if (((m_cpu->cpu_ctx.regs.eflags & IOPL_MASK) >> 12) >= (m_cpu->cpu_ctx.hflags & HFLG_CPL)) {
			LD_R32(EDX, CPU_CTX_EFLAGS);
			AND(EDX, ~IF_MASK);
			ST_R32(CPU_CTX_EFLAGS, EDX);
		}
		else {
			RAISEin0_t(EXP_GP);
		}
	}
	else {
		LD_R32(EDX, CPU_CTX_EFLAGS);
		AND(EDX, ~IF_MASK);
		ST_R32(CPU_CTX_EFLAGS, EDX);
	}
}

void
lc86_jit::inc(ZydisDecodedInstruction *instr)
{
	switch (instr->opcode)
	{
	case 0xFE:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0x40:
	case 0x41:
	case 0x42:
	case 0x43:
	case 0x44:
	case 0x45:
	case 0x46:
	case 0x47:
	case 0xFF: {
		size_t size = get_rm<OPNUM_SINGLE>(instr,
			[this](const op_info rm)
			{
				auto sum_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_REG_val(sum_host_reg, rm.val, rm.bits);
				MOV(MEMD(RSP, LOCAL_VARS_off(0), rm.bits), sum_host_reg);
				ADD(sum_host_reg, 1);
				MOV(MEMD(RSP, LOCAL_VARS_off(1), rm.bits), sum_host_reg);
				ST_REG_val(sum_host_reg, rm.val, rm.bits);
				return rm.bits;
			},
			[this](const op_info rm)
			{
				auto sum_host_reg = SIZED_REG(x64::rax, rm.bits);
				MOV(EDI, EDX);
				LD_MEM();
				MOV(MEMD(RSP, LOCAL_VARS_off(0), rm.bits), sum_host_reg);
				ADD(sum_host_reg, 1);
				MOV(MEMD(RSP, LOCAL_VARS_off(1), rm.bits), sum_host_reg);
				MOV(EDX, EDI);
				ST_MEM(sum_host_reg);
				return rm.bits;
			});

		LD_CF(EDI);
		auto sum_host_reg = SIZED_REG(x64::r8, size);
		auto a_host_reg = SIZED_REG(x64::rax, size);
		MOV(a_host_reg, MEMD(RSP, LOCAL_VARS_off(0), size));
		MOV(sum_host_reg, MEMD(RSP, LOCAL_VARS_off(1), size));
		set_flags_sum(a_host_reg, 1, sum_host_reg);
		MOV(EAX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_OF(EDX, EAX);
		XOR(EDX, EDI);
		SHR(EDX, 1);
		OR(EDX, EDI);
		AND(EAX, 0x3FFFFFFF);
		OR(EDX, EAX);
		MOV(MEMD32(RCX, CPU_CTX_EFLAGS_AUX), EDX);
	}
	break;

	default:
		LIB86CPU_ABORT();
	}
}

void
lc86_jit::jcc(ZydisDecodedInstruction *instr)
{
	addr_t next_eip = m_cpu->instr_eip + m_cpu->instr_bytes;
	addr_t jmp_eip = next_eip + instr->operands[OPNUM_SINGLE].imm.value.s;
	if (m_cpu->size_mode == SIZE16) {
		jmp_eip &= 0x0000FFFF;
	}
	addr_t next_pc = next_eip + m_cpu->cpu_ctx.regs.cs_hidden.base;
	addr_t dst_pc = jmp_eip + m_cpu->cpu_ctx.regs.cs_hidden.base;

	switch (instr->opcode)
	{
	case 0x70:
	case 0x80:
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_OF(EAX, EDX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0x80000000);
		CMOV_EQ(R9D, EDI); // OF != 0
		break;

	case 0x71:
	case 0x81:
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_OF(EAX, EDX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0);
		CMOV_EQ(R9D, EDI); // OF == 0
		break;

	case 0x72:
	case 0x82:
		LD_CF(EAX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0x80000000);
		CMOV_EQ(R9D, EDI); // CF != 0
		break;

	case 0x73:
	case 0x83:
		LD_CF(EAX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0);
		CMOV_EQ(R9D, EDI); // CF == 0
		break;

	case 0x74:
	case 0x84:
		LD_ZF(EAX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0);
		CMOV_EQ(R9D, EDI); // ZF != 0
		break;

	case 0x75:
	case 0x85:
		LD_ZF(EAX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0);
		CMOV_NE(R9D, EDI); // ZF == 0
		break;

	case 0x76:
	case 0x86:
		LD_CF(EAX);
		MOV(EDX, 0);
		MOV(EDI, 1);
		CMP(EAX, 0x80000000);
		CMOV_EQ(EDX, EDI);
		MOV(EAX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_ZF(EAX);
		MOV(R8D, 0);
		CMP(EAX, 0);
		CMOV_EQ(R8D, EDI);
		OR(EDX, R8D);
		CMP(EDX, 1);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMOV_EQ(R9D, EDI); // CF != 0 OR ZF != 0
		break;

	case 0x77:
	case 0x87:
		LD_CF(EAX);
		MOV(EDX, 0);
		MOV(EDI, 1);
		CMP(EAX, 0);
		CMOV_EQ(EDX, EDI);
		MOV(EAX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_ZF(EAX);
		MOV(R8D, 0);
		CMP(EAX, 0);
		CMOV_NE(R8D, EDI);
		AND(EDX, R8D);
		CMP(EDX, 1);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMOV_EQ(R9D, EDI); // CF == 0 AND ZF == 0
		break;

	case 0x78:
	case 0x88:
		MOV(EAX, MEMD32(RCX, CPU_CTX_EFLAGS_RES));
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_SF(EAX, EDX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 1);
		CMOV_EQ(R9D, EDI); // SF != 0
		break;

	case 0x79:
	case 0x89:
		MOV(EAX, MEMD32(RCX, CPU_CTX_EFLAGS_RES));
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_SF(EAX, EDX);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0);
		CMOV_EQ(R9D, EDI); // SF == 0
		break;

	case 0x7A:
	case 0x8A:
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_RES));
		MOV(EDI, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_PF(EAX, EDX, EDI);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0);
		CMOV_EQ(R9D, EDI); // PF != 0
		break;

	case 0x7B:
	case 0x8B:
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_RES));
		MOV(EDI, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_PF(EAX, EDX, EDI);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 1);
		CMOV_EQ(R9D, EDI); // PF == 0
		break;

	case 0x7C:
	case 0x8C:
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_OF(EAX, EDX);
		MOV(EDI, MEMD32(RCX, CPU_CTX_EFLAGS_RES));
		LD_SF(EDI, EDX);
		SHR(EAX, 0x1F);
		XOR(EAX, EDI);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 1);
		CMOV_EQ(R9D, EDI); // SF != OF
		break;

	case 0x7D:
	case 0x8D:
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_OF(EAX, EDX);
		MOV(EDI, MEMD32(RCX, CPU_CTX_EFLAGS_RES));
		LD_SF(EDI, EDX);
		SHR(EAX, 0x1F);
		XOR(EAX, EDI);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMP(EAX, 0);
		CMOV_EQ(R9D, EDI); // SF == OF
		break;

	case 0x7E:
	case 0x8E:
		LD_ZF(EAX);
		MOV(R8D, 0);
		MOV(EDI, 1);
		CMP(EAX, 0);
		CMOV_EQ(R8D, EDI);
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_OF(EDI, EDX);
		LD_SF(EAX, EDX);
		SHR(EDI, 0x1F);
		XOR(EAX, EDI);
		MOV(R9D, 0);
		MOV(EDI, 1);
		CMP(EAX, 1);
		CMOV_EQ(R9D, EDI);
		OR(R8D, R9D);
		CMP(R8D, 1);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMOV_EQ(R9D, EDI); // ZF != 0 OR SF != OF
		break;

	case 0x7F:
	case 0x8F:
		LD_ZF(EAX);
		MOV(R8D, 0);
		MOV(EDI, 1);
		CMP(EAX, 0);
		CMOV_NE(R8D, EDI);
		MOV(EDX, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
		LD_OF(EDI, EDX);
		LD_SF(EAX, EDX);
		SHR(EDI, 0x1F);
		XOR(EAX, EDI);
		MOV(R9D, 0);
		MOV(EDI, 1);
		CMP(EAX, 0);
		CMOV_EQ(R9D, EDI);
		AND(R8D, R9D);
		CMP(R8D, 1);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMOV_EQ(R9D, EDI); // ZF == 0 AND SF == OF
		break;

	case 0xE3:
		if (m_cpu->addr_mode == ADDR16) {
			MOVZX(EAX, MEMD16(RCX, CPU_CTX_ECX));
		}
		else {
			LD_R32(EAX, CPU_CTX_ECX);
		}
		CMP(EAX, 0);
		MOV(R9D, next_eip);
		MOV(EDI, jmp_eip);
		CMOV_EQ(R9D, EDI); // ECX == 0
		break;

	default:
		LIB86CPU_ABORT();
	}

	MOV(MEMD32(RCX, CPU_CTX_EIP), R9D);
	ADD(R9D, m_cpu->cpu_ctx.regs.cs_hidden.base);
	link_direct_emit(dst_pc, &next_pc, R9D);

	m_cpu->tc->flags |= TC_FLG_DIRECT;
	m_cpu->translate_next = 0;
}

void
lc86_jit::jmp(ZydisDecodedInstruction *instr)
{
	switch (instr->opcode)
	{
	case 0xE9:
	case 0xEB: {
		addr_t new_eip = (m_cpu->virt_pc - m_cpu->cpu_ctx.regs.cs_hidden.base) + m_cpu->instr_bytes + instr->operands[OPNUM_SINGLE].imm.value.s;
		if (m_cpu->size_mode == SIZE16) {
			new_eip &= 0x0000FFFF;
		}
		ST_R32(CPU_CTX_EIP, new_eip);
		link_direct_emit(m_cpu->cpu_ctx.regs.cs_hidden.base + new_eip, nullptr, m_cpu->cpu_ctx.regs.cs_hidden.base + new_eip);
		m_cpu->tc->flags |= TC_FLG_DIRECT;
	}
	break;

	case 0xEA: {
		addr_t new_eip = instr->operands[OPNUM_SINGLE].ptr.offset;
		uint16_t new_sel = instr->operands[OPNUM_SINGLE].ptr.segment;
		if (m_cpu->cpu_ctx.hflags & HFLG_PE_MODE) {
			MOV(RDI, RCX);
			MOV(MEMD32(RSP, STACK_ARGS_off), m_cpu->instr_eip);
			MOV(R9D, new_eip);
			MOV(R8B, m_cpu->size_mode);
			MOV(EDX, new_sel);
			MOV(RAX, &ljmp_pe_helper);
			CALL(RAX);
			MOV(RCX, RDI);
			CMP(AL, 0);
			BR_NEl(exp);
			link_indirect_emit();
			m_a.bind(exp_taken);
			RAISEin_no_param_f();
			m_cpu->tc->flags |= TC_FLG_INDIRECT;
		}
		else {
			new_eip = m_cpu->size_mode == SIZE16 ? new_eip & 0xFFFF : new_eip;
			ST_R16(CPU_CTX_CS, new_sel);
			ST_R32(CPU_CTX_EIP, new_eip);
			ST_R32(CPU_CTX_CS_BASE, static_cast<uint32_t>(new_sel) << 4);
			link_direct_emit((static_cast<uint32_t>(new_sel) << 4) + new_eip, nullptr, (static_cast<uint32_t>(new_sel) << 4) + new_eip);
			m_cpu->tc->flags |= TC_FLG_DIRECT;
		}
	}
	break;

	case 0xFF: {
		if (instr->raw.modrm.reg == 4) {
			get_rm<OPNUM_SINGLE>(instr,
				[this](const op_info rm)
				{
					LD_REG_val(SIZED_REG(x64::rax, m_cpu->size_mode), rm.val, rm.bits);
				},
				[this](const op_info rm)
				{
					LD_MEM();
				});
			if (m_cpu->size_mode == SIZE16) {
				MOVZX(EAX, AX);
			}
			ST_R32(CPU_CTX_EIP, EAX);
			link_indirect_emit();
			m_cpu->tc->flags |= TC_FLG_INDIRECT;
		}
		else if (instr->raw.modrm.reg == 5) {
			BAD;
		}
		else {
			LIB86CPU_ABORT();
		}
	}
	break;

	default:
		LIB86CPU_ABORT();
	}

	m_cpu->translate_next = 0;
}

void
lc86_jit::loop(ZydisDecodedInstruction *instr)
{
	switch (m_cpu->addr_mode)
	{
	case ADDR16:
		LD_R16(DI, CPU_CTX_ECX);
		SUB(DI, 1);
		ST_R16(CPU_CTX_ECX, DI);
		MOVZX(EDI, DI);
		break;

	case ADDR32:
		LD_R32(EDI, CPU_CTX_ECX);
		SUB(EDI, 1);
		ST_R32(CPU_CTX_ECX, EDI);
		break;

	default:
		LIB86CPU_ABORT();
	}

	Label next_taken = m_a.newLabel();
	Label end = m_a.newLabel();
	TEST(EDI, EDI);
	BR_EQ(next);

	switch (instr->opcode)
	{
	case 0xE0:
		LD_ZF(EDX);
		TEST(EDX, EDX);
		BR_NE(next);
		break;

	case 0xE1:
		LD_ZF(EDX);
		TEST(EDX, EDX);
		BR_EQ(next);
		break;

	case 0xE2:
		break;

	default:
		LIB86CPU_ABORT();
	}

	addr_t next_eip = m_cpu->instr_eip + m_cpu->instr_bytes;
	addr_t loop_eip = next_eip + instr->operands[OPNUM_SINGLE].imm.value.s;
	if (m_cpu->size_mode == SIZE16) {
		loop_eip &= 0x0000FFFF;
	}
	addr_t next_pc = next_eip + m_cpu->cpu_ctx.regs.cs_hidden.base;
	addr_t dst_pc = loop_eip + m_cpu->cpu_ctx.regs.cs_hidden.base;

	ST_R32(CPU_CTX_EIP, loop_eip);
	MOV(R8D, dst_pc);
	BR_UNCOND(end);
	m_a.bind(next_taken);
	ST_R32(CPU_CTX_EIP, next_eip);
	MOV(R8D, next_pc);
	m_a.bind(end);

	link_direct_emit(dst_pc, &next_pc, R8D);
	m_cpu->tc->flags |= TC_FLG_DIRECT;
	m_cpu->translate_next = 0;
}

void
lc86_jit::mov(ZydisDecodedInstruction *instr)
{
	switch (instr->opcode)
	{
	case 0x20: {
		if (m_cpu->cpu_ctx.hflags & HFLG_CPL) {
			RAISEin0_t(EXP_GP);
		}
		else {
			op_info src = GET_REG(OPNUM_SRC);
			auto src_host_reg = SIZED_REG(x64::rax, src.bits);
			LD_REG_val(src_host_reg, src.val, src.bits);
			ST_REG_val(src_host_reg, REG_off(instr->operands[OPNUM_DST].reg.value), src.bits);
		}
	}
	break;

	case 0x21: {
		LD_R32(EAX, CPU_CTX_DR7);
		AND(EAX, DR7_GD_MASK);
		BR_EQl(ok1);
		LD_R32(EDX, CPU_CTX_DR6);
		OR(EDX, DR6_BD_MASK);
		ST_R32(CPU_CTX_DR6, EDX);
		RAISEin0_f(EXP_DB);
		m_a.bind(ok1_taken);
		if (m_cpu->cpu_ctx.hflags & HFLG_CPL) {
			RAISEin0_t(EXP_GP);
		}
		else {
			size_t dr_offset = REG_off(instr->operands[OPNUM_SRC].reg.value);
			if (((instr->operands[OPNUM_SRC].reg.value == ZYDIS_REGISTER_DR4) || (instr->operands[OPNUM_SRC].reg.value == ZYDIS_REGISTER_DR5))) {
				LD_R32(EDX, CPU_CTX_CR4);
				AND(EDX, CR4_DE_MASK);
				BR_EQl(ok2);
				RAISEin0_f(EXP_UD);
				m_a.bind(ok2_taken);
				// turns dr4/5 to dr6/7
				dr_offset = REG_off((instr->operands[OPNUM_SRC].reg.value == ZYDIS_REGISTER_DR4 ? ZYDIS_REGISTER_DR6 : ZYDIS_REGISTER_DR7));
			}
			LD_R32(EAX, dr_offset);
			ST_R32(REG_off(instr->operands[OPNUM_DST].reg.value), EAX);
		}
	}
	break;

	case 0x22: {
		if (m_cpu->cpu_ctx.hflags & HFLG_CPL) {
			RAISEin0_t(EXP_GP);
		}
		else {
			LD_R32(EDX, REG_off(instr->operands[OPNUM_SRC].reg.value));
			int cr_idx = REG_idx(instr->operands[OPNUM_DST].reg.value);
			switch (cr_idx)
			{
			case ZYDIS_REGISTER_CR0:
				m_cpu->translate_next = 0;
				[[fallthrough]];

			case ZYDIS_REGISTER_CR3:
			case ZYDIS_REGISTER_CR4: {
				MOV(RDI, RCX);
				MOV(MEMD32(RSP, STACK_ARGS_off), m_cpu->instr_bytes);
				MOV(R9D, m_cpu->instr_eip);
				MOV(R8D, cr_idx - CR_offset);
				MOV(RAX, &update_crN_helper);
				CALL(RAX);
				MOV(RCX, RDI);
				CMP(AL, 0);
				BR_EQl(ok);
				RAISEin0_f(EXP_GP);
				m_a.bind(ok_taken);
			}
			break;

			case ZYDIS_REGISTER_CR2:
				ST_R32(CPU_CTX_CR2, EDX);
				break;

			default:
				LIB86CPU_ABORT();
			}
		}
	}
	break;

	case 0x23: {
		LD_R32(EAX, CPU_CTX_DR7);
		AND(EAX, DR7_GD_MASK);
		BR_EQl(ok1);
		LD_R32(EDX, CPU_CTX_DR6);
		OR(EDX, DR6_BD_MASK);
		ST_R32(CPU_CTX_DR6, EDX);
		RAISEin0_f(EXP_DB);
		m_a.bind(ok1_taken);
		if (m_cpu->cpu_ctx.hflags & HFLG_CPL) {
			RAISEin0_t(EXP_GP);
		}
		else {
			auto dr_pair = REG_pair(instr->operands[OPNUM_DST].reg.value);
			int dr_idx = dr_pair.first;
			size_t dr_offset = dr_pair.second;
			LD_R32(R8D, REG_off(instr->operands[OPNUM_SRC].reg.value));
			switch (dr_idx)
			{
			case DR0_idx:
			case DR1_idx:
			case DR2_idx:
			case DR3_idx: {
				// flush the old tlb entry, so mem accesses there will call the mem helpers and check for possible watchpoints on the same page
				// as the old one from the other dr regs, then set the new watchpoint if enabled
				LD_R32(EAX, CPU_CTX_DR7);
				LD_R32(EDX, CPU_CTX_CR4);
				MOV(R9D, EAX);
				SHR(EAX, DR7_TYPE_SHIFT + (dr_idx - DR_offset) * 4);
				AND(EAX, 3);
				AND(EDX, CR4_DE_MASK);
				OR(EAX, EDX);
				CMP(EAX, DR7_TYPE_IO_RW | CR4_DE_MASK); // check if it is a mem or io watchpoint
				BR_EQl(io);
				LEA(RDI, MEMD64(RCX, CPU_CTX_TLB));
				LD_R32(EAX, dr_offset);
				SHR(EAX, PAGE_SHIFT);
				MOV(EDX, MEMS32(RDI, RAX, 2));
				AND(EDX, (TLB_CODE | TLB_GLOBAL | TLB_DIRTY | TLB_WATCH));
				MOV(MEMS32(RDI, RAX, 2), EDX); // flush old tlb entry
				SHR(R9D, (dr_idx - DR_offset) * 2);
				AND(R9D, 3);
				BR_EQl(disabled); // check if new watchpoint is enabled
				MOV(EAX, R8D);
				SHR(EAX, PAGE_SHIFT);
				MOV(EDX, MEMS32(RDI, RAX, 2));
				OR(EDX, TLB_WATCH);
				MOV(MEMS32(RDI, RAX, 2), EDX); // set new enabled watchpoint
				BR_UNCOND(disabled_taken);
				m_a.bind(io_taken);
				LEA(RDI, MEMD64(RCX, CPU_CTX_IOTLB));
				LD_R32(EAX, dr_offset);
				SHR(EAX, IO_SHIFT);
				MOV(DX, MEMS16(RDI, RAX, 1));
				AND(DX, IOTLB_WATCH);
				MOV(MEMS16(RDI, RAX, 1), DX); // flush old iotlb entry
				SHR(R9D, (dr_idx - DR_offset) * 2);
				AND(R9D, 3);
				m_a.je(disabled_taken); // check if new io watchpoint is enabled
				MOV(EAX, R8D);
				SHR(EAX, IO_SHIFT);
				MOV(DX, MEMS16(RDI, RAX, 1));
				OR(DX, IOTLB_WATCH);
				MOV(MEMS16(RDI, RAX, 1), DX); // set new enabled io watchpoint
				m_a.bind(disabled_taken);
			}
			break;

			case DR4_idx: {
				LD_R32(EDX, CPU_CTX_CR4);
				AND(EDX, CR4_DE_MASK);
				BR_EQl(ok);
				RAISEin0_f(EXP_UD);
				m_a.bind(ok_taken);
				dr_offset = REG_off(ZYDIS_REGISTER_DR6); // turns dr4 to dr6
			}
			[[fallthrough]];

			case DR6_idx:
				OR(R8D, DR6_RES_MASK);
				break;

			case DR5_idx: {
				LD_R32(EDX, CPU_CTX_CR4);
				AND(EDX, CR4_DE_MASK);
				BR_EQl(ok);
				RAISEin0_f(EXP_UD);
				m_a.bind(ok_taken);
				dr_offset = REG_off(ZYDIS_REGISTER_DR7); // turns dr5 to dr7
			}
			[[fallthrough]];

			case DR7_idx: {
				static const char *abort_msg = "Io watchpoints are not supported";
				OR(R8D, DR7_RES_MASK);
				LD_R32(R9D, CPU_CTX_CR4);
				AND(R9D, CR4_DE_MASK);
				for (int idx = 0; idx < 4; ++idx) {
					MOV(EAX, R8D);
					SHR(EAX, DR7_TYPE_SHIFT + idx * 4);
					AND(EAX, 3);
					OR(EAX, R9D);
					CMP(EAX, DR7_TYPE_IO_RW | CR4_DE_MASK); // check if it is a mem or io watchpoint
					BR_EQl(io);
					LEA(RDI, MEMD64(RCX, CPU_CTX_TLB));
					MOV(EDX, R8D);
					SHR(EDX, idx * 2);
					AND(EDX, 3);
					BR_EQl(disabled); // check if watchpoint is enabled
					LD_R32(EAX, REG_off(static_cast<ZydisRegister>(ZYDIS_REGISTER_DR0 + idx)));
					SHR(EAX, PAGE_SHIFT);
					MOV(EDX, MEMS32(RDI, RAX, 2));
					OR(EDX, TLB_WATCH);
					MOV(MEMS32(RDI, RAX, 2), EDX); // set enabled watchpoint
					Label exit = m_a.newLabel();
					BR_UNCOND(exit);
					m_a.bind(disabled_taken);
					LD_R32(EAX, REG_off(static_cast<ZydisRegister>(ZYDIS_REGISTER_DR0 + idx)));
					SHR(EAX, PAGE_SHIFT);
					MOV(EDX, MEMS32(RDI, RAX, 2));
					AND(EDX, ~TLB_WATCH);
					MOV(MEMS32(RDI, RAX, 2), EDX); // remove disabled watchpoint
					BR_UNCOND(exit);
					m_a.bind(io_taken);
					// we don't support io watchpoints yet so for now we just abort
					MOV(RCX, abort_msg);
					MOV(RAX, &cpu_runtime_abort); // won't return
					CALL(RAX);
					m_a.bind(exit);
				}
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			ST_R32(dr_offset, R8D);
			ST_R32(CPU_CTX_EIP, m_cpu->instr_eip + m_cpu->instr_bytes);
			// instr breakpoint are checked at compile time, so we cannot jump to the next tc if we are writing to anything but dr6
			if ((((m_cpu->virt_pc + m_cpu->instr_bytes) & ~PAGE_MASK) == (m_cpu->virt_pc & ~PAGE_MASK)) && (dr_idx == DR6_idx)) {
				link_dst_only_emit();
				m_cpu->tc->flags |= TC_FLG_DST_ONLY;
			}
			m_cpu->translate_next = 0;
		}
	}
	break;

	case 0x88:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0x89: {
		auto src = GET_REG(OPNUM_SRC);
		auto src_host_reg = SIZED_REG(x64::rax, src.bits);
		LD_REG_val(src_host_reg, src.val, src.bits);
		get_rm<OPNUM_DST>(instr,
			[this, src, src_host_reg](const op_info rm)
			{
				ST_REG_val(src_host_reg, rm.val, src.bits);
			},
			[this, src_host_reg](const op_info rm)
			{
				ST_MEM(src_host_reg);
			});
	}
	break;

	case 0x8A:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0x8B: {
		auto dst = GET_REG(OPNUM_DST);
		get_rm<OPNUM_SRC>(instr,
			[this, dst](const op_info rm)
			{
				auto src_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_REG_val(src_host_reg, rm.val, rm.bits);
				ST_REG_val(src_host_reg, dst.val, rm.bits);
			},
			[this, dst](const op_info rm)
			{
				LD_MEM();
				ST_REG_val(SIZED_REG(x64::rax, rm.bits), dst.val, dst.bits);
			});
	}
	break;

	case 0x8C: {
		LD_SEG(AX, REG_off(instr->operands[OPNUM_SRC].reg.value));
		get_rm<OPNUM_DST>(instr,
			[this](const op_info rm)
			{
				MOVZX(EAX, AX);
				ST_REG_val(EAX, rm.val, SIZE32);
			},
			[this](const op_info rm)
			{
				ST_MEMs(AX, SIZE16);
			});
	}
	break;

	case 0x8E: {
		get_rm<OPNUM_SRC>(instr,
			[this](const op_info rm)
			{
				LD_REG_val(SIZED_REG(x64::rax, rm.bits), rm.val, rm.bits);
			},
			[this](const op_info rm)
			{
				LD_MEM();
			});
		if (m_cpu->cpu_ctx.hflags & HFLG_PE_MODE) {
			if (instr->operands[OPNUM_DST].reg.value == ZYDIS_REGISTER_SS) {
				MOV(RDI, RCX);
				MOV(R8D, m_cpu->instr_eip);
				MOV(DX, AX);
				MOV(RAX, &mov_sel_pe_helper<SS_idx>);
				CALL(RAX);
				MOV(RCX, RDI);
				CMP(AL, 0);
				BR_EQl(ok);
				RAISEin_no_param_f();
				m_a.bind(ok_taken);
				ST_R32(CPU_CTX_EIP, m_cpu->instr_eip + m_cpu->instr_bytes);
#if 0
				if (((m_cpu->virt_pc + m_cpu->instr_bytes) & ~PAGE_MASK) == (m_cpu->virt_pc & ~PAGE_MASK)) {

					BasicBlock *bb2 = getBB();
					BasicBlock *bb3 = getBB();
					BR_COND(bb2, bb3, ICMP_EQ(CONST32(cpu->cpu_ctx.hflags & HFLG_SS32), AND(LD(cpu->ptr_hflags, getIntegerType(32)), CONST32(HFLG_SS32))));
					cpu->bb = bb2;
					link_dst_only_emit();
					cpu->bb = bb3;
					m_cpu->tc->flags |= TC_FLG_COND_DST_ONLY;
				}
#endif
				m_cpu->translate_next = 0;
			}
			else {
				MOV(RDI, RCX);
				MOV(R8D, m_cpu->instr_eip);
				MOV(DX, AX);

				switch (REG_idx(instr->operands[OPNUM_DST].reg.value))
				{
				case DS_idx:
					MOV(RAX, &mov_sel_pe_helper<DS_idx>);
					break;

				case ES_idx:
					MOV(RAX, &mov_sel_pe_helper<ES_idx>);
					break;

				case FS_idx:
					MOV(RAX, &mov_sel_pe_helper<FS_idx>);
					break;

				case GS_idx:
					MOV(RAX, &mov_sel_pe_helper<GS_idx>);
					break;

				default:
					LIB86CPU_ABORT();
				}

				CALL(RAX);
				MOV(RCX, RDI);
				CMP(AL, 0);
				BR_EQl(ok);
				RAISEin_no_param_f();
				m_a.bind(ok_taken);
			}
		}
		else {
			const size_t seg_offset = REG_off(instr->operands[OPNUM_DST].reg.value);
			ST_SEG(seg_offset, AX);
			MOVZX(EAX, AX);
			SHL(EAX, 4);
			ST_SEG_BASE(seg_offset, EAX);
		}
	}
	break;

	case 0xA0:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0xA1: {
		GET_OP(OPNUM_SRC);
		LD_MEM();
		ST_REG_val(SIZED_REG(x64::rax, m_cpu->size_mode), GET_REG(OPNUM_DST).val, m_cpu->size_mode);
	}
	break;

	case 0xA2:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0xA3: {
		GET_OP(OPNUM_DST);
		op_info src = GET_OP(OPNUM_SRC);
		auto src_host_reg = SIZED_REG(x64::rax, src.bits);
		LD_REG_val(src_host_reg, src.val, src.bits);
		ST_MEM(src_host_reg);
	}
	break;

	case 0xB0:
	case 0xB1:
	case 0xB2:
	case 0xB3:
	case 0xB4:
	case 0xB5:
	case 0xB6:
	case 0xB7: {
		auto dst = GET_OP(OPNUM_DST);
		ST_REG_val(GET_IMM(), dst.val, dst.bits);
	}
	break;

	case 0xB8:
	case 0xB9:
	case 0xBA:
	case 0xBB:
	case 0xBC:
	case 0xBD:
	case 0xBE:
	case 0xBF: {
		auto dst = GET_OP(OPNUM_DST);
		ST_REG_val(GET_IMM(), dst.val, dst.bits);
	}
	break;

	case 0xC6:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0xC7: {
		get_rm<OPNUM_DST>(instr,
			[this, instr](const op_info rm)
			{
				ST_REG_val(GET_IMM(), rm.val, rm.bits);
			},
			[this, instr](const op_info rm)
			{
				ST_MEM(GET_IMM());
			});
	}
	break;

	default:
		LIB86CPU_ABORT();
	}
}

void
lc86_jit::out(ZydisDecodedInstruction *instr)
{
	switch (instr->opcode)
	{
	case 0xE6:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0xE7: {
		uint8_t port = instr->operands[OPNUM_DST].imm.value.u;
		check_io_priv_emit(port);
		MOV(EDX, port);
		XOR(EAX, EAX);
		LD_REG_val(SIZED_REG(x64::rax, m_cpu->size_mode), CPU_CTX_EAX, m_cpu->size_mode);
		ST_IO();
	}
	break;

	case 0xEE:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0xEF: {
		MOVZX(EDX, MEMD16(RCX, CPU_CTX_EDX));
		if (check_io_priv_emit(EDX)) {
			MOV(EDX, MEMD32(RSP, LOCAL_VARS_off(0)));
		}
		XOR(EAX, EAX);
		LD_REG_val(SIZED_REG(x64::rax, m_cpu->size_mode), CPU_CTX_EAX, m_cpu->size_mode);
		ST_IO();
	}
	break;

	default:
		LIB86CPU_ABORT();
	}
}

void
lc86_jit::sahf(ZydisDecodedInstruction *instr)
{
	MOVZX(R8D, MEMD8(RCX, CPU_CTX_EAX + 1));
	MOV(EDX, R8D);
	MOV(EDI, R8D);
	AND(EDX, 1);
	AND(EDI, 0x10);
	SHL(EDX, 0x1E);
	LEA(EAX, MEMSb32(R8, 2, 0));
	NOT(EAX);
	AND(EAX, 0x100);
	MOV(MEMD32(RCX, CPU_CTX_EFLAGS_RES), EAX);
	MOV(R9D, MEMD32(RCX, CPU_CTX_EFLAGS_AUX));
	LD_OF(EAX, R9D);
	SHR(EAX, 1);
	XOR(EDX, EAX);
	MOV(EAX, R8D);
	SHR(EAX, 6);
	OR(EDI, EAX);
	MOV(EAX, R8D);
	SHL(EAX, 6);
	NOT(EAX);
	SHR(EDI, 1);
	OR(EDX, EDI);
	SHL(R8D, 0x1F);
	AND(EAX, 0x100);
	OR(EDX, EAX);
	OR(EDX, R8D);
	MOV(MEMD32(RCX, CPU_CTX_EFLAGS_AUX), EDX);
}

void
lc86_jit::xor_(ZydisDecodedInstruction *instr)
{
	switch (instr->opcode)
	{
	case 0x30:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0x31: {
		op_info src = GET_REG(OPNUM_SRC);
		auto src_host_reg = SIZED_REG(x64::rdx, src.bits);
		LD_REG_val(src_host_reg, src.val, src.bits);
		auto dst_host_reg = get_rm<OPNUM_DST>(instr,
			[this, src_host_reg, instr](const op_info rm)
			{
				auto dst_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_REG_val(dst_host_reg, rm.val, rm.bits);
				XOR(dst_host_reg, src_host_reg);
				ST_REG_val(dst_host_reg, rm.val, rm.bits);
				return dst_host_reg;
			},
			[this, src_host_reg, instr](const op_info rm)
			{
				auto dst_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_MEMs(rm.bits);
				XOR(dst_host_reg, src_host_reg);
				ST_REG_val(dst_host_reg, rm.val, rm.bits);
				return dst_host_reg;
			});
		set_flags<EAX>(dst_host_reg, 0, m_cpu->size_mode);
	}
	break;

	case 0x32:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0x33: {
		op_info dst = GET_REG(OPNUM_DST);
		auto dst_host_reg = SIZED_REG(x64::rdx, dst.bits);
		LD_REG_val(dst_host_reg, dst.val, dst.bits);
		get_rm<OPNUM_SRC>(instr,
			[this, dst_host_reg, dst, instr](const op_info rm)
			{
				auto src_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_REG_val(src_host_reg, rm.val, rm.bits);
				XOR(dst_host_reg, src_host_reg);
				ST_REG_val(dst_host_reg, dst.val, dst.bits);
			},
			[this, dst_host_reg, dst, instr](const op_info rm)
			{
				auto src_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_MEMs(rm.bits);
				XOR(dst_host_reg, src_host_reg);
				ST_REG_val(dst_host_reg, dst.val, dst.bits);
			});
		set_flags<EDX>(dst_host_reg, 0, m_cpu->size_mode);
	}
	break;

	case 0x34:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0x35: {
		op_info dst = GET_REG(OPNUM_DST);
		auto dst_host_reg = SIZED_REG(x64::rax, dst.bits);
		LD_REG_val(dst_host_reg, dst.val, dst.bits);
		XOR(dst_host_reg, GET_IMM());
		ST_REG_val(dst_host_reg, dst.val, dst.bits);
		set_flags<EAX>(dst_host_reg, 0, m_cpu->size_mode);
	}
	break;

	case 0x80:
		m_cpu->size_mode = SIZE8;
		[[fallthrough]];

	case 0x81: {
		uint32_t src = GET_IMM();
		auto dst_host_reg = get_rm<OPNUM_DST>(instr,
			[this, src, instr](const op_info rm)
			{
				auto dst_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_REG_val(dst_host_reg, rm.val, rm.bits);
				XOR(dst_host_reg, src);
				ST_REG_val(dst_host_reg, rm.val, rm.bits);
				return dst_host_reg;
			},
			[this, src, instr](const op_info rm)
			{
				auto dst_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_MEMs(rm.bits);
				XOR(dst_host_reg, src);
				ST_REG_val(dst_host_reg, rm.val, rm.bits);
				return dst_host_reg;
			});
		set_flags<EAX>(dst_host_reg, 0, m_cpu->size_mode);
	}
	break;

	case 0x83: {
		int32_t src = static_cast<int32_t>(static_cast<int8_t>(GET_IMM()));
		auto dst_host_reg = get_rm<OPNUM_DST>(instr,
			[this, src, instr](const op_info rm)
			{
				auto dst_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_REG_val(dst_host_reg, rm.val, rm.bits);
				XOR(dst_host_reg, src);
				ST_REG_val(dst_host_reg, rm.val, rm.bits);
				return dst_host_reg;
			},
			[this, src, instr](const op_info rm)
			{
				auto dst_host_reg = SIZED_REG(x64::rax, rm.bits);
				LD_MEMs(rm.bits);
				XOR(dst_host_reg, src);
				ST_REG_val(dst_host_reg, rm.val, rm.bits);
				return dst_host_reg;
			});
		set_flags<EAX>(dst_host_reg, 0, m_cpu->size_mode);
	}
	break;

	default:
		LIB86CPU_ABORT();
	}
}

#endif
