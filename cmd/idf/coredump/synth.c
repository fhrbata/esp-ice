/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/coredump/synth.c
 * @brief BIN_V* -> ELF32 core file synthesiser.
 *
 * Mirrors @c esp_coredump.corefile.loader._extract_bin_corefile.
 * The on-wire BIN data section carries
 *
 *   for i in [0..task_num):
 *     TaskHeader { tcb_addr, stack_top, stack_end }   (12 bytes, LE)
 *     <tcb_bytes> (header.tcbsz bytes)
 *     <stack_bytes> (|stack_top - stack_end| bytes)
 *     -- aligned to a 4-byte boundary --
 *   for i in [0..segs_num):     // V2 / V2_1 only
 *     MemSegmentHeader { mem_start, mem_sz }          (8 bytes, LE)
 *     <mem_data> (mem_sz bytes)
 *
 * We turn that into an ELF32 core file: ELF header, program-header
 * table (one PT_NOTE up front, then PT_LOAD for each TCB / stack /
 * memory segment), notes blob (one NT_PRSTATUS per task with the
 * arch-specific register set extracted from the captured stack
 * frame), and the raw bytes of every PT_LOAD region.
 *
 * V1 carries no @c segs_num (CORE_FIELD_ABSENT in the parsed
 * header); we treat it as zero segments and process only the task
 * array.
 *
 * The IDF-specific notes upstream emits on top -- TASK_INFO,
 * ESP_CORE_DUMP_INFO, EXTRA_INFO -- are skipped here.  GDB's
 * @c{info threads} and @c{thread apply all bt} work without them;
 * they're only needed for upstream's @c print @c exccause and the
 * pretty task-name display.
 */
#include "synth.h"

#include "ice.h"
#include "loader.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* ELF / arch constants                                                */
/* ------------------------------------------------------------------ */

#define EI_NIDENT 16
#define ET_CORE 4
#define EM_XTENSA 94
#define EM_RISCV 243
#define EV_CURRENT 1
#define ELFCLASS32 1
#define ELFDATA2LSB 1

#define PT_LOAD 1
#define PT_NOTE 4

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/*
 * NT_PRSTATUS == 1.  Note that upstream's
 * _build_note_section('CORE', ElfFile.PT_LOAD, ...) reuses the
 * @c PT_LOAD constant (also 1) -- the comment is misleading; the
 * field on the wire is the ELF note type, not a program-header
 * type.  We name it correctly here.
 */
#define NT_PRSTATUS 1

/* Sizes of the fixed-shape arch register sets (PRSTATUS payload). */
#define XTENSA_REG_NUM 129
#define XTENSA_PRSTATUS_HDR 76
#define XTENSA_PRSTATUS_SIZE                                                   \
	(XTENSA_PRSTATUS_HDR + XTENSA_REG_NUM * 4) /* 592 */

#define RISCV_REG_NUM 32
#define RISCV_PRSTATUS_SIZE                                                    \
	204 /* fixed: 12 pad + cursig + 8 pad +                                \
	     * pid + 44 pad + 32 regs + 4 pad */

/* ------------------------------------------------------------------ */
/* LE helpers                                                          */
/* ------------------------------------------------------------------ */

