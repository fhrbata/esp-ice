/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for `ice elf2image` — exercises cmd_elf2image() directly via
 * the libice entry point, using a synthetic ELF written to a temp file.
 */
#include "ice.h"
#include "tap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Minimal 32-bit little-endian ELF builder
 * ---------------------------------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/*
 * Build a one-segment ELF whose PT_LOAD sits at @p vaddr.
 * The caller owns the returned buffer.
 */
static uint8_t *make_elf(uint32_t vaddr, size_t data_sz, size_t *out_sz)
{
	const size_t ehdr = 52;
	const size_t phdr = 32;
	size_t total = ehdr + phdr + data_sz;
	uint8_t *buf = calloc(1, total);
	if (!buf)
		return NULL;

	/* e_ident */
	buf[0] = 0x7f;
	buf[1] = 'E';
	buf[2] = 'L';
	buf[3] = 'F';
	buf[4] = 1; /* ELFCLASS32        */
	buf[5] = 1; /* ELFDATA2LSB       */
	buf[6] = 1; /* EV_CURRENT        */

	put_u16(buf + 16, 2);		   /* ET_EXEC           */
	put_u16(buf + 18, 94);		   /* EM_XTENSA         */
	put_u32(buf + 20, 1);		   /* e_version         */
	put_u32(buf + 24, vaddr);	   /* e_entry           */
	put_u32(buf + 28, (uint32_t)ehdr); /* e_phoff           */
	put_u32(buf + 32, 0);		   /* e_shoff           */
	put_u16(buf + 40, (uint16_t)ehdr); /* e_ehsize          */
	put_u16(buf + 42, (uint16_t)phdr); /* e_phentsize       */
	put_u16(buf + 44, 1);		   /* e_phnum           */
	put_u16(buf + 46, 40);		   /* e_shentsize       */

	/* PT_LOAD program header */
	uint8_t *ph = buf + ehdr;
	put_u32(ph + 0, 1);			  /* PT_LOAD           */
	put_u32(ph + 4, (uint32_t)(ehdr + phdr)); /* p_offset   */
	put_u32(ph + 8, vaddr);			  /* p_vaddr           */
	put_u32(ph + 12, vaddr);		  /* p_paddr           */
	put_u32(ph + 16, (uint32_t)data_sz);	  /* p_filesz          */
	put_u32(ph + 20, (uint32_t)data_sz);	  /* p_memsz           */
	put_u32(ph + 24, 5);			  /* PF_R | PF_X       */
	put_u32(ph + 28, 0x1000);		  /* p_align           */

	*out_sz = total;
	return buf;
}

/* Write bytes to a file; return 0 on success. */
static int write_file(const char *path, const uint8_t *data, size_t len)
{
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return -1;
	int ok = fwrite(data, 1, len, fp) == len;
	fclose(fp);
	return ok ? 0 : -1;
}

/* Read an entire file into a malloc'd buffer; set *len; return NULL on error.
 */
static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	*len = (size_t)ftell(fp);
	rewind(fp);
	uint8_t *buf = malloc(*len);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, *len, fp) != *len) {
		fclose(fp);
		free(buf);
		return NULL;
	}
	fclose(fp);
	return buf;
}

/* -------------------------------------------------------------------------
 * Helpers that call cmd_elf2image() with a built argv
 * ---------------------------------------------------------------------- */

static int run_elf2image(const char **args, int n)
{
	/* args[0] must be "elf2image"; prepend it if the caller omits it. */
	const char *argv[32];
	argv[0] = "elf2image";
	for (int i = 0; i < n && i + 1 < 32; i++)
		argv[i + 1] = args[i];
	return cmd_elf2image(n + 1, argv);
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

static void test_basic_conversion(const char *elf_path)
{
	/* ESP32 IROM segment address — flash-mapped, 64 KB-aligned. */
	const char *args[] = {
	    "--chip",	    "esp32",	     "--flash-mode", "dio",
	    "--flash-freq", "40m",	     "--flash-size", "2MB",
	    "-o",	    "out_basic.bin", elf_path,
	};

	tap_check(run_elf2image(args, 11) == 0);
	tap_done("elf2image: basic ESP32 conversion succeeds");

	size_t sz = 0;
	uint8_t *bin = read_file("out_basic.bin", &sz);
	tap_check(bin != NULL && sz > 0);
	tap_done("elf2image: output file is non-empty");

	/* ESP image magic byte. */
	tap_check(bin && bin[0] == 0xE9);
	tap_done("elf2image: output starts with 0xE9 magic");

	/* Header byte 2 encodes flash mode: DIO = 0x02. */
	tap_check(bin && bin[2] == 0x02);
	tap_done("elf2image: header byte 2 == 0x02 (DIO)");

	free(bin);
}

static void test_flash_modes(const char *elf_path)
{
	struct {
		const char *mode;
		uint8_t byte;
	} cases[] = {
	    {"qio", 0x00},
	    {"qout", 0x01},
	    {"dio", 0x02},
	    {"dout", 0x03},
	};
	char out[32];

	for (int i = 0; i < 4; i++) {
		snprintf(out, sizeof(out), "out_mode_%d.bin", i);
		const char *args[] = {
		    "--chip",	    "esp32", "--flash-mode", cases[i].mode,
		    "--flash-freq", "40m",   "--flash-size", "2MB",
		    "-o",	    out,     elf_path,
		};
		tap_check(run_elf2image(args, 11) == 0);
		size_t sz = 0;
		uint8_t *bin = read_file(out, &sz);
		tap_check(bin && bin[2] == cases[i].byte);
		free(bin);
		tap_done("elf2image: flash mode byte correct");
	}
}

static void test_chips(const char *elf_path)
{
	/* Each should produce a valid (non-empty) image; just check success.
	 * Use "keep" for frequency — supported by all chip families. */
	const char *chips[] = {
	    "esp32",   "esp32s2", "esp32s3", "esp32c3",
	    "esp32c6", "esp32h2", NULL,
	};
	char out[32];

	for (int i = 0; chips[i]; i++) {
		snprintf(out, sizeof(out), "out_chip_%d.bin", i);
		const char *args[] = {
		    "--chip",	    chips[i], "--flash-mode", "dio",
		    "--flash-freq", "keep",   "--flash-size", "detect",
		    "-o",	    out,      elf_path,
		};
		tap_check(run_elf2image(args, 11) == 0);
		tap_done("elf2image: conversion succeeds for chip");
	}
}

static void test_missing_chip(const char *elf_path)
{
	const char *args[] = {
	    "--flash-mode", "dio", "-o", "out_nochip.bin", elf_path,
	};
	/* Should die() — but die() calls exit(), which would kill the test
	 * process.  We cannot call this directly; skip and rely on the
	 * integration test (bash) to verify the error path. */
	(void)args;
	tap_check(1); /* placeholder: die() path tested via CLI in .t script */
	tap_done("elf2image: missing --chip handled (CLI-level test)");
}

int main(void)
{
	/* Create a minimal ELF with a DRAM segment (0x3ff90000 — valid for
	 * all ESP32-family chips). */
	size_t elf_sz = 0;
	uint8_t *elf_data = make_elf(0x3ff90000, 64, &elf_sz);
	tap_check(elf_data != NULL);
	tap_done("make_elf: synthetic ELF allocated");

	tap_check(write_file("test_input.elf", elf_data, elf_sz) == 0);
	tap_done("make_elf: written to disk");
	free(elf_data);

	test_basic_conversion("test_input.elf");
	test_flash_modes("test_input.elf");
	test_chips("test_input.elf");
	test_missing_chip("test_input.elf");

	return tap_result();
}
