#include <assert.h>
#include <string.h>
#include <gelf.h>
#include <sys/mman.h>

#include "mcount-arch.h"
#include "libmcount/mcount.h"
#include "utils/filter.h"

int mcount_get_register_arg(struct mcount_arg_context *ctx,
			    struct ftrace_arg_spec *spec)
{
	struct mcount_regs *regs = ctx->regs;
	int reg_idx;

	switch (spec->type) {
	case ARG_TYPE_REG:
		reg_idx = spec->reg_idx;
		break;
	case ARG_TYPE_INDEX:
		reg_idx = spec->idx; /* for integer arguments */
		break;
	case ARG_TYPE_FLOAT:
		reg_idx = spec->idx + X86_REG_FLOAT_BASE;
		break;
	case ARG_TYPE_STACK:
	default:
		return -1;
	}

	switch (reg_idx) {
	case X86_REG_RDI:
		ctx->val.i = ARG1(regs);
		break;
	case X86_REG_RSI:
		ctx->val.i = ARG2(regs);
		break;
	case X86_REG_RDX:
		ctx->val.i = ARG3(regs);
		break;
	case X86_REG_RCX:
		ctx->val.i = ARG4(regs);
		break;
	case X86_REG_R8:
		ctx->val.i = ARG5(regs);
		break;
	case X86_REG_R9:
		ctx->val.i = ARG6(regs);
		break;
	case X86_REG_XMM0:
		asm volatile ("movsd %%xmm0, %0\n" : "=m" (ctx->val.v));
		break;
	case X86_REG_XMM1:
		asm volatile ("movsd %%xmm1, %0\n" : "=m" (ctx->val.v));
		break;
	case X86_REG_XMM2:
		asm volatile ("movsd %%xmm2, %0\n" : "=m" (ctx->val.v));
		break;
	case X86_REG_XMM3:
		asm volatile ("movsd %%xmm3, %0\n" : "=m" (ctx->val.v));
		break;
	case X86_REG_XMM4:
		asm volatile ("movsd %%xmm4, %0\n" : "=m" (ctx->val.v));
		break;
	case X86_REG_XMM5:
		asm volatile ("movsd %%xmm5, %0\n" : "=m" (ctx->val.v));
		break;
	case X86_REG_XMM6:
		asm volatile ("movsd %%xmm6, %0\n" : "=m" (ctx->val.v));
		break;
	case X86_REG_XMM7:
		asm volatile ("movsd %%xmm7, %0\n" : "=m" (ctx->val.v));
		break;
	default:
		return -1;
	}

	return 0;
}

void mcount_get_stack_arg(struct mcount_arg_context *ctx,
			  struct ftrace_arg_spec *spec)
{
	int offset;

	switch (spec->type) {
	case ARG_TYPE_STACK:
		offset = spec->stack_ofs;
		break;
	case ARG_TYPE_INDEX:
		offset = spec->idx - ARCH_MAX_REG_ARGS;
		break;
	case ARG_TYPE_FLOAT:
		offset = (spec->idx - ARCH_MAX_FLOAT_REGS) * 2 - 1;
		break;
	case ARG_TYPE_REG:
	default:
		/* should not reach here */
		pr_err_ns("invalid stack access for arguments\n");
		break;
	}

	if (offset < 1 || offset > 100)
		pr_dbg("invalid stack offset: %d\n", offset);

	memcpy(ctx->val.v, ctx->stack_base + offset, spec->size);
}

void mcount_arch_get_arg(struct mcount_arg_context *ctx,
			 struct ftrace_arg_spec *spec)
{
	if (mcount_get_register_arg(ctx, spec) < 0)
		mcount_get_stack_arg(ctx, spec);
}

void mcount_arch_get_retval(struct mcount_arg_context *ctx,
			    struct ftrace_arg_spec *spec)
{
	/* type of return value cannot be FLOAT, so check format instead */
	if (spec->fmt != ARG_FMT_FLOAT)
		memcpy(ctx->val.v, ctx->retval, spec->size);
	else if (spec->size == 10) /* for long double type */
		asm volatile ("fstpt %0\n\tfldt %0" : "=m" (ctx->val.v));
	else
		asm volatile ("movsd %%xmm0, %0\n" : "=m" (ctx->val.v));
}

