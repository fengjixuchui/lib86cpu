/*
 * x86 translation code
 *
 * ergo720                Copyright (c) 2019
 */

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Instructions.h"
#include "x86_internal.h"
#include "x86_isa.h"
#include "x86_frontend.h"
#include "x86_memory.h"

#define BAD       printf("%s: encountered unimplemented instruction %s\n", __func__, get_instr_name(instr.opcode)); return LIB86CPU_OP_NOT_IMPLEMENTED
#define BAD_MODE  printf("%s: instruction %s not implemented in %s mode\n", __func__, get_instr_name(instr.opcode), cpu_ctx->hflags & HFLG_PE_MODE ? "protected" : "real"); return LIB86CPU_OP_NOT_IMPLEMENTED


const char *
get_instr_name(unsigned num)
{
	return mnemo[num];
}

[[noreturn]] void
cpu_throw_exception(cpu_ctx_t *cpu_ctx, uint8_t expno, uint32_t eip)
{
	cpu_ctx->hflags |= HFLG_CPL_PRIV;
	throw expno;
}

[[noreturn]] void
cpu_raise_exception(cpu_ctx_t *cpu_ctx, uint8_t expno, uint32_t eip)
{
	cpu_t *cpu = cpu_ctx->cpu;

	if (cpu_ctx->hflags & HFLG_PE_MODE) {
		cpu_ctx->hflags |= HFLG_CPL_PRIV;
		LIB86CPU_ABORT_msg("Exceptions are unsupported in protected mode (for now)\n");
	}

	// push to the stack eflags, cs and eip
	addr_t stack_base = cpu_ctx->regs.ss_hidden.base + (cpu_ctx->regs.esp & 0x0000FFFF) - 2;
	mem_write<uint16_t>(cpu, stack_base, cpu_ctx->regs.eflags |
		((cpu_ctx->lazy_eflags.auxbits & 0x80000000) >> 31) |
		((cpu_ctx->lazy_eflags.parity[(cpu_ctx->lazy_eflags.result & 0xFF) ^ ((cpu_ctx->lazy_eflags.auxbits & 0xFF00) >> 8)] ^ 1) << 2) |
		((cpu_ctx->lazy_eflags.auxbits & 8) << 1) |
		((cpu_ctx->lazy_eflags.result == 0) << 6) |
		(((cpu_ctx->lazy_eflags.result & 0x80000000) >> 31) ^ (cpu_ctx->lazy_eflags.auxbits & 1) << 7) |
		(((cpu_ctx->lazy_eflags.auxbits & 0x80000000) ^ ((cpu_ctx->lazy_eflags.auxbits & 0x40000000) << 1)) >> 20),
		eip);
	stack_base -= 2;
	mem_write<uint16_t>(cpu, stack_base, cpu_ctx->regs.cs, eip);
	stack_base -= 2;
	mem_write<uint16_t>(cpu, stack_base, eip, eip);
	cpu_ctx->regs.esp = stack_base - cpu_ctx->regs.ss_hidden.base;

	// clear IF, TF, RF and AC flags
	cpu_ctx->regs.eflags &= ~(TF_MASK | IF_MASK | RF_MASK | AC_MASK);

	// transfer program control to the exception handler specified in the idt
	addr_t vec_addr = cpu_ctx->regs.idtr_base + expno * 4;
	uint32_t vec_entry = mem_read<uint32_t>(cpu, vec_addr, eip);
	cpu_ctx->regs.cs = (vec_entry & 0xFFFF0000) >> 16;
	cpu_ctx->regs.cs_hidden.base = cpu_ctx->regs.cs << 4;
	cpu_ctx->regs.eip = vec_entry & 0x0000FFFF;

	// throw an exception to forcefully transfer control to the exception handler
	throw expno;
}

JIT_EXTERNAL_CALL_C void
cpu_update_crN(cpu_ctx_t *cpu_ctx, uint32_t new_cr, uint8_t idx, uint32_t eip, uint32_t bytes)
{
	switch (idx)
	{
	case 0:
		if (((new_cr & CR0_PE_MASK) == 0 && (new_cr & CR0_PG_MASK) >> 31 == 1) ||
			((new_cr & CR0_CD_MASK) == 0 && (new_cr & CR0_NW_MASK) >> 29 == 1)) {
			cpu_raise_exception(cpu_ctx, EXP_GP, eip);
		}
		if ((cpu_ctx->regs.cr0 & CR0_PE_MASK) != (new_cr & CR0_PE_MASK)) {
			tc_cache_clear(cpu_ctx->cpu);
			if (new_cr & CR0_PE_MASK) {
				if (cpu_ctx->regs.cs_hidden.flags & SEG_HIDDEN_DB) {
					cpu_ctx->hflags |= HFLG_CS32;
				}
				cpu_ctx->hflags |= (HFLG_PE_MODE | (cpu_ctx->regs.cs & HFLG_CPL));
			}
			else {
				cpu_ctx->hflags &= ~(HFLG_CS32 | HFLG_PE_MODE);
			}

			// since tc_cache_clear has deleted the calling code block, we must return to the translator with an exception. We also have to setup the eip
			// to point to the next instruction
			cpu_ctx->regs.eip = (eip + bytes);
			cpu_ctx->regs.cr0 = ((new_cr & CR0_FLG_MASK) | CR0_ET_MASK);
			throw static_cast<uint8_t>(0xFF);
		}
		cpu_ctx->regs.cr0 = ((new_cr & CR0_FLG_MASK) | CR0_ET_MASK);
		break;

	case 3:
		cpu_ctx->regs.cr3 = (new_cr & CR3_FLG_MASK);
		cpu_ctx->cpu->pt_mr = as_memory_search_addr<uint8_t>(cpu_ctx->cpu, cpu_ctx->regs.cr3 & CR3_PD_MASK);
		assert(cpu_ctx->cpu->pt_mr->type == MEM_RAM);
		break;

	case 2:
	case 4:
		break;
	}
}