static uint32_t rd_le32(const uint8_t *p)
{
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void sbuf_addle32(struct sbuf *sb, uint32_t v)
{
	uint8_t b[4];
	wr_le32(b, v);
	sbuf_add(sb, b, 4);
}

static void sbuf_zero(struct sbuf *sb, size_t n)
{
	static const uint8_t zeros[64] = {0};
	while (n > 0) {
		size_t chunk = n > sizeof zeros ? sizeof zeros : n;
		sbuf_add(sb, zeros, chunk);
		n -= chunk;
	}
}

/* Round up to a multiple of 4. */
static size_t roundup4(size_t v) { return (v + 3u) & ~(size_t)3u; }

/* ------------------------------------------------------------------ */
/* Xtensa register extraction                                          */
/* ------------------------------------------------------------------ */

/*
 * Stack-frame indices from the captured Xtensa exception entry --
 * see upstream xtensa.py XT_STK_*.  The full frame is 25 u32s; the
 * "solicited" entry (rc == 0) carries fewer values in different
 * slots.
 */
enum {
	XT_STK_EXIT = 0,
	XT_STK_PC = 1,
	XT_STK_PS = 2,
	XT_STK_AR_START = 3,
	XT_STK_AR_NUM = 16,
	XT_STK_SAR = 19,
	XT_STK_LBEG = 22,
	XT_STK_LEND = 23,
	XT_STK_LCOUNT = 24,
	XT_STK_FRMSZ = 25,
};

enum {
	XT_SOL_EXIT = 0,
	XT_SOL_PC = 1,
	XT_SOL_PS = 2,
	XT_SOL_AR_START = 4,
	XT_SOL_AR_NUM = 4,
};

/*
 * GDB-shape Xtensa register-set indices.  The full set is 129 u32s;
 * we only ever populate a subset (PC/PS/loop regs/SAR/AR[0..15])
 * and leave the rest zero.
 */
enum {
	X_REG_PC = 0,
	X_REG_PS = 1,
	X_REG_LB = 2,
	X_REG_LE = 3,
	X_REG_LC = 4,
	X_REG_SAR = 5,
	X_REG_AR_START = 64,
};

static int extract_xtensa_regs(const uint8_t *stack, size_t stack_len,
			       uint32_t out_regs[XTENSA_REG_NUM],
			       const char **err)
{
	if (stack_len < (size_t)XT_STK_FRMSZ * 4) {
		*err = "Xtensa task stack smaller than exception frame";
		return -1;
	}

	memset(out_regs, 0, XTENSA_REG_NUM * 4);

	uint32_t rc = rd_le32(stack + XT_STK_EXIT * 4);
	if (rc != 0) {
		/* Hardware exception entry: full register set. */
		out_regs[X_REG_PC] = rd_le32(stack + XT_STK_PC * 4);
		out_regs[X_REG_PS] = rd_le32(stack + XT_STK_PS * 4);
		for (int i = 0; i < XT_STK_AR_NUM; i++)
			out_regs[X_REG_AR_START + i] =
			    rd_le32(stack + (XT_STK_AR_START + i) * 4);
		out_regs[X_REG_SAR] = rd_le32(stack + XT_STK_SAR * 4);
		out_regs[X_REG_LB] = rd_le32(stack + XT_STK_LBEG * 4);
		out_regs[X_REG_LE] = rd_le32(stack + XT_STK_LEND * 4);
		out_regs[X_REG_LC] = rd_le32(stack + XT_STK_LCOUNT * 4);
		/*
		 * Crashed / running tasks (e.g. prvIdleTask) have the
		 * EXCM bit set in PS, which makes GDB unwind them as
		 * "non-windowed call0".  Match upstream: clear the
		 * inner CALLINC bit so the windowed unwinder runs.
		 */
		if (out_regs[X_REG_PS] & (1u << 5))
			out_regs[X_REG_PS] &= ~(1u << 4);
	} else {
		/* Solicited (yield) entry: fewer registers. */
		out_regs[X_REG_PC] = rd_le32(stack + XT_SOL_PC * 4);
		out_regs[X_REG_PS] = rd_le32(stack + XT_SOL_PS * 4);
		for (int i = 0; i < XT_SOL_AR_NUM; i++)
			out_regs[X_REG_AR_START + i] =
			    rd_le32(stack + (XT_SOL_AR_START + i) * 4);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* RISC-V register extraction                                          */
/* ------------------------------------------------------------------ */

static int extract_riscv_regs(const uint8_t *stack, size_t stack_len,
			      uint32_t out_regs[RISCV_REG_NUM],
			      const char **err)
{
	if (stack_len < (size_t)RISCV_REG_NUM * 4) {
		*err = "RISC-V task stack smaller than register set";
		return -1;
	}
	for (int i = 0; i < RISCV_REG_NUM; i++)
		out_regs[i] = rd_le32(stack + i * 4);
	return 0;
}

/* ------------------------------------------------------------------ */
/* PRSTATUS note builder                                               */
/* ------------------------------------------------------------------ */

/*
 * Append one ELF note record to @p notes:
 *   namesz (LE u32) | descsz (LE u32) | type (LE u32)
 *   name + NUL, padded to 4-byte boundary
 *   desc, padded to 4-byte boundary
 */
static void emit_note(struct sbuf *notes, const char *name, uint32_t type,
		      const void *desc, size_t desc_len)
{
	size_t name_len = strlen(name) + 1;
	size_t name_pad = roundup4(name_len) - name_len;
	size_t desc_pad = roundup4(desc_len) - desc_len;

	sbuf_addle32(notes, (uint32_t)name_len);
	sbuf_addle32(notes, (uint32_t)desc_len);
	sbuf_addle32(notes, type);
	sbuf_add(notes, name, name_len);
	sbuf_zero(notes, name_pad);
	sbuf_add(notes, desc, desc_len);
	sbuf_zero(notes, desc_pad);
}

/*
 * Build the PRSTATUS payload for one Xtensa task.  Layout:
 *   76-byte header (mostly zeros; pr_pid carries tcb_addr) +
 *   129 * u32 register set (pre-extracted by the caller).
 */
static void build_xtensa_prstatus(uint32_t tcb_addr, const uint32_t *regs,
				  uint8_t out[XTENSA_PRSTATUS_SIZE])
{
	memset(out, 0, XTENSA_PRSTATUS_SIZE);
	/* pr_pid lives at offset 24 in the 76-byte header. */
	wr_le32(out + 24, tcb_addr);
	for (int i = 0; i < XTENSA_REG_NUM; i++)
		wr_le32(out + XTENSA_PRSTATUS_HDR + i * 4, regs[i]);
}

/*
 * Build the PRSTATUS payload for one RISC-V task.  Layout from
 * upstream riscv.py: 12 pad, cursig (u16), 8 pad, pid (u32), 44
 * pad, 32 * u32 regs, 4 trailing pad = 204 bytes.
 */
static void build_riscv_prstatus(uint32_t tcb_addr, const uint32_t *regs,
				 uint8_t out[RISCV_PRSTATUS_SIZE])
{
	memset(out, 0, RISCV_PRSTATUS_SIZE);
	wr_le32(out + 24, tcb_addr); /* pr_pid */
	for (int i = 0; i < RISCV_REG_NUM; i++)
		wr_le32(out + 72 + i * 4, regs[i]);
}

/* ------------------------------------------------------------------ */
/* Wire-format parser                                                  */
/* ------------------------------------------------------------------ */

struct task_rec {
	uint32_t tcb_addr;
	uint32_t stack_top;
	uint32_t stack_end;
	const uint8_t *tcb;
	size_t tcb_len;
	const uint8_t *stack;
	size_t stack_len;
	uint32_t regs[XTENSA_REG_NUM]; /* RV uses first 32 only */
};

struct seg_rec {
	uint32_t mem_start;
	uint32_t mem_sz;
	const uint8_t *data;
};

static int parse_tasks(const uint8_t *p, size_t total, size_t *consumed,
		       uint32_t task_num, uint32_t tcbsz,
		       struct task_rec *tasks, const char **err)
{
	size_t pos = 0;

	for (uint32_t i = 0; i < task_num; i++) {
		struct task_rec *t = &tasks[i];

		if (pos + 12 > total) {
			*err = "task array truncated (header)";
			return -1;
		}
		t->tcb_addr = rd_le32(p + pos + 0);
		t->stack_top = rd_le32(p + pos + 4);
		t->stack_end = rd_le32(p + pos + 8);
		pos += 12;

		t->tcb_len = tcbsz;
		if (pos + t->tcb_len > total) {
			*err = "task array truncated (TCB)";
			return -1;
		}
		t->tcb = p + pos;
		pos += t->tcb_len;

		uint32_t stop = t->stack_top, send = t->stack_end;
		t->stack_len = (stop > send) ? (size_t)(stop - send)
					     : (size_t)(send - stop);
		if (pos + t->stack_len > total) {
			*err = "task array truncated (stack)";
			return -1;
		}
		t->stack = p + pos;
		pos += t->stack_len;

		/* Each task is padded to a 4-byte boundary. */
		size_t aligned = roundup4(pos);
		if (aligned > total) {
			*err = "task array truncated (alignment)";
			return -1;
		}
		pos = aligned;
	}

	*consumed = pos;
	return 0;
}

static int parse_segs(const uint8_t *p, size_t total, size_t pos,
		      uint32_t segs_num, struct seg_rec *segs, const char **err)
{
	for (uint32_t i = 0; i < segs_num; i++) {
		struct seg_rec *s = &segs[i];

		if (pos + 8 > total) {
			*err = "segment array truncated (header)";
			return -1;
		}
		s->mem_start = rd_le32(p + pos + 0);
		s->mem_sz = rd_le32(p + pos + 4);
		pos += 8;
		if (pos + s->mem_sz > total) {
			*err = "segment array truncated (data)";
			return -1;
		}
		s->data = p + pos;
		pos += s->mem_sz;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* ELF writer                                                          */
/* ------------------------------------------------------------------ */

/*
 * Write the ELF32 core file into @p out.  Each task contributes
 * (a) one NT_PRSTATUS note in the notes blob, (b) one PT_LOAD for
 * its TCB at @c tcb_addr, (c) one PT_LOAD for its stack at
 * @c min(stack_top, stack_end).  Each captured memory segment
 * contributes one PT_LOAD at its @c mem_start.  A single PT_NOTE
 * program header up front points at the notes blob.
 */
static void write_elf(struct sbuf *out, uint16_t e_machine, int is_xtensa,
		      const struct task_rec *tasks, uint32_t task_num,
		      const struct seg_rec *segs, uint32_t seg_num)
{
	const size_t prstatus_size =
	    is_xtensa ? XTENSA_PRSTATUS_SIZE : RISCV_PRSTATUS_SIZE;

	/* ---- Build the notes blob ------------------------------- */
	struct sbuf notes = SBUF_INIT;
	for (uint32_t i = 0; i < task_num; i++) {
		const struct task_rec *t = &tasks[i];
		uint8_t prstatus[XTENSA_PRSTATUS_SIZE]; /* >= riscv size */

		if (is_xtensa)
			build_xtensa_prstatus(t->tcb_addr, t->regs, prstatus);
		else
			build_riscv_prstatus(t->tcb_addr, t->regs, prstatus);

		emit_note(&notes, "CORE", NT_PRSTATUS, prstatus, prstatus_size);
	}

	/* ---- Compute layout ------------------------------------- */
	const size_t ehdr_size = 52;
	const size_t phdr_size = 32;
	const uint16_t phnum = (uint16_t)(1u + 2u * task_num + seg_num);
	const size_t phdrs_off = ehdr_size;
	const size_t notes_off = phdrs_off + (size_t)phnum * phdr_size;
	const size_t notes_size = notes.len;
	const size_t segs_off = notes_off + notes_size;

	/* ---- ELF header ----------------------------------------- */
	uint8_t ehdr[52] = {0};
	ehdr[0] = 0x7f;
	ehdr[1] = 'E';
	ehdr[2] = 'L';
	ehdr[3] = 'F';
	ehdr[4] = ELFCLASS32;
	ehdr[5] = ELFDATA2LSB;
	ehdr[6] = EV_CURRENT;
	/* ehdr[7..15] = OSABI / ABIVERSION / pad -- already zero */
	wr_le16(ehdr + 16, ET_CORE);
	wr_le16(ehdr + 18, e_machine);
	wr_le32(ehdr + 20, EV_CURRENT);
	wr_le32(ehdr + 24, 0);			 /* e_entry */
	wr_le32(ehdr + 28, (uint32_t)phdrs_off); /* e_phoff */
	wr_le32(ehdr + 32, 0);			 /* e_shoff */
	wr_le32(ehdr + 36, 0);			 /* e_flags */
	wr_le16(ehdr + 40, (uint16_t)ehdr_size); /* e_ehsize */
	wr_le16(ehdr + 42, (uint16_t)phdr_size); /* e_phentsize */
	wr_le16(ehdr + 44, phnum);		 /* e_phnum */
	wr_le16(ehdr + 46, 0);			 /* e_shentsize */
	wr_le16(ehdr + 48, 0);			 /* e_shnum */
	wr_le16(ehdr + 50, 0);			 /* e_shstrndx */
	sbuf_add(out, ehdr, sizeof ehdr);

	/* ---- Program-header table ------------------------------- */
	/* PT_NOTE first. */
	{
		uint8_t ph[32] = {0};
		wr_le32(ph + 0, PT_NOTE);
		wr_le32(ph + 4, (uint32_t)notes_off);
		wr_le32(ph + 8, 0);  /* p_vaddr */
		wr_le32(ph + 12, 0); /* p_paddr */
		wr_le32(ph + 16, (uint32_t)notes_size);
		wr_le32(ph + 20, 0); /* p_memsz */
		wr_le32(ph + 24, 0); /* p_flags */
		wr_le32(ph + 28, 1); /* p_align */
		sbuf_add(out, ph, sizeof ph);
	}

	/* PT_LOAD entries: TCB + stack per task, then memory segs. */
	size_t cur_off = segs_off;
	for (uint32_t i = 0; i < task_num; i++) {
		const struct task_rec *t = &tasks[i];

		/* TCB */
		uint8_t ph[32] = {0};
		wr_le32(ph + 0, PT_LOAD);
		wr_le32(ph + 4, (uint32_t)cur_off);
		wr_le32(ph + 8, t->tcb_addr);  /* p_vaddr */
		wr_le32(ph + 12, t->tcb_addr); /* p_paddr */
		wr_le32(ph + 16, (uint32_t)t->tcb_len);
		wr_le32(ph + 20, (uint32_t)t->tcb_len);
		wr_le32(ph + 24, PF_R | PF_W);
		wr_le32(ph + 28, 1);
		sbuf_add(out, ph, sizeof ph);
		cur_off += t->tcb_len;

		/* Stack */
		uint32_t s_addr =
		    (t->stack_top < t->stack_end) ? t->stack_top : t->stack_end;
		uint8_t ph2[32] = {0};
		wr_le32(ph2 + 0, PT_LOAD);
		wr_le32(ph2 + 4, (uint32_t)cur_off);
		wr_le32(ph2 + 8, s_addr);
		wr_le32(ph2 + 12, s_addr);
		wr_le32(ph2 + 16, (uint32_t)t->stack_len);
		wr_le32(ph2 + 20, (uint32_t)t->stack_len);
		wr_le32(ph2 + 24, PF_R | PF_W);
		wr_le32(ph2 + 28, 1);
		sbuf_add(out, ph2, sizeof ph2);
		cur_off += t->stack_len;
	}
	for (uint32_t i = 0; i < seg_num; i++) {
		const struct seg_rec *s = &segs[i];
		uint8_t ph[32] = {0};
		wr_le32(ph + 0, PT_LOAD);
		wr_le32(ph + 4, (uint32_t)cur_off);
		wr_le32(ph + 8, s->mem_start);
		wr_le32(ph + 12, s->mem_start);
		wr_le32(ph + 16, s->mem_sz);
		wr_le32(ph + 20, s->mem_sz);
		wr_le32(ph + 24, PF_R | PF_W);
		wr_le32(ph + 28, 1);
		sbuf_add(out, ph, sizeof ph);
		cur_off += s->mem_sz;
	}

	/* ---- Notes blob ----------------------------------------- */
	sbuf_add(out, notes.buf, notes.len);
	sbuf_release(&notes);

	/* ---- PT_LOAD payloads ----------------------------------- */
	for (uint32_t i = 0; i < task_num; i++) {
		const struct task_rec *t = &tasks[i];
		sbuf_add(out, t->tcb, t->tcb_len);
		sbuf_add(out, t->stack, t->stack_len);
	}
	for (uint32_t i = 0; i < seg_num; i++)
		sbuf_add(out, segs[i].data, segs[i].mem_sz);
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

int core_synth_elf(const struct core_header *h, const void *data,
		   size_t data_len, struct sbuf *out, const char **err)
{
	uint32_t chip_ver = CORE_CHIP_VER(h);
	const char *idf_name = core_chip_idf_name(chip_ver);

	if (!idf_name) {
		*err = "unknown chip in core dump";
		return -1;
	}

	/*
	 * Xtensa: esp32 / esp32s2 / esp32s3 (chip_ver 0, 2, 9).
	 * Everything else in the supported set is RISC-V.
	 */
	int is_xtensa = (chip_ver == 0u || chip_ver == 2u || chip_ver == 9u);
	uint16_t e_machine = is_xtensa ? EM_XTENSA : EM_RISCV;

	uint32_t task_num =
	    (h->task_num == CORE_FIELD_ABSENT) ? 0 : h->task_num;
	uint32_t segs_num =
	    (h->segs_num == CORE_FIELD_ABSENT) ? 0 : h->segs_num;
	uint32_t tcbsz = (h->tcbsz == CORE_FIELD_ABSENT) ? 0 : h->tcbsz;

	if (task_num == 0) {
		*err = "BIN core dump carries zero tasks";
		return -1;
	}

	struct task_rec *tasks = calloc(task_num, sizeof *tasks);
	struct seg_rec *segs = segs_num ? calloc(segs_num, sizeof *segs) : NULL;
	if (!tasks || (segs_num && !segs)) {
		free(tasks);
		free(segs);
		*err = "out of memory";
		return -1;
	}

	int rc = -1;
	size_t consumed = 0;
	if (parse_tasks(data, data_len, &consumed, task_num, tcbsz, tasks,
			err) < 0)
		goto done;
	if (parse_segs(data, data_len, consumed, segs_num, segs, err) < 0)
		goto done;

	for (uint32_t i = 0; i < task_num; i++) {
		struct task_rec *t = &tasks[i];
		int grows_down = (t->stack_end > t->stack_top);
		(void)grows_down; /* both archs assume grows-down today */
		int rrc;
		if (is_xtensa)
			rrc = extract_xtensa_regs(t->stack, t->stack_len,
						  t->regs, err);
		else
			rrc = extract_riscv_regs(t->stack, t->stack_len,
						 t->regs, err);
		if (rrc < 0)
			goto done;
	}

	write_elf(out, e_machine, is_xtensa, tasks, task_num, segs, segs_num);
	rc = 0;

done:
	free(tasks);
	free(segs);
	return rc;
}