#define R_OFFSET_POS  2
#define PUSH_IDX_POS  1
#define JMP_OFS_POS   7
#define JMP_INSN_SIZE 6
#define PLTGOT_SIZE   8
#define PAD_SIZE      5

int mcount_arch_undo_bindnow(Elf *elf, struct symtabs *symtabs,
			     unsigned long offset, unsigned long pltgot_addr)
{
	unsigned idx;
	int got_idx;
	struct sym *sym;
	struct symtab *dsymtab;
	unsigned r_offset;
	unsigned long r_addr;
	unsigned long real_addr;
	void *target_addr;
	unsigned jump_offset;
	void *trampoline_buf;
	size_t i, trampoline_size;
	unsigned char trampoline[] = {
		0x68, 0x00, 0x00, 0x00, 0x00,        /* push $idx */
		0xff, 0x25, 0x00, 0x00, 0x00, 0x00,  /* jmp *(offset) */
		0xcc, 0xcc, 0xcc, 0xcc, 0xcc,        /* padding */
	};
	Elf_Scn *sec;
	const char *skip_syms[] = {
		"mcount", "__fentry__",
		"__cyg_profile_func_enter", "__cyg_profile_func_exit",
		"__cxa_finalize",  /* XXX: it caused segfault */
	};

	plthook_setup(symtabs);
	dsymtab = &symtabs->dsymtab;

	trampoline_size = (dsymtab->nr_sym + 1) * sizeof(trampoline);
	trampoline_buf = mmap(0, trampoline_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (trampoline_buf == MAP_FAILED)
		pr_err("failed to mmap trampoline for bind now");

	pr_dbg2("setup bind-now PLT trampoline at %#lx\n", trampoline_buf);

	for (idx = 0; idx < dsymtab->nr_sym; idx++) {
		sym = dsymtab->sym_names[idx];

		for (i = 0; i < ARRAY_SIZE(skip_syms); i++) {
			if (!strcmp(sym->name, skip_syms[i]))
				break;
		}
		if (i != ARRAY_SIZE(skip_syms))
			continue;

		memcpy(&r_offset, (void *)sym->addr + R_OFFSET_POS, sizeof(r_offset));
		r_addr = r_offset + sym->addr + JMP_INSN_SIZE;

		/* GOT index can be different than the symbol index */
		got_idx = (r_addr - pltgot_addr) / sizeof(long);

		target_addr = trampoline_buf + (idx * sizeof(trampoline));
		real_addr = setup_pltgot(got_idx, idx, target_addr);

		jump_offset = (dsymtab->nr_sym - idx - 1) * sizeof(trampoline) + PAD_SIZE;

		pr_dbg3("[%d] %s got idx %d, r_offset %lx, real address = %#lx, "
			"target addr = %p, jump offset = %#lx\n",
			idx, sym->name, got_idx, r_offset + sym->addr - offset + JMP_INSN_SIZE,
			real_addr, target_addr, jump_offset);

		/* make up the instruction and copy to the trampoline buffer */
		memcpy(&trampoline[PUSH_IDX_POS], &idx, sizeof(idx));
		memcpy(&trampoline[JMP_OFS_POS], &jump_offset, sizeof(jump_offset));
		memcpy(target_addr, trampoline, sizeof(trampoline));
	}

	sec = NULL;
	while ((sec = elf_nextscn(elf, sec)) != NULL) {
		size_t shstr_idx;
		GElf_Shdr shdr;
		char *shname;

		if (elf_getshdrstrndx(elf, &shstr_idx) < 0)
			return -1;

		if (gelf_getshdr(sec, &shdr) == NULL)
			return -1;

		shname = elf_strptr(elf, shstr_idx, shdr.sh_name);
		if (strcmp(shname, ".plt"))
			continue;

		real_addr = shdr.sh_addr + offset;
		break;
	}

	if (sec == NULL)
		return -1;

	pr_dbg2("real address to jump: %#lx\n", real_addr);
	memcpy(trampoline_buf + (idx * sizeof(trampoline)),
	       &real_addr, sizeof(real_addr));

	mprotect(trampoline_buf, trampoline_size, PROT_READ | PROT_EXEC);
	return 0;
}