static lib86cpu_status
cpu_translate(cpu_t *cpu, disas_ctx_t *disas_ctx, translated_code_t *tc)
{
	uint8_t translate_next = 1;
	uint8_t size_mode;
	uint8_t addr_mode;
	BasicBlock *bb = disas_ctx->bb;
	Function *func = bb->getParent();
	cpu_ctx_t *cpu_ctx = &cpu->cpu_ctx;
	addr_t pc = disas_ctx->virt_pc;
	size_t bytes = 0;
	// we can use the same indexes for both loads and stores because they have the same order in cpu->ptr_mem_xxfn
	static const uint8_t fn_idx[3] = { MEM_LD32_idx, MEM_LD16_idx, MEM_LD8_idx };

	Function::arg_iterator args_func = func->arg_begin();
	args_func++;
	Value *ptr_eip = CONST32(pc - cpu_ctx->regs.cs_hidden.base);
	cpu->ptr_cpu_ctx = args_func++;
	cpu->ptr_cpu_ctx->setName("cpu_ctx");
	cpu->ptr_regs = GEP(cpu->ptr_cpu_ctx, 1);
	cpu->ptr_regs->setName("regs");
	cpu->ptr_eflags = GEP(cpu->ptr_cpu_ctx, 2);
	cpu->ptr_eflags->setName("eflags");
	cpu->ptr_hflags = GEP(cpu->ptr_cpu_ctx, 3);
	cpu->ptr_hflags->setName("hflags");

	do {

		x86_instr instr = { 0 };
		ptr_eip = ADD(ptr_eip, CONST32(bytes));

		try {

#ifdef DEBUG_LOG

			// print the disassembled instructions only in debug builds
			char disassembly_line[80];
			bytes = disasm_instr(cpu, &instr, disassembly_line, sizeof(disassembly_line), disas_ctx);

			printf(".,%08lx ", static_cast<unsigned long>(pc));
			for (uint8_t i = 0; i < bytes; i++) {
				printf("%02X ", disas_ctx->instr_bytes[i]);
			}
			printf("%*s", (24 - 3 * bytes) + 1, "");
			printf("%-23s\n", disassembly_line);

#else

			decode_instr(cpu, &instr, disas_ctx);
			bytes = get_instr_length(&instr);

#endif

		}
		catch (uint8_t expno) {
			switch (expno)
			{
			case EXP_PF:
				// page fault during instruction fetch
				disas_ctx->flags |= DISAS_FLG_FETCH_FAULT;
				[[fallthrough]];

			case EXP_GP:
				// the instruction exceeded the maximum allowed length
				RAISE(expno);
				disas_ctx->next_pc = CONST32(0); // unreachable
				return LIB86CPU_SUCCESS;

			case 0xFF:
				// TODO: actually this should raise an UD exception for illegal opcodes
				printf("error: unable to decode opcode %x\n", instr.opcode_byte);
				return LIB86CPU_UNKNOWN_INSTR;

			default:
				LIB86CPU_ABORT();
			}
		}

		if ((disas_ctx->flags & DISAS_FLG_CS32) ^ instr.op_size_override) {
			size_mode = SIZE32;
		}
		else {
			size_mode = SIZE16;
		}

		if ((disas_ctx->flags & DISAS_FLG_CS32) ^ instr.addr_size_override) {
			addr_mode = ADDR32;
		}
		else {
			addr_mode = ADDR16;
		}

		switch (instr.opcode) {
		case X86_OPC_AAA:         BAD;
		case X86_OPC_AAD:         BAD;
		case X86_OPC_AAM:         BAD;
		case X86_OPC_AAS:         BAD;
		case X86_OPC_ADC:         BAD;
		case X86_OPC_ADD: {
			switch (instr.opcode_byte)
			{
			case 0x00:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x01: {
				Value *rm, *dst, *sum, *val;
				val = LD_REG_val(GET_REG(OPNUM_SRC));
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm); sum = ADD(dst, val); ST_REG_val(sum, rm);,
					dst = LD_MEM(fn_idx[size_mode], rm); sum = ADD(dst, val); ST_MEM(fn_idx[size_mode], rm, sum););
				SET_FLG_SUM(sum, dst, val);
			}
			break;

			case 0x04:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x05: {
				Value *val, *sum, *eax, *dst;
				val = GET_IMM();
				dst = GET_REG(OPNUM_DST);
				eax = LD_REG_val(dst);
				sum = ADD(eax, val);
				ST_REG_val(sum, dst);
				SET_FLG_SUM(sum, eax, val);
			}
			break;

			case 0x80:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x83: {
				assert(instr.reg_opc == 0);

				Value *rm, *dst, *sum, *val = GET_IMM8();
				val = size_mode == SIZE16 ? SEXT16(val) : size_mode == SIZE32 ? SEXT32(val) : val;
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm); sum = ADD(dst, val); ST_REG_val(sum, rm);,
					dst = LD_MEM(fn_idx[size_mode], rm); sum = ADD(dst, val); ST_MEM(fn_idx[size_mode], rm, sum););
				SET_FLG_SUM(sum, dst, val);
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_AND: {
			switch (instr.opcode_byte)
			{
			case 0x24:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x25: {
				Value *val, *eax;
				eax = GET_REG(OPNUM_DST);
				val = AND(LD_REG_val(eax), GET_IMM());
				ST_REG_val(val, eax);
				SET_FLG(val, CONST32(0));
			}
			break;

			case 0x80:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x81: {
				assert(instr.reg_opc == 4);

				Value *val, *rm, *src = GET_IMM();
				GET_RM(OPNUM_DST, val = LD_REG_val(rm); val = AND(val, src); ST_REG_val(val, rm);, val = LD_MEM(fn_idx[size_mode], rm);
				val = AND(val, src); ST_MEM(fn_idx[size_mode], rm, val););
				SET_FLG(val, CONST32(0));
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_ARPL:        BAD;
		case X86_OPC_BOUND:       BAD;
		case X86_OPC_BSF:         BAD;
		case X86_OPC_BSR:         BAD;
		case X86_OPC_BSWAP:       BAD;
		case X86_OPC_BT:          BAD;
		case X86_OPC_BTC:         BAD;
		case X86_OPC_BTR:         BAD;
		case X86_OPC_BTS:         BAD;
		case X86_OPC_LCALL: // AT&T
		case X86_OPC_CALL: {
			switch (instr.opcode_byte)
			{
			case 0x9A: {
				if (cpu_ctx->hflags & HFLG_PE_MODE) {
					BAD_MODE;
				}
				addr_t ret_eip = (pc - cpu_ctx->regs.cs_hidden.base) + bytes;
				addr_t call_eip = instr.operand[OPNUM_SRC].imm;
				uint16_t new_sel = instr.operand[OPNUM_SRC].seg_sel;
				if (size_mode == SIZE16) {
					call_eip &= 0x0000FFFF;
				}
				// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
				Value *sp = SUB(LD_R16(ESP_idx), size_mode == SIZE16 ? CONST16(2) : CONST16(4));
				ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), size_mode == SIZE16 ? CONST16(cpu_ctx->regs.cs) : CONST32(cpu_ctx->regs.cs));
				sp = SUB(sp, size_mode == SIZE16 ? CONST16(2) : CONST16(4));
				ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), size_mode == SIZE16 ? CONST16(ret_eip) : CONST32(ret_eip));
				ST_R16(sp, ESP_idx);
				ST_SEG(CONST16(new_sel), CS_idx);
				ST_R32(CONST32(call_eip), EIP_idx);
				ST_SEG_HIDDEN(CONST32(static_cast<uint32_t>(new_sel) << 4), CS_idx, SEG_BASE_idx);
				disas_ctx->next_pc = CONST32((static_cast<uint32_t>(new_sel) << 4) + call_eip);
			}
			break;

			case 0xE8: {
				addr_t ret_eip = (pc - cpu_ctx->regs.cs_hidden.base) + bytes;
				addr_t call_eip = ret_eip + instr.operand[OPNUM_SRC].rel;
				if (size_mode == SIZE16) {
					call_eip &= 0x0000FFFF;
				}
				// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
				Value *sp = SUB(LD_R16(ESP_idx), size_mode == SIZE16 ? CONST16(2) : CONST16(4));
				ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), size_mode == SIZE16 ? CONST16(ret_eip) : CONST32(ret_eip));
				ST_R16(sp, ESP_idx);
				ST_R32(CONST32(call_eip), EIP_idx);
				disas_ctx->next_pc = CONST32(cpu_ctx->regs.cs_hidden.base + call_eip);
			}
			break;

			case 0xFF: {
				if (instr.reg_opc == 2) {
					Value *call_eip, *rm, *sp;
					addr_t ret_eip = (pc - cpu_ctx->regs.cs_hidden.base) + bytes;
					GET_RM(OPNUM_SRC, call_eip = LD_REG_val(rm);, call_eip = LD_MEM(fn_idx[size_mode], rm););
					// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
					sp = SUB(LD_R16(ESP_idx), size_mode == SIZE16 ? CONST16(2) : CONST16(4));
					ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), size_mode == SIZE16 ? CONST16(ret_eip) : CONST32(ret_eip));
					if (size_mode == SIZE16) {
						call_eip = ZEXT32(call_eip);
					}
					ST_R16(sp, ESP_idx);
					ST_R32(call_eip, EIP_idx);
					disas_ctx->next_pc = ADD(CONST32(cpu_ctx->regs.cs_hidden.base), call_eip);
					disas_ctx->flags |= DISAS_FLG_TC_INDIRECT;
				}
				else if (instr.reg_opc == 3) {
					if (cpu_ctx->hflags & HFLG_PE_MODE) {
						BAD_MODE;
					}
					assert(instr.operand[OPNUM_SRC].type == OPTYPE_MEM ||
						instr.operand[OPNUM_SRC].type == OPTYPE_MEM_DISP ||
						instr.operand[OPNUM_SRC].type == OPTYPE_SIB_MEM ||
						instr.operand[OPNUM_SRC].type == OPTYPE_SIB_DISP);

					addr_t ret_eip = (pc - cpu_ctx->regs.cs_hidden.base) + bytes;
					// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
					Value *sp = SUB(LD_R16(ESP_idx), size_mode == SIZE16 ? CONST16(2) : CONST16(4));
					ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), size_mode == SIZE16 ? CONST16(cpu_ctx->regs.cs) : CONST32(cpu_ctx->regs.cs));
					sp = SUB(sp, size_mode == SIZE16 ? CONST16(2) : CONST16(4));
					ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), size_mode == SIZE16 ? CONST16(ret_eip) : CONST32(ret_eip));
					Value *call_eip, *call_cs, *cs_addr, *offset_addr = GET_OP(OPNUM_SRC);
					if (size_mode == SIZE16) {
						call_eip = ZEXT32(LD_MEM(MEM_LD16_idx, offset_addr));
						cs_addr = ADD(offset_addr, CONST32(2));
					}
					else {
						call_eip = LD_MEM(MEM_LD32_idx, offset_addr);
						cs_addr = ADD(offset_addr, CONST32(4));
					}
					call_cs = LD_MEM(MEM_LD16_idx, cs_addr);

					ST_R16(sp, ESP_idx);
					ST_SEG(call_cs, CS_idx);
					ST_R32(call_eip, EIP_idx);
					Value *call_cs_base = SHL(ZEXT32(call_cs), CONST32(4));
					ST_SEG_HIDDEN(call_cs_base, CS_idx, SEG_BASE_idx);
					disas_ctx->next_pc = ADD(call_cs_base, call_eip);
					disas_ctx->flags |= DISAS_FLG_TC_INDIRECT;
				}
				else {
					LIB86CPU_ABORT();
				}
			}
			break;

			default:
				LIB86CPU_ABORT();
			}

			translate_next = 0;
		}
		break;

		case X86_OPC_CBW:         BAD;
		case X86_OPC_CBTV:        BAD;
		case X86_OPC_CDQ:         BAD;
		case X86_OPC_CLC: {
			assert(instr.opcode_byte == 0xF8);

			Value *of_new = SHR(XOR(CONST32(0), LD_OF()), CONST32(1));
			ST_FLG_AUX(OR(AND(LD_FLG_AUX(), CONST32(0x3FFFFFFF)), of_new));
		}
		break;

		case X86_OPC_CLD: {
			assert(instr.opcode_byte == 0xFC);

			Value *eflags = LD_R32(EFLAGS_idx);
			eflags = AND(eflags, CONST32(~DF_MASK));
			ST_R32(eflags, EFLAGS_idx);
		}
		break;

		case X86_OPC_CLI: {
			assert(instr.opcode_byte == 0xFA);

			if (cpu_ctx->hflags & HFLG_PE_MODE) {
				BAD_MODE;
			}
			else {
				Value *eflags = LD_R32(EFLAGS_idx);
				eflags = AND(eflags, CONST32(~IF_MASK));
				ST_R32(eflags, EFLAGS_idx);
			}
		}
		break;

		case X86_OPC_CLTD:        BAD;
		case X86_OPC_CLTS:        BAD;
		case X86_OPC_CMC:         BAD;
		case X86_OPC_CMOVA:       BAD;
		case X86_OPC_CMOVB:       BAD;
		case X86_OPC_CMOVBE:      BAD;
		case X86_OPC_CMOVG:       BAD;
		case X86_OPC_CMOVGE:      BAD;
		case X86_OPC_CMOVL:       BAD;
		case X86_OPC_CMOVLE:      BAD;
		case X86_OPC_CMOVNB:      BAD;
		case X86_OPC_CMOVNE:      BAD;
		case X86_OPC_CMOVNO:      BAD;
		case X86_OPC_CMOVNS:      BAD;
		case X86_OPC_CMOVO:       BAD;
		case X86_OPC_CMOVPE:      BAD;
		case X86_OPC_CMOVPO:      BAD;
		case X86_OPC_CMOVS:       BAD;
		case X86_OPC_CMOVZ:       BAD;
		case X86_OPC_CMP: {
			Value *val, *cmp, *sub, *rm;
			switch (instr.opcode_byte)
			{
			case 0x38:
				size_mode = SIZE8;
				val = LD_REG_val(GET_REG(OPNUM_SRC));
				GET_RM(OPNUM_DST, cmp = LD_REG_val(rm);, cmp = LD_MEM(fn_idx[size_mode], rm););
				break;

			case 0x39:
				val = LD_REG_val(GET_REG(OPNUM_SRC));
				GET_RM(OPNUM_DST, cmp = LD_REG_val(rm);, cmp = LD_MEM(fn_idx[size_mode], rm););
				break;

			case 0x3C:
				size_mode = SIZE8;
				val = LD_REG_val(GET_REG(OPNUM_DST));
				cmp = GET_IMM8();
				break;

			case 0x3D:
				val = LD_REG_val(GET_REG(OPNUM_DST));
				cmp = GET_IMM();
				break;

			case 0x80:
			case 0x82:
				assert(instr.reg_opc == 7);
				size_mode = SIZE8;
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				cmp = GET_IMM8();
				break;

			case 0x81:
				assert(instr.reg_opc == 7);
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				cmp = GET_IMM();
				break;

			case 0x83:
				assert(instr.reg_opc == 7);
				GET_RM(OPNUM_DST, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
				cmp = SEXT(size_mode == SIZE16 ? 16 : 32, GET_IMM8());
				break;

			default:
				BAD;
			}

			sub = SUB(val, cmp);
			SET_FLG_SUB(sub, val, cmp);
		}
		break;

		case X86_OPC_CMPS: {
			switch (instr.opcode_byte)
			{
			case 0xA6:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xA7: {
				Value *val, *df, *sub, *addr1, *addr2, *src1, *src2, *esi, *edi;
				BasicBlock *bb_next = _BB();

				if (instr.rep_prefix) {
					REP_start();
				}

				switch (addr_mode)
				{
				case ADDR16:
					esi = ZEXT32(LD_R16(ESI_idx));
					addr1 = ADD(LD_SEG_HIDDEN(SEG_offset + instr.seg, SEG_BASE_idx), esi);
					edi = ZEXT32(LD_R16(EDI_idx));
					addr2 = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				case ADDR32:
					esi = LD_R32(ESI_idx);
					addr1 = ADD(LD_SEG_HIDDEN(SEG_offset + instr.seg, SEG_BASE_idx), esi);
					edi = LD_R32(EDI_idx);
					addr2 = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				default:
					LIB86CPU_ABORT();
				}

				switch (size_mode)
				{
				case SIZE8:
					val = CONST32(1);
					src1 = LD_MEM(fn_idx[size_mode], addr1);
					src2 = LD_MEM(fn_idx[size_mode], addr2);
					sub = SUB(src1, src2);
					break;

				case SIZE16:
					val = CONST32(2);
					src1 = LD_MEM(fn_idx[size_mode], addr1);
					src2 = LD_MEM(fn_idx[size_mode], addr2);
					sub = SUB(src1, src2);
					break;

				case SIZE32:
					val = CONST32(4);
					src1 = LD_MEM(fn_idx[size_mode], addr1);
					src2 = LD_MEM(fn_idx[size_mode], addr2);
					sub = SUB(src1, src2);
					break;

				default:
					LIB86CPU_ABORT();
				}

				SET_FLG_SUB(sub, src1, src2);

				df = AND(LD_R32(EFLAGS_idx), CONST32(DF_MASK));
				BasicBlock *bb_sum = _BB();
				BasicBlock *bb_sub = _BB();
				BR_COND(bb_sum, bb_sub, ICMP_EQ(df, CONST32(0)), bb);

				bb = bb_sum;
				Value *esi_sum = ADD(esi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(esi_sum), ESI_idx) : ST_R32(esi_sum, ESI_idx);
				Value *edi_sum = ADD(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sum), EDI_idx) : ST_R32(edi_sum, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 1: {
					REPNZ();
				}
				break;

				case 2: {
					REPZ();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_sub;
				Value *esi_sub = SUB(esi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(esi_sub), ESI_idx) : ST_R32(esi_sub, ESI_idx);
				Value *edi_sub = SUB(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sub), EDI_idx) : ST_R32(edi_sub, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 1: {
					REPNZ();
				}
				break;

				case 2: {
					REPZ();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_next;
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case X86_OPC_CMPXCHG8B:   BAD;
		case X86_OPC_CMPXCHG:     BAD;
		case X86_OPC_CPUID:       BAD;
		case X86_OPC_CWD:         BAD;
		case X86_OPC_CWDE:        BAD;
		case X86_OPC_CWTD:        BAD;
		case X86_OPC_CWTL:        BAD;
		case X86_OPC_DAA:         BAD;
		case X86_OPC_DAS:         BAD;
		case X86_OPC_DEC:         BAD;
		case X86_OPC_DIV: {
			switch (instr.opcode_byte)
			{
			case 0xF6:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xF7: {
				assert(instr.reg_opc == 6);

				// TODO: division exceptions. This will happily try to divide by zero and doesn't care about overflows
				Value *val, *reg, *rm;
				switch (size_mode)
				{
				case SIZE8:
					reg = LD_R16(EAX_idx);
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					ST_REG_val(TRUNC8(UDIV(reg, ZEXT16(val))), GEP_R8L(EAX_idx));
					ST_REG_val(TRUNC8(UREM(reg, ZEXT16(val))), GEP_R8H(EAX_idx));
					break;

				case SIZE16:
					reg = OR(SHL(ZEXT32(LD_R16(EDX_idx)), CONST32(16)), ZEXT32(LD_R16(EAX_idx)));
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					ST_REG_val(TRUNC16(UDIV(reg, ZEXT32(val))), GEP_R16(EAX_idx));
					ST_REG_val(TRUNC16(UREM(reg, ZEXT32(val))), GEP_R16(EDX_idx));
					break;

				case SIZE32:
					reg = OR(SHL(ZEXT64(LD_R32(EDX_idx)), CONST64(32)), ZEXT64(LD_R32(EAX_idx)));
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					ST_REG_val(TRUNC32(UDIV(reg, ZEXT64(val))), GEP_R32(EAX_idx));
					ST_REG_val(TRUNC32(UREM(reg, ZEXT64(val))), GEP_R32(EDX_idx));
					break;

				default:
					LIB86CPU_ABORT();
				}
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case X86_OPC_ENTER:       BAD;
		case X86_OPC_HLT:         BAD;
		case X86_OPC_IDIV:        BAD;
		case X86_OPC_IMUL: {
			switch (instr.opcode_byte)
			{
			case 0xF6:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xF7: {
				assert(instr.reg_opc == 5);

				Value *val, *reg, *rm, *out;
				switch (size_mode)
				{
				case SIZE8:
					reg = LD_R8L(EAX_idx);
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					out = MUL(SEXT16(reg), SEXT16(val));
					ST_REG_val(out, GEP_R16(EAX_idx));
					ST_FLG_AUX(SHL(ZEXT32(NOT_ZERO(16, XOR(out, LD_R8L(EAX_idx)))), CONST32(31)));
					break;

				case SIZE16:
					reg = LD_R16(EAX_idx);
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					out = MUL(SEXT32(reg), SEXT32(val));
					ST_REG_val(TRUNC16(SHR(out, CONST32(16))), GEP_R16(EDX_idx));
					ST_REG_val(TRUNC16(out), GEP_R16(EAX_idx));
					ST_FLG_AUX(SHL(NOT_ZERO(32, XOR(SEXT32(LD_R16(EAX_idx)), out)), CONST32(31)));
					break;

				case SIZE32:
					reg = LD_R32(EAX_idx);
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					out = MUL(SEXT64(reg), SEXT64(val));
					ST_REG_val(TRUNC32(SHR(out, CONST64(32))), GEP_R32(EDX_idx));
					ST_REG_val(TRUNC32(out), GEP_R32(EAX_idx));
					ST_FLG_AUX(SHL(TRUNC32(NOT_ZERO(64, XOR(ZEXT64(LD_R32(EAX_idx)), out))), CONST32(31)));
					break;

				default:
					LIB86CPU_ABORT();
				}
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_IN:          BAD;
		case X86_OPC_INC: {
			switch (instr.opcode_byte)
			{
			case 0x40:
			case 0x41:
			case 0x42:
			case 0x43:
			case 0x44:
			case 0x45:
			case 0x46:
			case 0x47: {
				Value *sum, *val, *one, *cf_old, *reg = GET_OP(OPNUM_SRC);
				switch (size_mode)
				{
				case SIZE16:
					val = LD_REG_val(reg);
					one = CONST16(1);
					sum = ADD(val, one);
					cf_old = LD_CF();
					ST_REG_val(sum, reg);
					break;

				case SIZE32:
					val = LD_REG_val(reg);
					one = CONST32(1);
					sum = ADD(val, one);
					cf_old = LD_CF();
					ST_REG_val(sum, reg);
					break;

				default:
					LIB86CPU_ABORT();
				}
				SET_FLG_SUM(sum, val, one);
				ST_FLG_AUX(OR(OR(cf_old, SHR(XOR(cf_old, LD_OF()), CONST32(1))), AND(LD_FLG_AUX(), CONST32(0x3FFFFFFF))));
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_INS:         BAD;
		case X86_OPC_INT3:        BAD;
		case X86_OPC_INT:         BAD;
		case X86_OPC_INTO:        BAD;
		case X86_OPC_INVD:        BAD;
		case X86_OPC_INVLPG:      BAD;
		case X86_OPC_IRET:        BAD;
		case X86_OPC_JECXZ:
		case X86_OPC_JO:
		case X86_OPC_JNO:
		case X86_OPC_JC:
		case X86_OPC_JNC:
		case X86_OPC_JZ:
		case X86_OPC_JNZ:
		case X86_OPC_JBE:
		case X86_OPC_JNBE:
		case X86_OPC_JS:
		case X86_OPC_JNS:
		case X86_OPC_JP:
		case X86_OPC_JNP:
		case X86_OPC_JL:
		case X86_OPC_JNL:
		case X86_OPC_JLE:
		case X86_OPC_JNLE: {
			Value *val;
			switch (instr.opcode_byte)
			{
			case 0x70:
			case 0x80:
				val = ICMP_NE(LD_OF(), CONST32(0)); // OF != 0
				break;

			case 0x71:
			case 0x81:
				val = ICMP_EQ(LD_OF(), CONST32(0)); // OF == 0
				break;

			case 0x72:
			case 0x82:
				val = ICMP_NE(LD_CF(), CONST32(2)); // CF != 0
				break;

			case 0x73:
			case 0x83:
				val = ICMP_EQ(LD_CF(), CONST32(0)); // CF == 0
				break;

			case 0x74:
			case 0x84:
				val = ICMP_EQ(LD_ZF(), CONST32(0)); // ZF != 0
				break;

			case 0x75:
			case 0x85:
				val = ICMP_NE(LD_ZF(), CONST32(0)); // ZF == 0
				break;

			case 0x76:
			case 0x86:
				val = OR(ICMP_NE(LD_CF(), CONST32(0)), ICMP_EQ(LD_ZF(), CONST32(0))); // CF != 0 OR ZF != 0
				break;

			case 0x77:
			case 0x87:
				val = AND(ICMP_EQ(LD_CF(), CONST32(0)), ICMP_NE(LD_ZF(), CONST32(0))); // CF == 0 AND ZF == 0
				break;

			case 0x78:
			case 0x88:
				val = ICMP_NE(LD_SF(), CONST32(0)); // SF != 0
				break;

			case 0x79:
			case 0x89:
				val = ICMP_EQ(LD_SF(), CONST32(0)); // SF == 0
				break;

			case 0x7A:
			case 0x8A:
				val = ICMP_EQ(LD_PARITY(LD_PF()), CONST8(0)); // PF != 0
				break;

			case 0x7B:
			case 0x8B:
				val = ICMP_NE(LD_PARITY(LD_PF()), CONST8(0)); // PF == 0
				break;

			case 0x7C:
			case 0x8C:
				val = ICMP_NE(LD_SF(), SHR(LD_OF(), CONST32(31))); // SF != OF
				break;

			case 0x7D:
			case 0x8D:
				val = ICMP_EQ(LD_SF(), SHR(LD_OF(), CONST32(31))); // SF == OF
				break;

			case 0x7E:
			case 0x8E:
				val = OR(ICMP_EQ(LD_ZF(), CONST32(0)), ICMP_NE(LD_SF(), SHR(LD_OF(), CONST32(31)))); // ZF != 0 OR SF != OF
				break;

			case 0x7F:
			case 0x8F:
				val = AND(ICMP_NE(LD_ZF(), CONST32(0)), ICMP_EQ(LD_SF(), SHR(LD_OF(), CONST32(31)))); // ZF == 0 AND SF == OF
				break;

			case 0xE3:
				val = addr_mode == ADDR16 ? ICMP_EQ(LD_R16(ECX_idx), CONST16(0)) : ICMP_EQ(LD_R32(ECX_idx), CONST32(0)); // ECX == 0
				break;

			default:
				LIB86CPU_ABORT();
			}

			Value *dst_pc = ALLOC32();
			BasicBlock *bb_jmp = _BB();
			BasicBlock *bb_exit = _BB();
			BasicBlock *bb_next = _BB();
			BR_COND(bb_jmp, bb_exit, val, bb);

			bb = bb_exit;
			Value *next_pc = calc_next_pc_emit(cpu, tc, bb, ptr_eip, bytes);
			ST(dst_pc, next_pc);
			BR_UNCOND(bb_next, bb);

			addr_t jump_eip = (pc - cpu_ctx->regs.cs_hidden.base) + bytes + instr.operand[OPNUM_SRC].rel;
			if (size_mode == SIZE16) {
				jump_eip &= 0x0000FFFF;
			}
			bb = bb_jmp;
			ST(GEP_EIP(), CONST32(jump_eip));
			ST(dst_pc, CONST32(jump_eip + cpu_ctx->regs.cs_hidden.base));
			BR_UNCOND(bb_next, bb);

			bb = bb_next;
			disas_ctx->next_pc = LD(dst_pc);

			translate_next = 0;
		}
		break;

		case X86_OPC_LAHF:        BAD;
		case X86_OPC_LAR:         BAD;
		case X86_OPC_LEA:         BAD;
		case X86_OPC_LEAVE:       BAD;
		case X86_OPC_LGDTD:
		case X86_OPC_LGDTL:
		case X86_OPC_LGDTW:
		case X86_OPC_LIDTD:
		case X86_OPC_LIDTL:
		case X86_OPC_LIDTW: {
			if (instr.operand[OPNUM_SRC].type == OPTYPE_REG) {
				RAISE(EXP_UD);
				disas_ctx->next_pc = CONST32(0); // unreachable
				translate_next = 0;
			}
			else {
				Value *rm, *limit, *base;
				uint8_t reg_idx;
				if (instr.opcode == X86_OPC_LGDTD || instr.opcode == X86_OPC_LGDTL || instr.opcode == X86_OPC_LGDTW) {
					assert(instr.reg_opc == 2);
					reg_idx = GDTR_idx;
				}
				else {
					assert(instr.reg_opc == 3);
					reg_idx = IDTR_idx;
				}
				GET_RM(OPNUM_SRC, assert(0);, limit = LD_MEM(MEM_LD16_idx, rm); rm = ADD(rm, CONST32(2)); base = LD_MEM(MEM_LD32_idx, rm););
				if (size_mode == SIZE16) {
					base = AND(base, CONST32(0x00FFFFFF));
				}
				ST_R48(base, reg_idx, R48_BASE);
				ST_R48(limit, reg_idx, R48_LIMIT);
			}
		}
		break;

		case X86_OPC_LJMP: // AT&T
		case X86_OPC_JMP: {
			switch (instr.opcode_byte)
			{
			case 0xE9:
			case 0xEB: {
				addr_t new_eip = (pc - cpu_ctx->regs.cs_hidden.base) + bytes + instr.operand[OPNUM_SRC].rel;
				if (size_mode == SIZE16) {
					new_eip &= 0x0000FFFF;
				}
				ST_R32(CONST32(new_eip), EIP_idx);
				disas_ctx->next_pc = CONST32(cpu_ctx->regs.cs_hidden.base + new_eip);
			}
			break;

			case 0xEA: {
				addr_t new_eip = instr.operand[OPNUM_SRC].imm;
				uint16_t new_sel = instr.operand[OPNUM_SRC].seg_sel;
				if (size_mode == SIZE16) {
					new_eip &= 0x0000FFFF;
				}
				if (cpu_ctx->hflags & HFLG_PE_MODE) {
					ljmp_pe_emit(cpu, tc, bb, CONST16(new_sel), CONST32(new_eip), ptr_eip);
					disas_ctx->next_pc = ADD(LD_SEG_HIDDEN(CS_idx, SEG_BASE_idx), CONST32(new_eip));
				}
				else {
					ST_R32(CONST32(new_eip), EIP_idx);
					ST_SEG(CONST16(new_sel), CS_idx);
					ST_SEG_HIDDEN(CONST32(static_cast<uint32_t>(new_sel) << 4), CS_idx, SEG_BASE_idx);
					disas_ctx->next_pc = CONST32((static_cast<uint32_t>(new_sel) << 4) + new_eip);
				}
			}
			break;

			case 0xFF: {
				if (instr.reg_opc == 5) {
					BAD;
#if 0
					if (cpu_ctx->hflags & HFLG_PE_MODE) {
						BAD_MODE;
					}
					assert(instr.operand[OPNUM_SRC].type == OPTYPE_MEM ||
						instr.operand[OPNUM_SRC].type == OPTYPE_MEM_DISP ||
						instr.operand[OPNUM_SRC].type == OPTYPE_SIB_MEM ||
						instr.operand[OPNUM_SRC].type == OPTYPE_SIB_DISP);
					Value *new_eip, *new_sel;
					Value *sel_addr, *offset_addr = GET_OP(OPNUM_SRC);
					if (size_mode == SIZE16) {
						new_eip = ZEXT32(LD_MEM(MEM_LD16_idx, offset_addr));
						sel_addr = ADD(offset_addr, CONST32(2));
					}
					else {
						new_eip = LD_MEM(MEM_LD32_idx, offset_addr);
						sel_addr = ADD(offset_addr, CONST32(4));
					}
					new_sel = LD_MEM(MEM_LD16_idx, sel_addr);

					ST_R32(new_eip, EIP_idx);
					ST_SEG(new_sel, CS_idx);
					ST_SEG_HIDDEN(SHL(ZEXT32(new_sel), CONST32(4)), CS_idx, SEG_BASE_idx);
					disas_ctx->next_pc = ADD(LD_SEG_HIDDEN(CS_idx, SEG_BASE_idx), new_eip);
#endif
				}
				else if(instr.reg_opc == 4) {
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

			translate_next = 0;
		}
		break;

		case X86_OPC_LLDT:        BAD;
		case X86_OPC_LMSW:        BAD;
		case X86_OPC_LODS: {
			switch (instr.opcode_byte)
			{
			case 0xAC:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xAD: {
				Value *val, *df, *addr, *src, *esi;
				BasicBlock *bb_next = _BB();

				if (instr.rep_prefix) {
					REP_start();
				}

				switch (addr_mode)
				{
				case ADDR16:
					esi = ZEXT32(LD_R16(ESI_idx));
					addr = ADD(LD_SEG_HIDDEN(SEG_offset + instr.seg, SEG_BASE_idx), esi);
					break;

				case ADDR32:
					esi = LD_R32(ESI_idx);
					addr = ADD(LD_SEG_HIDDEN(SEG_offset + instr.seg, SEG_BASE_idx), esi);
					break;

				default:
					LIB86CPU_ABORT();
				}

				switch (size_mode)
				{
				case SIZE8:
					val = CONST32(1);
					src = LD_MEM(fn_idx[size_mode], addr);
					ST_R8L(src, EAX_idx);
					break;

				case SIZE16:
					val = CONST32(2);
					src = LD_MEM(fn_idx[size_mode], addr);
					ST_R16(src, EAX_idx);
					break;

				case SIZE32:
					val = CONST32(4);
					src = LD_MEM(fn_idx[size_mode], addr);
					ST_R32(src, EAX_idx);
					break;

				default:
					LIB86CPU_ABORT();
				}

				df = AND(LD_R32(EFLAGS_idx), CONST32(DF_MASK));
				BasicBlock *bb_sum = _BB();
				BasicBlock *bb_sub = _BB();
				BR_COND(bb_sum, bb_sub, ICMP_EQ(df, CONST32(0)), bb);

				bb = bb_sum;
				Value *esi_sum = ADD(esi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(esi_sum), ESI_idx) : ST_R32(esi_sum, ESI_idx);
				switch (instr.rep_prefix)
				{
				case 2: {
					REP();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_sub;
				Value *esi_sub = SUB(esi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(esi_sub), ESI_idx) : ST_R32(esi_sub, ESI_idx);
				switch (instr.rep_prefix)
				{
				case 2: {
					REP();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_next;
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case X86_OPC_LOOP:
		case X86_OPC_LOOPE:
		case X86_OPC_LOOPNE: {
			Value *val, *zero, *zf;
			switch (instr.opcode_byte)
			{
			case 0xE0:
				zf = ICMP_NE(LD_ZF(), CONST32(0));
				break;

			case 0xE1:
				zf = ICMP_EQ(LD_ZF(), CONST32(0));
				break;

			case 0xE2:
				zf = CONSTs(1, 1);
				break;

			default:
				LIB86CPU_ABORT();
			}

			switch (addr_mode)
			{
			case ADDR16:
				val = SUB(LD_R16(ECX_idx), CONST16(1));
				ST_R16(val, ECX_idx);
				zero = CONST16(0);
				break;

			case ADDR32:
				val = SUB(LD_R32(ECX_idx), CONST32(1));
				ST_R32(val, ECX_idx);
				zero = CONST32(0);
				break;

			default:
				LIB86CPU_ABORT();
			}

			Value *dst_pc = ALLOC32();
			BasicBlock *bb_loop = _BB();
			BasicBlock *bb_exit = _BB();
			BasicBlock* bb_next = _BB();
			BR_COND(bb_loop, bb_exit, AND(ICMP_NE(val, zero), zf), bb);

			bb = bb_exit;
			Value *exit_pc = calc_next_pc_emit(cpu, tc, bb, ptr_eip, bytes);
			ST(dst_pc, exit_pc);
			BR_UNCOND(bb_next, bb);

			addr_t loop_eip = (pc - cpu_ctx->regs.cs_hidden.base) + bytes + instr.operand[OPNUM_SRC].rel;
			if (size_mode == SIZE16) {
				loop_eip &= 0x0000FFFF;
			}
			bb = bb_loop;
			ST(GEP_EIP(), CONST32(loop_eip));
			ST(dst_pc, CONST32(loop_eip + cpu_ctx->regs.cs_hidden.base));
			BR_UNCOND(bb_next, bb);

			bb = bb_next;
			disas_ctx->next_pc = LD(dst_pc);

			translate_next = 0;
		}
		break;

		case X86_OPC_LSL:         BAD;
		case X86_OPC_LDS:
		case X86_OPC_LES:
		case X86_OPC_LFS:
		case X86_OPC_LGS:
		case X86_OPC_LSS: {
			if (cpu_ctx->hflags & HFLG_PE_MODE) {
				BAD_MODE;
			}
			if (instr.operand[OPNUM_SRC].type == OPTYPE_REG) {
				RAISE(EXP_UD);
				disas_ctx->next_pc = CONST32(0); // unreachable
				translate_next = 0;
			}
			else {
				Value *offset, *sel, *rm;
				GET_RM(OPNUM_SRC, assert(0);, offset = LD_MEM(fn_idx[size_mode], rm);
				rm = size_mode == SIZE16 ? ADD(rm, CONST32(2)) : ADD(rm, CONST32(4));
				sel = LD_MEM(MEM_LD16_idx, rm););
				ST_REG_val(offset, GET_REG(OPNUM_DST));
				switch (instr.opcode_byte)
				{
				case 0xB2:
					ST_SEG(sel, SS_idx);
					ST_SEG_HIDDEN(SHL(ZEXT32(sel), CONST32(4)), SS_idx, SEG_BASE_idx);
					break;

				case 0xB4:
					ST_SEG(sel, FS_idx);
					ST_SEG_HIDDEN(SHL(ZEXT32(sel), CONST32(4)), FS_idx, SEG_BASE_idx);
					break;

				case 0xB5:
					ST_SEG(sel, GS_idx);
					ST_SEG_HIDDEN(SHL(ZEXT32(sel), CONST32(4)), GS_idx, SEG_BASE_idx);
					break;

				case 0xC4:
					ST_SEG(sel, ES_idx);
					ST_SEG_HIDDEN(SHL(ZEXT32(sel), CONST32(4)), ES_idx, SEG_BASE_idx);
					break;

				case 0xC5:
					ST_SEG(sel, DS_idx);
					ST_SEG_HIDDEN(SHL(ZEXT32(sel), CONST32(4)), DS_idx, SEG_BASE_idx);
					break;

				default:
					LIB86CPU_ABORT();
				}
			}
		}
		break;

		case X86_OPC_LTR:         BAD;
		case X86_OPC_MOV:
			switch (instr.opcode_byte)
			{
			case 0x20: {
				ST_R32(LD_REG_val(GET_REG(OPNUM_SRC)), instr.operand[OPNUM_DST].reg);
			}
			break;

			case 0x22: {
				Value *val = LD_REG_val(GET_REG(OPNUM_SRC));
				switch (instr.operand[OPNUM_DST].reg)
				{
				case 0:
				case 3:
					CallInst::Create(cpu->crN_fn, std::vector<Value *>{ cpu->ptr_cpu_ctx, val, CONST8(instr.operand[OPNUM_DST].reg), ptr_eip, CONST32(bytes) }, "", bb);
					break;
				case 2:
				case 4:
					BAD;

				default:
					LIB86CPU_ABORT();
				}

				disas_ctx->next_pc = calc_next_pc_emit(cpu, tc, bb, ptr_eip, bytes);
				translate_next = 0;
			}
			break;

			case 0x88:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x89: {
				Value *reg, *rm;
				reg = LD_REG_val(GET_REG(OPNUM_SRC));
				GET_RM(OPNUM_DST, ST_REG_val(reg, rm);, ST_MEM(fn_idx[size_mode], rm, reg););
			}
			break;

			case 0x8C: {
				if (cpu_ctx->hflags & HFLG_PE_MODE) {
					BAD_MODE;
				}
				Value *val, *rm;
				val = LD_SEG(instr.operand[OPNUM_SRC].reg + SEG_offset);
				GET_RM(OPNUM_DST, ST_REG_val(ZEXT32(val), IBITCAST32(rm));, ST_MEM(MEM_LD16_idx, rm, val););
			}
			break;

			case 0x8E: {
				if (cpu_ctx->hflags & HFLG_PE_MODE) {
					BAD_MODE;
				}
				if (instr.operand[OPNUM_DST].reg == 1 || instr.operand[OPNUM_DST].reg > 5) {
					RAISE(EXP_UD);
					disas_ctx->next_pc = CONST32(0); // unreachable
				}
				else {
					Value *val, *rm;
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(MEM_LD16_idx, rm););
					ST_SEG(val, instr.operand[OPNUM_DST].reg + SEG_offset);
					ST_SEG_HIDDEN(SHL(ZEXT32(val), CONST32(4)), instr.operand[OPNUM_DST].reg + SEG_offset, SEG_BASE_idx);
				}
				translate_next = 0;
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
				Value *reg8 = GET_OP(OPNUM_DST);
				ST_REG_val(GET_IMM8(), reg8);
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
				Value *reg = GET_OP(OPNUM_DST);
				ST_REG_val(GET_IMM(), reg);
			}
			break;

			case 0xC6:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xC7: {
				Value *rm;
				GET_RM(OPNUM_DST, ST_REG_val(GET_IMM(), rm);, ST_MEM(fn_idx[size_mode], rm, GET_IMM()););
			}
			break;

			default:
				BAD;
			}
			break;

		case X86_OPC_MOVS: {
			switch (instr.opcode_byte)
			{
			case 0xA4:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xA5: {
				Value *val, *df, *addr1, *addr2, *src, *esi, *edi;
				BasicBlock *bb_next = _BB();

				if (instr.rep_prefix) {
					REP_start();
				}

				switch (addr_mode)
				{
				case ADDR16:
					esi = ZEXT32(LD_R16(ESI_idx));
					addr1 = ADD(LD_SEG_HIDDEN(SEG_offset + instr.seg, SEG_BASE_idx), esi);
					edi = ZEXT32(LD_R16(EDI_idx));
					addr2 = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				case ADDR32:
					edi = LD_R32(ESI_idx);
					addr1 = ADD(LD_SEG_HIDDEN(SEG_offset + instr.seg, SEG_BASE_idx), esi);
					edi = LD_R32(EDI_idx);
					addr2 = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				default:
					LIB86CPU_ABORT();
				}

				switch (size_mode)
				{
				case SIZE8:
					val = CONST32(1);
					src = LD_MEM(fn_idx[size_mode], addr1);
					ST_MEM(fn_idx[size_mode], addr2, src);
					break;

				case SIZE16:
					val = CONST32(2);
					src = LD_MEM(fn_idx[size_mode], addr1);
					ST_MEM(fn_idx[size_mode], addr2, src);
					break;

				case SIZE32:
					val = CONST32(4);
					src = LD_MEM(fn_idx[size_mode], addr1);
					ST_MEM(fn_idx[size_mode], addr2, src);
					break;

				default:
					LIB86CPU_ABORT();
				}

				df = AND(LD_R32(EFLAGS_idx), CONST32(DF_MASK));
				BasicBlock *bb_sum = _BB();
				BasicBlock *bb_sub = _BB();
				BR_COND(bb_sum, bb_sub, ICMP_EQ(df, CONST32(0)), bb);

				bb = bb_sum;
				Value *esi_sum = ADD(esi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(esi_sum), ESI_idx) : ST_R32(esi_sum, ESI_idx);
				Value *edi_sum = ADD(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sum), EDI_idx) : ST_R32(edi_sum, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 2: {
					REP();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_sub;
				Value *esi_sub = SUB(esi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(esi_sub), ESI_idx) : ST_R32(esi_sub, ESI_idx);
				Value *edi_sub = SUB(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sub), EDI_idx) : ST_R32(edi_sub, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 2: {
					REP();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_next;
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case X86_OPC_MOVSX:       BAD;
		case X86_OPC_MOVSXB:      BAD;
		case X86_OPC_MOVSXW:      BAD;
		case X86_OPC_MOVZX:       BAD;
		case X86_OPC_MOVZXB:      BAD;
		case X86_OPC_MOVZXW:      BAD;
		case X86_OPC_MUL: {
			switch (instr.opcode_byte)
			{
			case 0xF6:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xF7: {
				assert(instr.reg_opc == 4);

				Value *val, *reg, *rm, *out;
				switch (size_mode)
				{
				case SIZE8:
					reg = LD_R8L(EAX_idx);
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					out = MUL(ZEXT16(reg), ZEXT16(val));
					ST_REG_val(out, GEP_R16(EAX_idx));
					ST_FLG_AUX(SHL(ZEXT32(NOT_ZERO(16, SHR(out, CONST16(8)))), CONST32(31)));
					break;

				case SIZE16:
					reg = LD_R16(EAX_idx);
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					out = MUL(ZEXT32(reg), ZEXT32(val));
					ST_REG_val(TRUNC16(SHR(out, CONST32(16))), GEP_R16(EDX_idx));
					ST_REG_val(TRUNC16(out), GEP_R16(EAX_idx));
					ST_FLG_AUX(SHL(NOT_ZERO(32, SHR(out, CONST32(16))), CONST32(31)));
					break;

				case SIZE32:
					reg = LD_R32(EAX_idx);
					GET_RM(OPNUM_SRC, val = LD_REG_val(rm);, val = LD_MEM(fn_idx[size_mode], rm););
					out = MUL(ZEXT64(reg), ZEXT64(val));
					ST_REG_val(TRUNC32(SHR(out, CONST64(32))), GEP_R32(EDX_idx));
					ST_REG_val(TRUNC32(out), GEP_R32(EAX_idx));
					ST_FLG_AUX(SHL(TRUNC32(NOT_ZERO(64, SHR(out, CONST64(32)))), CONST32(31)));
					break;

				default:
					LIB86CPU_ABORT();
				}
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case X86_OPC_NEG:         BAD;
		case X86_OPC_NOP:         BAD;
		case X86_OPC_NOT:         BAD;
		case X86_OPC_OR: {
			switch (instr.opcode_byte)
			{
			case 0x08:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x09: {
				Value *val, *rm, *src;
				src = LD_REG_val(GET_REG(OPNUM_SRC));
				GET_RM(OPNUM_DST, val = LD_REG_val(rm); val = OR(val, src); ST_REG_val(val, rm);, val = LD_MEM(fn_idx[size_mode], rm);
				val = OR(val, src); ST_MEM(fn_idx[size_mode], rm, val););
				SET_FLG(val, CONST32(0));
			}
			break;

			case 0x0C:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x0D: {
				Value *val, *eax;
				val = GET_IMM();
				eax = GET_REG(OPNUM_DST);
				val = OR(LD_REG_val(eax), val);
				ST_REG_val(val, eax);
				SET_FLG(val, CONST32(0));
			}
			break;

			case 0x80:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x81: {
				assert(instr.reg_opc == 1);

				Value *val, *rm, *src = GET_IMM();
				GET_RM(OPNUM_DST, val = LD_REG_val(rm); val = OR(val, src); ST_REG_val(val, rm);, val = LD_MEM(fn_idx[size_mode], rm);
				val = OR(val, src); ST_MEM(fn_idx[size_mode], rm, val););
				SET_FLG(val, CONST32(0));
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_OUT:
			switch (instr.opcode_byte)
			{
			case 0xEE: {
				ST_MEM(IO_ST8_idx, LD_R16(EDX_idx), LD_R8L(EAX_idx));
			}
			break;

			default:
				BAD;
			}
			break;

		case X86_OPC_OUTS:        BAD;
		case X86_OPC_POP:         BAD;
		case X86_OPC_POPA: {
			// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
			Value *sp = LD_R16(ESP_idx);
			Value *sp_add = size_mode == SIZE16 ? CONST16(2) : CONST16(4);
			for (int8_t reg_idx = EDI_idx; reg_idx >= EAX_idx; reg_idx--) {
				if (reg_idx != ESP_idx) {
					Value *reg = LD_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)));
					size_mode == SIZE16 ? ST_R16(reg, reg_idx) : ST_R32(reg, reg_idx);
				}
				sp = ADD(sp, sp_add);
			}
			ST_R16(sp, ESP_idx);
		}
		break;

		case X86_OPC_POPF:        BAD;
		case X86_OPC_PUSH:        BAD;
		case X86_OPC_PUSHA: {
			// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
			Value *sp = LD_R16(ESP_idx);
			Value *sp_sub = size_mode == SIZE16 ? CONST16(2) : CONST16(4);
			Value *esp_ori = size_mode == SIZE16 ? LD_R16(ESP_idx) : LD_R32(ESP_idx);
			for (uint8_t reg_idx = EAX_idx; reg_idx < ES_idx; reg_idx++) {
				sp = SUB(sp, sp_sub);
				if (reg_idx == ESP_idx) {
					ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), esp_ori);
				}
				else {
					ST_MEM(fn_idx[size_mode], ADD(ZEXT32(sp), LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)), size_mode == SIZE16 ? LD_R16(reg_idx) : LD_R32(reg_idx));
				}
			}
			ST_R16(sp, ESP_idx);
		}
		break;

		case X86_OPC_PUSHF:       BAD;
		case X86_OPC_RCL:         BAD;
		case X86_OPC_RCR:         BAD;
		case X86_OPC_RDMSR:       BAD;
		case X86_OPC_RDPMC:       BAD;
		case X86_OPC_RDTSC:       BAD;
		case X86_OPC_RET: {
			switch (instr.opcode_byte)
			{
			case 0xC3: {
				// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
				Value *sp = ZEXT32(LD_R16(ESP_idx));
				Value *ret_eip = LD_MEM(fn_idx[size_mode], ADD(sp, LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)));
				if (size_mode == SIZE16) {
					ret_eip = ZEXT32(ret_eip);
				}
				ST_R16(TRUNC16(ADD(sp, size_mode == SIZE16 ? CONST32(2) : CONST32(4))), ESP_idx);
				ST_R32(ret_eip, EIP_idx);

				disas_ctx->next_pc = ADD(CONST32(cpu_ctx->regs.cs_hidden.base), ret_eip);
			}
			break;

			default:
				BAD;
			}

			disas_ctx->flags |= DISAS_FLG_TC_INDIRECT;
			translate_next = 0;
		}
		break;

		case X86_OPC_LRET: // AT&T
		case X86_OPC_RETF: {
			switch (instr.opcode_byte)
			{
			case 0xCB: {
				// TODO: this should use the B flag of the current stack segment descriptor instead of being hardcoded to the sp
				Value *sp = ZEXT32(LD_R16(ESP_idx));
				Value *ret_eip = LD_MEM(fn_idx[size_mode], ADD(sp, LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)));
				if (size_mode == SIZE16) {
					ret_eip = ZEXT32(ret_eip);
				}
				sp = ADD(sp, size_mode == SIZE16 ? CONST32(2) : CONST32(4));
				Value *ret_cs = LD_MEM(fn_idx[size_mode], ADD(sp, LD_SEG_HIDDEN(SS_idx, SEG_BASE_idx)));
				ST_R16(TRUNC16(ADD(sp, size_mode == SIZE16 ? CONST32(2) : CONST32(4))), ESP_idx);
				ST_R32(ret_eip, EIP_idx);
				ST_R16(size_mode == SIZE16 ? ret_cs : TRUNC16(ret_cs), CS_idx);
				Value *ret_cs_base = SHL(size_mode == SIZE16 ? ZEXT32(ret_cs) : ret_cs, CONST32(4));
				ST_SEG_HIDDEN(ret_cs_base, CS_idx, SEG_BASE_idx);

				disas_ctx->next_pc = ADD(ret_cs_base, ret_eip);
			}
			break;

			default:
				BAD;
			}

			disas_ctx->flags |= DISAS_FLG_TC_INDIRECT;
			translate_next = 0;
		}
		break;

		case X86_OPC_ROL:         BAD;
		case X86_OPC_ROR:         BAD;
		case X86_OPC_RSM:         BAD;
		case X86_OPC_SAHF: {
			assert(instr.opcode_byte == 0x9E);

			Value *ah = ZEXT32(LD_R8H(EAX_idx));
			Value *sfd = SHR(AND(ah, CONST32(128)), CONST32(7));
			Value *pdb = SHL(XOR(CONST32(4), AND(ah, CONST32(4))), CONST32(6));
			Value *of_new = SHR(XOR(SHL(AND(ah, CONST32(1)), CONST32(31)), LD_OF()), CONST32(1));
			ST_FLG_RES(SHL(XOR(AND(ah, CONST32(64)), CONST32(64)), CONST32(2)));
			ST_FLG_AUX(OR(OR(OR(OR(SHL(AND(ah, CONST32(1)), CONST32(31)), SHR(AND(ah, CONST32(16)), CONST32(1))), of_new), sfd), pdb));
		}
		break;

		case X86_OPC_SAL:         BAD;
		case X86_OPC_SAR:         BAD;
		case X86_OPC_SBB:         BAD;
		case X86_OPC_SCAS: {
			switch (instr.opcode_byte)
			{
			case 0xAE:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xAF: {
				Value *val, *df, *sub, *addr, *src, *edi, *eax;
				BasicBlock *bb_next = _BB();

				if (instr.rep_prefix) {
					REP_start();
				}

				switch (addr_mode)
				{
				case ADDR16:
					edi = ZEXT32(LD_R16(EDI_idx));
					addr = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				case ADDR32:
					edi = LD_R32(EDI_idx);
					addr = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				default:
					LIB86CPU_ABORT();
				}

				switch (size_mode)
				{
				case SIZE8:
					val = CONST32(1);
					src = LD_MEM(fn_idx[size_mode], addr);
					eax = LD_R8L(EAX_idx);
					sub = SUB(eax, src);
					break;

				case SIZE16:
					val = CONST32(2);
					src = LD_MEM(fn_idx[size_mode], addr);
					eax = LD_R16(EAX_idx);
					sub = SUB(eax, src);
					break;

				case SIZE32:
					val = CONST32(4);
					src = LD_MEM(fn_idx[size_mode], addr);
					eax = LD_R32(EAX_idx);
					sub = SUB(eax, src);
					break;

				default:
					LIB86CPU_ABORT();
				}

				SET_FLG_SUB(sub, eax, src);

				df = AND(LD_R32(EFLAGS_idx), CONST32(DF_MASK));
				BasicBlock *bb_sum = _BB();
				BasicBlock *bb_sub = _BB();
				BR_COND(bb_sum, bb_sub, ICMP_EQ(df, CONST32(0)), bb);

				bb = bb_sum;
				Value *edi_sum = ADD(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sum), EDI_idx) : ST_R32(edi_sum, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 1: {
					REPNZ();
				}
				break;

				case 2: {
					REPZ();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_sub;
				Value *edi_sub = SUB(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sub), EDI_idx) : ST_R32(edi_sub, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 1: {
					REPNZ();
				}
				break;

				case 2: {
					REPZ();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_next;
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case X86_OPC_SETA:        BAD;
		case X86_OPC_SETB:        BAD;
		case X86_OPC_SETBE:       BAD;
		case X86_OPC_SETG:        BAD;
		case X86_OPC_SETGE:       BAD;
		case X86_OPC_SETL:        BAD;
		case X86_OPC_SETLE:       BAD;
		case X86_OPC_SETNB:       BAD;
		case X86_OPC_SETNE:       BAD;
		case X86_OPC_SETNO:       BAD;
		case X86_OPC_SETNS:       BAD;
		case X86_OPC_SETO:        BAD;
		case X86_OPC_SETPE:       BAD;
		case X86_OPC_SETPO:       BAD;
		case X86_OPC_SETS:        BAD;
		case X86_OPC_SETZ:        BAD;
		case X86_OPC_SGDTD:       BAD;
		case X86_OPC_SGDTL:       BAD;
		case X86_OPC_SGDTW:       BAD;
		case X86_OPC_SHL: {
			assert(instr.reg_opc == 4);

			switch (instr.opcode_byte)
			{
			case 0xC1: {
				uint8_t count = instr.operand[OPNUM_SRC].imm & 0x1F;
				if (count != 0) {
					Value *val, *rm, *cf, *of, *cf_mask, *of_mask;
					cf_mask = CONST32(1 << 32 - count);
					of_mask = size_mode == SIZE16 ? CONST32(1 << 15) : CONST32(1 << 31);
					GET_RM(OPNUM_DST, val = LD_REG_val(rm); val = size_mode == SIZE16 ? ZEXT32(val) : val; cf = AND(val, cf_mask); val = SHL(val, CONST32(count));
					of = AND(val, of_mask); val = size_mode == SIZE16 ? TRUNC16(val) : val; ST_REG_val(val, rm);, val = LD_MEM(fn_idx[size_mode], rm);
					val = size_mode == SIZE16 ? ZEXT32(val) : val; cf = AND(val, cf_mask); val = SHL(val, CONST32(count)); of = AND(val, of_mask);
					val = size_mode == SIZE16 ? TRUNC16(val) : val; ST_MEM(fn_idx[size_mode], rm, val););
					of = count == 1 ? (size_mode == SIZE16 ? SHL(of, CONST32(15)) : SHR(of, CONST32(1))) : of;
					SET_FLG(val, OR(SHL(cf, CONST32(count - 1)), of));
				}
			}
			break;

			case 0xD0: {
				Value *val, *rm, *cf;
				size_mode = SIZE8;
				GET_RM(OPNUM_SRC, val = LD_REG_val(rm); cf = AND(val, CONST8(0xC0)); val = SHL(val, CONST8(1)); ST_REG_val(val, rm);,
					val = LD_MEM(fn_idx[size_mode], rm); cf = AND(val, CONST8(0xC0)); val = SHL(val, CONST8(1)); ST_MEM(fn_idx[size_mode], rm, val););
				SET_FLG(val, SHL(ZEXT32(cf), CONST32(24)));
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_SHLD:        BAD;
		case X86_OPC_SHR: {
			assert(instr.reg_opc == 5);

			switch (instr.opcode_byte)
			{
			case 0xC1: {
				uint8_t count = instr.operand[OPNUM_SRC].imm & 0x1F;
				if (count != 0) {
					Value *val, *rm, *cf, *of, *cf_mask, *of_mask;
					cf_mask = CONST32(1 << count - 1);
					of_mask = size_mode == SIZE16 ? CONST32(1 << 15) : CONST32(1 << 31);
					GET_RM(OPNUM_DST, val = LD_REG_val(rm); val = size_mode == SIZE16 ? ZEXT32(val) : val; cf = AND(val, cf_mask); of = AND(val, of_mask);
					val = SHR(val, CONST32(count)); val = size_mode == SIZE16 ? TRUNC16(val) : val; ST_REG_val(val, rm); , val = LD_MEM(fn_idx[size_mode], rm);
					val = size_mode == SIZE16 ? ZEXT32(val) : val; cf = AND(val, cf_mask); of = AND(val, of_mask); val = SHR(val, CONST32(count));
					val = size_mode == SIZE16 ? TRUNC16(val) : val; ST_MEM(fn_idx[size_mode], rm, val););
					of = count == 1 ? SHL(XOR(cf, SHR(of, CONST32(size_mode == SIZE16 ? 15 : 31))), CONST32(30)) : of;
					SET_FLG(val, OR(SHL(cf, CONST32(31 - (count - 1))), of));
				}
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_SHRD:        BAD;
		case X86_OPC_SIDTD:       BAD;
		case X86_OPC_SIDTL:       BAD;
		case X86_OPC_SIDTW:       BAD;
		case X86_OPC_SLDT:        BAD;
		case X86_OPC_SMSW:        BAD;
		case X86_OPC_STC: {
			assert(instr.opcode_byte == 0xF9);

			Value *of_new = SHR(XOR(CONST32(0x80000000), LD_OF()), CONST32(1));
			ST_FLG_AUX(OR(AND(LD_FLG_AUX(), CONST32(0x3FFFFFFF)), OR(of_new, CONST32(0x80000000))));
		}
		break;

		case X86_OPC_STD: {
			assert(instr.opcode_byte == 0xFD);

			Value *eflags = LD_R32(EFLAGS_idx);
			eflags = OR(eflags, CONST32(DF_MASK));
			ST_R32(eflags, EFLAGS_idx);
		}
		break;

		case X86_OPC_STI:         BAD;
		case X86_OPC_STOS: {
			switch (instr.opcode_byte)
			{
			case 0xAA:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xAB: {
				Value *val, *df, *addr, *edi;
				BasicBlock *bb_next = _BB();

				if (instr.rep_prefix) {
					REP_start();
				}

				switch (addr_mode)
				{
				case ADDR16:
					edi = ZEXT32(LD_R16(EDI_idx));
					addr = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				case ADDR32:
					edi = LD_R32(EDI_idx);
					addr = ADD(LD_SEG_HIDDEN(ES_idx, SEG_BASE_idx), edi);
					break;

				default:
					LIB86CPU_ABORT();
				}

				switch (size_mode)
				{
				case SIZE8:
					val = CONST32(1);
					ST_MEM(fn_idx[size_mode], addr, LD_R8L(EAX_idx));
					break;

				case SIZE16:
					val = CONST32(2);
					ST_MEM(fn_idx[size_mode], addr, LD_R16(EAX_idx));
					break;

				case SIZE32:
					val = CONST32(4);
					ST_MEM(fn_idx[size_mode], addr, LD_R32(EAX_idx));
					break;

				default:
					LIB86CPU_ABORT();
				}

				df = AND(LD_R32(EFLAGS_idx), CONST32(DF_MASK));
				BasicBlock *bb_sum = _BB();
				BasicBlock *bb_sub = _BB();
				BR_COND(bb_sum, bb_sub, ICMP_EQ(df, CONST32(0)), bb);

				bb = bb_sum;
				Value *edi_sum = ADD(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sum), EDI_idx) : ST_R32(edi_sum, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 2: {
					REP();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_sub;
				Value *edi_sub = SUB(edi, val);
				addr_mode == ADDR16 ? ST_R16(TRUNC16(edi_sub), EDI_idx) : ST_R32(edi_sub, EDI_idx);
				switch (instr.rep_prefix)
				{
				case 2: {
					REP();
				}
				break;

				default:
					BR_UNCOND(bb_next, bb);
				}

				bb = bb_next;
			}
			break;

			default:
				LIB86CPU_ABORT();
			}
		}
		break;

		case X86_OPC_STR:         BAD;
		case X86_OPC_SUB: {
			switch (instr.opcode_byte)
			{
			case 0x80:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x83: {
				assert(instr.reg_opc == 5);

				Value *rm, *dst, *sub, *val = GET_IMM8();
				val = size_mode == SIZE16 ? SEXT16(val) : size_mode == SIZE32 ? SEXT32(val) : val;
				GET_RM(OPNUM_DST, dst = LD_REG_val(rm); sub = SUB(dst, val); ST_REG_val(sub, rm);,
					dst = LD_MEM(fn_idx[size_mode], rm); sub = SUB(dst, val); ST_MEM(fn_idx[size_mode], rm, sub););
				SET_FLG_SUB(sub, dst, val);
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_SYSENTER:    BAD;
		case X86_OPC_SYSEXIT:     BAD;
		case X86_OPC_TEST: {
			switch (instr.opcode_byte)
			{
			case 0xA8:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0xA9: {
				Value *val = AND(LD_REG_val(GET_REG(OPNUM_DST)), GET_IMM());
				SET_FLG(val, CONST32(0));
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_UD1:         BAD;
		case X86_OPC_UD2:         BAD;
		case X86_OPC_VERR:        BAD;
		case X86_OPC_VERW:        BAD;
		case X86_OPC_WBINVD:      BAD;
		case X86_OPC_WRMSR:       BAD;
		case X86_OPC_XADD:        BAD;
		case X86_OPC_XCHG: {
			switch (instr.opcode_byte)
			{
			case 0x86:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x87: {
				Value *reg, *val, *rm, *rm_src;
				rm_src = rm = GET_REG(OPNUM_SRC);
				reg = LD_REG_val(rm);
				GET_RM(OPNUM_DST, val = LD_REG_val(rm); ST_REG_val(reg, rm); ST_REG_val(val, rm_src);,
					val = LD_MEM(fn_idx[size_mode], rm); ST_MEM(fn_idx[size_mode], rm, reg); ST_REG_val(val, rm_src););
			}
			break;

			default:
				BAD;
			}
		}
		break;

		case X86_OPC_XLATB:       BAD;
		case X86_OPC_XOR:
			switch (instr.opcode_byte)
			{
			case 0x30:
				size_mode = SIZE8;
				[[fallthrough]];

			case 0x31: {
				Value *reg = GET_OP(OPNUM_SRC);
				Value *val, *rm;
				GET_RM(OPNUM_DST, val = LD_REG_val(rm); val = XOR(val, LD_REG_val(reg)); ST_REG_val(val, rm);,
					val = LD_MEM(fn_idx[size_mode], rm); val = XOR(val, LD_REG_val(reg)); ST_MEM(fn_idx[size_mode], rm, val););
				SET_FLG(val, CONST32(0));
			}
			break;

			default:
				BAD;
			}
			break;

		default:
			LIB86CPU_ABORT();
		}

		pc += bytes;

	} while ((translate_next | (disas_ctx->flags & DISAS_FLG_PAGE_CROSS)) == 1);

	disas_ctx->bb = bb;

	if (disas_ctx->next_pc == nullptr) {
		// this can happen when the last instruction crosses a page boundary and it's not a control flow change instruction

		disas_ctx->next_pc = calc_next_pc_emit(cpu, tc, bb, ptr_eip, bytes);
	}

	return LIB86CPU_SUCCESS;
}

lib86cpu_status
cpu_exec_tc(cpu_t *cpu)
{
	translated_code_t *prev_tc = nullptr, *ptr_tc = nullptr;
	addr_t pc;

	// main cpu loop
	while (true) {

		try {
			pc = mmu_translate_addr(cpu, get_pc(&cpu->cpu_ctx), 0, cpu->cpu_ctx.regs.eip, cpu_raise_exception);
		}
		catch (uint8_t expno) {
			// page fault during instruction fetching
			prev_tc = nullptr;
		}

		ptr_tc = tc_cache_search(cpu, pc);

		if (ptr_tc == nullptr) {

			// code block for this pc not present, we need to translate new code
			std::unique_ptr<translated_code_t> tc(new translated_code_t);
			tc->ctx = new LLVMContext();
			if (tc->ctx == nullptr) {
				return LIB86CPU_NO_MEMORY;
			}
			tc->mod = new Module(cpu->cpu_name, _CTX());
			if (tc->mod == nullptr) {
				delete tc->ctx;
				return LIB86CPU_NO_MEMORY;
			}

			FunctionType *fntype = create_tc_fntype(cpu, tc.get());
			Function *func = create_tc_prologue(cpu, tc.get(), fntype);

			// add to the module the external host functions that will be called by the translated guest code
			get_ext_fn(cpu, tc.get(), func);

			// prepare the disas ctx
			disas_ctx_t disas_ctx;
			disas_ctx.flags = (cpu->cpu_ctx.hflags & HFLG_CS32) >> CS32_SHIFT;
			disas_ctx.bb = _BB();
			disas_ctx.next_pc = nullptr;
			disas_ctx.virt_pc = get_pc(&cpu->cpu_ctx);
			disas_ctx.pc = pc;
			disas_ctx.instr_page_addr = disas_ctx.virt_pc & ~PAGE_MASK;

			// start guest code translation
			lib86cpu_status status = cpu_translate(cpu, &disas_ctx, tc.get());
			if (!LIB86CPU_CHECK_SUCCESS(status)) {
				delete tc->mod;
				delete tc->ctx;
				return status;
			}

			create_tc_epilogue(cpu, tc.get(), fntype, &disas_ctx);

			if (cpu->cpu_flags & CPU_PRINT_IR) {
				tc->mod->print(errs(), nullptr);
			}

			if (cpu->cpu_flags & CPU_CODEGEN_OPTIMIZE) {
				optimize(tc.get(), func);
				if (cpu->cpu_flags & CPU_PRINT_IR_OPTIMIZED) {
					tc->mod->print(errs(), nullptr);
				}
			}

			orc::ThreadSafeContext tsc(std::unique_ptr<LLVMContext>(tc->ctx));
			orc::ThreadSafeModule tsm(std::unique_ptr<Module>(tc->mod), tsc);
			if (cpu->jit->addIRModule(std::move(tsm))) {
				delete tc->mod;
				delete tc->ctx;
				return LIB86CPU_LLVM_ERROR;
			}

			tc->pc = pc;
			tc->cs_base = cpu->cpu_ctx.regs.cs_hidden.base;
			tc->flags = cpu->cpu_ctx.hflags | (cpu->cpu_ctx.regs.eflags & (TF_MASK | RF_MASK | AC_MASK));

			tc->ptr_code = reinterpret_cast<void *>(cpu->jit->lookup("start")->getAddress());
			assert(tc->ptr_code);
			tc->jmp_offset[0] = reinterpret_cast<void *>(cpu->jit->lookup("tail")->getAddress());
			tc->jmp_offset[1] = nullptr;
			tc->jmp_offset[2] = reinterpret_cast<void *>(cpu->jit->lookup("main")->getAddress());
			assert(tc->jmp_offset[0] && tc->jmp_offset[2]);

			{
				// now remove the function symbol names so that we can reuse them for other modules
				// NOTE: the mangle object must be destroyed when tc_cache_clear is called or else some symbols won't be removed when the jit
				// object is destroyed and llvm will assert
				orc::MangleAndInterner mangle(cpu->jit->getExecutionSession(), *cpu->dl);
				orc::SymbolNameSet module_symbol_names({ mangle("start"), mangle("tail"), mangle("main") });
				[[maybe_unused]] auto err = cpu->jit->getMainJITDylib().remove(module_symbol_names);
				assert(!err);
			}

			// llvm will delete the context and the module by itself, so we just null both the pointers now to prevent accidental usage
			tc->ctx = nullptr;
			tc->mod = nullptr;

			ptr_tc = tc.get();

			if (disas_ctx.flags & DISAS_FLG_PAGE_CROSS) {
				// this will leave behind the memory of the generated code block, however tc_cache_clear will still delete it later so
				// this is probably acceptable for now

				tc_run_code(&cpu->cpu_ctx, ptr_tc);
				prev_tc = nullptr;
				continue;
			}
			else {
				if (cpu->num_tc == CODE_CACHE_MAX_SIZE) {
					tc_cache_clear(cpu);
					prev_tc = nullptr;
				}
				tc_cache_insert(cpu, pc, std::move(tc));
			}
		}

		// see if we can link the previous tc with the current one
		if (prev_tc != nullptr &&
#if defined __i386 || defined _M_IX86
			(prev_tc->jmp_code_size == 20)) {
#else
#error don't know the size of the direct jump code of the tc on this platform
#endif
			tc_link_direct(prev_tc, ptr_tc, pc);
		}

		prev_tc = tc_run_code(&cpu->cpu_ctx, ptr_tc);
	}
}
