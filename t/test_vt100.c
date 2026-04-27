/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for vt100.c -- the inner-terminal vt100 emulator.
 *
 * Phases 1-5 covered:
 *   - parser byte classification (printable / C0 / CSI / OSC / charset)
 *   - putc / cursor / SGR / clears / DECSC-DECRC / DECSET ?7 ?25
 *   - scroll regions, IL/DL/ICH/DCH/SU/SD, implicit scroll
 *   - DSR synth (5n / 6n) with reply cap
 *   - cell-row → ANSI byte string serializer
 *   - vt100 → tui_log scrollback bridge
 */
#include "ice.h"
#include "tap.h"
#include "tui.h"
#include "vt100.h"

static void feed(struct vt100 *V, const char *s)
{
	vt100_input(V, s, strlen(s));
}

/* File-scope decorator used by the "decorator runs on grid rows" test. */
static int decorator_calls;
static int decorator_grid_calls;

static int test_decorator(const char *line, size_t len, void *ctx,
			  struct tui_overlay *ov, int max)
{
	(void)ctx;
	(void)ov;
	(void)max;
	decorator_calls++;
	for (size_t i = 0; i + 4 <= len; i++) {
		if (memcmp(line + i, "GRID", 4) == 0) {
			decorator_grid_calls++;
			break;
		}
	}
	return 0;
}

int main(void)
{
	/* vt100_new yields an idle 24x80 instance with no pending reply. */
	{
		struct vt100 *V = vt100_new(24, 80);
		tap_check(V != NULL);
		tap_check(vt100_rows(V) == 24);
		tap_check(vt100_cols(V) == 80);
		tap_check(vt100_idle(V));
		tap_check(vt100_reply(V)->len == 0);
		vt100_free(V);
		tap_done("vt100_new yields an idle 24x80 instance");
	}

	/* vt100_resize updates the grid dimensions. */
	{
		struct vt100 *V = vt100_new(24, 80);
		vt100_resize(V, 30, 100);
		tap_check(vt100_rows(V) == 30);
		tap_check(vt100_cols(V) == 100);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("vt100_resize updates rows and cols");
	}

	/* Printable bytes count toward `printable` only. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		feed(V, "hello world");
		tap_check(c->printable == 11);
		tap_check(c->c0 == 0);
		tap_check(c->csi == 0);
		tap_check(c->osc == 0);
		tap_check(c->esc == 0);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("printable bytes classified as printable");
	}

	/* C0 controls count toward `c0` only. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		feed(V, "\r\n\t\b");
		tap_check(c->c0 == 4);
		tap_check(c->printable == 0);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("C0 controls classified as c0");
	}

	/* CSI sequences dispatch once per final byte. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		feed(V, "\x1b[1;31mhello\x1b[0m");
		tap_check(c->csi == 2);
		tap_check(c->printable == 5);
		tap_check(c->esc == 0);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("CSI sequences dispatch once per final byte");
	}

	/* CSI split mid-byte across two vt100_input calls dispatches once. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		vt100_input(V, "\x1b[3", 3);
		tap_check(c->csi == 0);
		tap_check(!vt100_idle(V));
		vt100_input(V, "2m", 2);
		tap_check(c->csi == 1);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("CSI split across input boundaries dispatches once");
	}

	/* Private-marker CSI (linenoise's DECSET ?7 / ?25) classifies. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		feed(V, "\x1b[?25l\x1b[?25h");
		tap_check(c->csi == 2);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("CSI with private-marker prefix classifies");
	}

	/* OSC string terminated by BEL classifies once. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		feed(V, "\x1b]0;title\x07");
		tap_check(c->osc == 1);
		tap_check(c->printable == 0);
		tap_check(c->c0 == 0);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("OSC terminated by BEL dispatches once");
	}

	/* OSC split across input boundaries terminated by ST (ESC \). */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		vt100_input(V, "\x1b]0;tit", 7);
		tap_check(c->osc == 0);
		tap_check(!vt100_idle(V));
		vt100_input(V, "le\x1b\\", 4);
		tap_check(c->osc == 1);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("OSC terminated by ST across input boundaries");
	}

	/* Charset designation (ESC ( B) split across two calls. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		vt100_input(V, "\x1b", 1);
		tap_check(!vt100_idle(V));
		vt100_input(V, "(B", 2);
		tap_check(c->esc == 1);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("charset escape (ESC ( B) classified as esc");
	}

	/* CAN (0x18) and SUB (0x1A) cancel an in-flight escape sequence. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		vt100_input(V, "\x1b[1;3", 5); /* mid-CSI */
		tap_check(!vt100_idle(V));
		vt100_input(V, "\x18", 1); /* CAN */
		tap_check(vt100_idle(V));
		tap_check(c->csi == 0);
		feed(V, "x"); /* now plain ground */
		tap_check(c->printable == 1);
		vt100_free(V);
		tap_done("CAN cancels an in-flight CSI to GROUND");
	}

	/* CSI_IGNORE: a colon in params poisons the sequence -- no dispatch. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		feed(V, "\x1b[1:2m");
		tap_check(c->csi == 0);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("malformed CSI (colon in params) drops to ignore");
	}

	/* DEL (0x7F) is ignored in every state. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);

		feed(V, "\x7f\x7f\x7f");
		tap_check(c->printable == 0);
		tap_check(c->c0 == 0);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("DEL is ignored in GROUND");
	}

	/* Synthesized linenoise-style trace classifies cleanly. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);
		/*
		 * Mirrors what linenoise emits during prompt setup,
		 * editing, and a refresh: getColumns probe (DSR + CHA +
		 * DSR), CR + prompt + EL, user input, refresh, SGR-wrapped
		 * output, an OSC title, a charset designation, a tail.
		 *
		 * Expected counts (verified by hand):
		 *   printable = 27   "esp> " + "hello" + "esp> hello" +
		 *                    "world" + "ok"
		 *   c0        = 3    \r \r \n   (\x07 is OSC terminator)
		 *   csi       = 7    \x1b[6n,  \x1b[999C, \x1b[6n,
		 *                    \x1b[0K, \x1b[0K,
		 *                    \x1b[32m, \x1b[0m
		 *   osc       = 1    \x1b]0;ESP32\x07
		 *   esc       = 1    \x1b(B
		 */
		const char trace[] =
		    "\x1b[6n"	       /* DSR-CPR */
		    "\x1b[999C"	       /* CHA right-edge */
		    "\x1b[6n"	       /* DSR-CPR again */
		    "\r"	       /* CR */
		    "esp> "	       /* prompt (5 printable) */
		    "\x1b[0K"	       /* EL */
		    "hello"	       /* user input (5 printable) */
		    "\r"	       /* CR */
		    "esp> hello"       /* refresh (10 printable) */
		    "\x1b[0K"	       /* EL */
		    "\x1b[32m"	       /* SGR green */
		    "world"	       /* output (5 printable) */
		    "\x1b[0m"	       /* SGR reset */
		    "\n"	       /* LF */
		    "\x1b]0;ESP32\x07" /* OSC set window title */
		    "\x1b(B"	       /* charset US-ASCII */
		    "ok";	       /* tail (2 printable) */

		vt100_input(V, trace, sizeof(trace) - 1);
		tap_check(c->printable == 27);
		tap_check(c->c0 == 3);
		tap_check(c->csi == 7);
		tap_check(c->osc == 1);
		tap_check(c->esc == 1);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done(
		    "linenoise-style trace classifies cleanly, parser idle");
	}

	/* Fed byte-by-byte the same trace yields identical counters and
	 * leaves the parser idle -- proves split-resilience at every byte. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const struct vt100_counters *c = vt100_counters(V);
		const char trace[] = "\x1b[6n\x1b[999C\x1b[6n"
				     "\resp> \x1b[0Khello"
				     "\resp> hello\x1b[0K"
				     "\x1b[32mworld\x1b[0m\n"
				     "\x1b]0;ESP32\x07"
				     "\x1b(B"
				     "ok";

		for (size_t i = 0; i < sizeof(trace) - 1; i++)
			vt100_input(V, &trace[i], 1);

		tap_check(c->printable == 27);
		tap_check(c->c0 == 3);
		tap_check(c->csi == 7);
		tap_check(c->osc == 1);
		tap_check(c->esc == 1);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done("byte-by-byte feed yields the same classification");
	}

	/* ------------------------------------------------------------ */
	/*  Phase 2: putc + cursor + clears + SGR                       */
	/* ------------------------------------------------------------ */

	/* putc lands characters in the grid and advances the cursor. */
	{
		struct vt100 *V = vt100_new(24, 80);

		feed(V, "hi");
		tap_check(vt100_cell(V, 0, 0)->cp == 'h');
		tap_check(vt100_cell(V, 0, 1)->cp == 'i');
		tap_check(vt100_cell(V, 0, 2)->cp == 0);
		tap_check(vt100_cursor_row(V) == 0);
		tap_check(vt100_cursor_col(V) == 2);
		tap_check(!vt100_pending_wrap(V));
		vt100_free(V);
		tap_done("putc lands chars in the grid and advances cursor");
	}

	/* Initial cell defaults: cp=0 and SGR is "default" everywhere. */
	{
		struct vt100 *V = vt100_new(4, 6);
		const struct vt100_cell *cell;

		for (int r = 0; r < 4; r++) {
			for (int co = 0; co < 6; co++) {
				cell = vt100_cell(V, r, co);
				tap_check(cell->cp == 0);
				tap_check(cell->sgr.fg == VT100_DEFAULT_COLOR);
				tap_check(cell->sgr.bg == VT100_DEFAULT_COLOR);
				tap_check(cell->sgr.attrs == 0);
			}
		}
		tap_check(vt100_cell(V, 4, 0) == NULL);
		tap_check(vt100_cell(V, 0, 6) == NULL);
		vt100_free(V);
		tap_done("fresh grid is blank with default SGR");
	}

	/* CUP / HVP take 1-based row,col and translate to 0-based. */
	{
		struct vt100 *V = vt100_new(24, 80);

		feed(V, "\x1b[3;5H");
		tap_check(vt100_cursor_row(V) == 2);
		tap_check(vt100_cursor_col(V) == 4);
		feed(V, "\x1b[10;20f");
		tap_check(vt100_cursor_row(V) == 9);
		tap_check(vt100_cursor_col(V) == 19);
		feed(V, "\x1b[H");
		tap_check(vt100_cursor_row(V) == 0);
		tap_check(vt100_cursor_col(V) == 0);
		vt100_free(V);
		tap_done("CUP / HVP set absolute 1-based cursor positions");
	}

	/* CHA changes column only, leaves row alone. */
	{
		struct vt100 *V = vt100_new(24, 80);

		feed(V, "\x1b[5;10H"); /* row 4, col 9 */
		feed(V, "\x1b[3G");    /* col 2, row stays */
		tap_check(vt100_cursor_row(V) == 4);
		tap_check(vt100_cursor_col(V) == 2);
		vt100_free(V);
		tap_done("CHA changes column only");
	}

	/* CUU / CUD / CUF / CUB with default and explicit counts. */
	{
		struct vt100 *V = vt100_new(24, 80);

		feed(V, "\x1b[10;20H"); /* (9, 19) */
		feed(V, "\x1b[2A");
		tap_check(vt100_cursor_row(V) == 7);
		feed(V, "\x1b[B"); /* default = 1 */
		tap_check(vt100_cursor_row(V) == 8);
		feed(V, "\x1b[5C");
		tap_check(vt100_cursor_col(V) == 24);
		feed(V, "\x1b[3D");
		tap_check(vt100_cursor_col(V) == 21);
		/* Clamp at edges. */
		feed(V, "\x1b[H");
		feed(V, "\x1b[10A");
		tap_check(vt100_cursor_row(V) == 0);
		feed(V, "\x1b[10D");
		tap_check(vt100_cursor_col(V) == 0);
		vt100_free(V);
		tap_done("CUU/CUD/CUF/CUB with defaults + edge clamp");
	}

	/* EL 0 clears from cursor to end of line; cursor stays put. */
	{
		struct vt100 *V = vt100_new(4, 10);

		feed(
		    V,
		    "abcdefghij"); /* fills row 0, cur at (0,9), wrap pending */
		feed(V, "\x1b[5G\x1b[0K"); /* CHA col 5, EL 0 */
		tap_check(vt100_cell(V, 0, 0)->cp == 'a');
		tap_check(vt100_cell(V, 0, 3)->cp == 'd');
		tap_check(vt100_cell(V, 0, 4)->cp ==
			  0); /* cleared from col 4 */
		tap_check(vt100_cell(V, 0, 9)->cp == 0);
		tap_check(vt100_cursor_col(V) == 4);
		vt100_free(V);
		tap_done("EL 0 clears from cursor to end of line");
	}

	/* EL 1 clears from start of line to cursor inclusive. */
	{
		struct vt100 *V = vt100_new(4, 10);

		feed(V, "abcdefghij");
		feed(V, "\x1b[5G\x1b[1K"); /* col 5 (= 0-based 4), EL 1 */
		tap_check(vt100_cell(V, 0, 0)->cp == 0);
		tap_check(vt100_cell(V, 0, 4)->cp == 0); /* inclusive */
		tap_check(vt100_cell(V, 0, 5)->cp == 'f');
		tap_check(vt100_cell(V, 0, 9)->cp == 'j');
		vt100_free(V);
		tap_done("EL 1 clears from start of line to cursor");
	}

	/* EL 2 clears the entire line; cursor stays. */
	{
		struct vt100 *V = vt100_new(4, 10);

		feed(V, "abcdefghij");
		feed(V, "\x1b[5G\x1b[2K");
		for (int co = 0; co < 10; co++)
			tap_check(vt100_cell(V, 0, co)->cp == 0);
		vt100_free(V);
		tap_done("EL 2 clears the entire line");
	}

	/* ED 2 clears the whole grid. */
	{
		struct vt100 *V = vt100_new(3, 4);

		feed(V, "ab\ncd\nef"); /* row0=ab, row1=cd, row2=ef */
		feed(V, "\x1b[2J");
		for (int r = 0; r < 3; r++)
			for (int co = 0; co < 4; co++)
				tap_check(vt100_cell(V, r, co)->cp == 0);
		vt100_free(V);
		tap_done("ED 2 clears the whole grid");
	}

	/* SGR 31 sets fg = red on subsequent puts; SGR 0 resets. */
	{
		struct vt100 *V = vt100_new(2, 8);

		feed(V, "\x1b[31mR\x1b[0mN");
		const struct vt100_cell *r = vt100_cell(V, 0, 0);
		const struct vt100_cell *n = vt100_cell(V, 0, 1);

		tap_check(r->cp == 'R');
		tap_check(r->sgr.fg == 1);
		tap_check(n->cp == 'N');
		tap_check(n->sgr.fg == VT100_DEFAULT_COLOR);
		vt100_free(V);
		tap_done("SGR 31 sets fg, SGR 0 resets");
	}

	/* SGR 1;4;7 stacks bold + underline + reverse; 22/24/27 unstack. */
	{
		struct vt100 *V = vt100_new(2, 4);

		feed(V, "\x1b[1;4;7mX");
		const struct vt100_cell *x = vt100_cell(V, 0, 0);
		uint8_t want =
		    VT100_ATTR_BOLD | VT100_ATTR_UNDERLINE | VT100_ATTR_REVERSE;
		tap_check(x->sgr.attrs == want);

		feed(V, "\x1b[22mY");
		const struct vt100_cell *y = vt100_cell(V, 0, 1);
		want = VT100_ATTR_UNDERLINE | VT100_ATTR_REVERSE;
		tap_check(y->sgr.attrs == want);
		vt100_free(V);
		tap_done("SGR attr stacking and individual clear");
	}

	/* SGR bright fg/bg via 90-97 / 100-107 maps to colors 8-15. */
	{
		struct vt100 *V = vt100_new(2, 4);

		feed(V, "\x1b[92;105mZ");
		const struct vt100_cell *z = vt100_cell(V, 0, 0);

		tap_check(z->sgr.fg == 10); /* 8 + (92 - 90) */
		tap_check(z->sgr.bg == 13); /* 8 + (105 - 100) */
		vt100_free(V);
		tap_done("SGR bright colors map to 8-15");
	}

	/* DECSC saves cursor + SGR; DECRC restores both. */
	{
		struct vt100 *V = vt100_new(8, 16);

		feed(V, "\x1b[3;5H\x1b[31m");
		feed(V, "\x1b"
			"7"); /* DECSC */
		feed(V, "\x1b[7;9H\x1b[32m");
		tap_check(vt100_cursor_row(V) == 6);
		tap_check(vt100_cursor_col(V) == 8);
		feed(V, "\x1b"
			"8"); /* DECRC */
		tap_check(vt100_cursor_row(V) == 2);
		tap_check(vt100_cursor_col(V) == 4);
		feed(V, "X");
		tap_check(vt100_cell(V, 2, 4)->sgr.fg == 1); /* red */
		vt100_free(V);
		tap_done("DECSC / DECRC save and restore cursor + SGR");
	}

	/* DECSET ?25l hides the cursor; ?25h shows it. */
	{
		struct vt100 *V = vt100_new(4, 8);

		tap_check(vt100_cursor_visible(V));
		feed(V, "\x1b[?25l");
		tap_check(!vt100_cursor_visible(V));
		feed(V, "\x1b[?25h");
		tap_check(vt100_cursor_visible(V));
		vt100_free(V);
		tap_done("DECSET ?25 toggles cursor visibility");
	}

	/* Deferred wrap: write COLS chars to a row, then \r\x1b[0K
	 * clears the same row -- not the next. */
	{
		struct vt100 *V = vt100_new(4, 8);

		feed(V, "ABCDEFGH"); /* fills row 0 */
		tap_check(vt100_cursor_row(V) == 0);
		tap_check(vt100_cursor_col(V) == 7);
		tap_check(vt100_pending_wrap(V));
		feed(V, "\r\x1b[0K");
		tap_check(vt100_cursor_row(V) == 0);
		tap_check(vt100_cursor_col(V) == 0);
		tap_check(!vt100_pending_wrap(V));
		for (int co = 0; co < 8; co++)
			tap_check(vt100_cell(V, 0, co)->cp == 0);
		for (int co = 0; co < 8; co++)
			tap_check(vt100_cell(V, 1, co)->cp == 0);
		vt100_free(V);
		tap_done("deferred wrap: \\r\\x1b[0K clears same row");
	}

	/* Deferred wrap fires on the next printable byte. */
	{
		struct vt100 *V = vt100_new(4, 4);

		feed(V, "ABCD"); /* (0,3) pending_wrap */
		tap_check(vt100_pending_wrap(V));
		feed(V, "X");
		tap_check(vt100_cell(V, 1, 0)->cp == 'X');
		tap_check(vt100_cursor_row(V) == 1);
		tap_check(vt100_cursor_col(V) == 1);
		tap_check(!vt100_pending_wrap(V));
		vt100_free(V);
		tap_done("deferred wrap fires on next printable");
	}

	/* DECSET ?7l (autowrap off): pending_wrap is set but does not
	 * advance to a new row -- subsequent prints overwrite the last
	 * column. */
	{
		struct vt100 *V = vt100_new(4, 4);

		feed(V, "\x1b[?7l");
		feed(V, "ABCD"); /* fills row 0, pending_wrap */
		feed(V, "X");
		tap_check(vt100_cell(V, 0, 3)->cp == 'X');
		tap_check(vt100_cell(V, 1, 0)->cp == 0);
		tap_check(vt100_cursor_row(V) == 0);
		vt100_free(V);
		tap_done("DECSET ?7l pins printable bytes at last column");
	}

	/* Resize blanks the grid and clamps cursor + saved cursor. */
	{
		struct vt100 *V = vt100_new(8, 16);

		feed(V, "\x1b[5;10H");
		feed(V, "abc");
		feed(V, "\x1b"
			"7"); /* DECSC at (4, 12) */
		vt100_resize(V, 4, 8);
		tap_check(vt100_rows(V) == 4);
		tap_check(vt100_cols(V) == 8);
		tap_check(vt100_cursor_row(V) == 3);
		tap_check(vt100_cursor_col(V) == 7);
		tap_check(vt100_cell(V, 0, 0)->cp == 0);
		feed(V, "\x1b"
			"8"); /* DECRC -- saved (4,12) clamped to (3,7) */
		tap_check(vt100_cursor_row(V) == 3);
		tap_check(vt100_cursor_col(V) == 7);
		vt100_free(V);
		tap_done("resize blanks grid and clamps cursors");
	}

	/* Linenoise refreshSingleLine simulation: \r + prompt + buf +
	 * \x1b[0K + \r + \x1b[<plen+pos>C.  After this sequence the
	 * grid holds the prompt + buffer and the cursor is at the
	 * intended edit position. */
	{
		struct vt100 *V = vt100_new(24, 80);
		const char prompt[] = "esp> ";
		const char user[] = "hello";
		const int plen = (int)(sizeof(prompt) - 1);
		const int pos = 3; /* user has typed 3 chars when refreshing */
		char tail[16];

		feed(V, "\r");
		feed(V, prompt);
		feed(V, user);
		feed(V, "\x1b[0K");
		feed(V, "\r");
		snprintf(tail, sizeof(tail), "\x1b[%dC", plen + pos);
		feed(V, tail);

		tap_check(vt100_cursor_row(V) == 0);
		tap_check(vt100_cursor_col(V) == plen + pos);
		tap_check(vt100_cell(V, 0, 0)->cp == 'e');
		tap_check(vt100_cell(V, 0, 1)->cp == 's');
		tap_check(vt100_cell(V, 0, 2)->cp == 'p');
		tap_check(vt100_cell(V, 0, 3)->cp == '>');
		tap_check(vt100_cell(V, 0, 4)->cp == ' ');
		tap_check(vt100_cell(V, 0, 5)->cp == 'h');
		tap_check(vt100_cell(V, 0, 6)->cp == 'e');
		tap_check(vt100_cell(V, 0, 7)->cp == 'l');
		tap_check(vt100_cell(V, 0, 8)->cp == 'l');
		tap_check(vt100_cell(V, 0, 9)->cp == 'o');
		tap_check(vt100_cell(V, 0, 10)->cp == 0);
		tap_check(vt100_idle(V));
		vt100_free(V);
		tap_done(
		    "linenoise refreshSingleLine sequence reproduces grid");
	}

	/* ------------------------------------------------------------ */
	/*  Phase 3: scroll regions, line/char ops, implicit scroll     */
	/* ------------------------------------------------------------ */

	/* DECSTBM sets the scroll region and homes the cursor. */
	{
		struct vt100 *V = vt100_new(10, 8);

		tap_check(vt100_scroll_top(V) == 0);
		tap_check(vt100_scroll_bottom(V) == 9);
		feed(V, "\x1b[3;7r"); /* rows 2..6 0-based */
		tap_check(vt100_scroll_top(V) == 2);
		tap_check(vt100_scroll_bottom(V) == 6);
		tap_check(vt100_cursor_row(V) == 0);
		tap_check(vt100_cursor_col(V) == 0);
		feed(V, "\x1b[r"); /* reset to full screen */
		tap_check(vt100_scroll_top(V) == 0);
		tap_check(vt100_scroll_bottom(V) == 9);
		vt100_free(V);
		tap_done("DECSTBM sets and resets scroll region");
	}

	/* DECSTBM with invalid params (top >= bottom) is ignored. */
	{
		struct vt100 *V = vt100_new(10, 8);

		feed(V, "\x1b[5;5r");
		tap_check(vt100_scroll_top(V) == 0);
		tap_check(vt100_scroll_bottom(V) == 9);
		feed(V, "\x1b[8;3r");
		tap_check(vt100_scroll_top(V) == 0);
		tap_check(vt100_scroll_bottom(V) == 9);
		vt100_free(V);
		tap_done("DECSTBM with invalid params is ignored");
	}

	/* SU on a full-screen scroll region enqueues the dropped rows. */
	{
		struct vt100 *V = vt100_new(4, 4);
		const struct vt100_cell *r;

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD");
		feed(V, "\x1b[2S");
		tap_check(vt100_scrolled_count(V) == 2);
		r = vt100_scrolled_row(V, 0, NULL);
		tap_check(r[0].cp == 'A');
		r = vt100_scrolled_row(V, 1, NULL);
		tap_check(r[0].cp == 'B');
		tap_check(vt100_cell(V, 0, 0)->cp == 'C');
		tap_check(vt100_cell(V, 1, 0)->cp == 'D');
		tap_check(vt100_cell(V, 2, 0)->cp == 0);
		tap_check(vt100_cell(V, 3, 0)->cp == 0);
		vt100_free(V);
		tap_done("SU on full-screen region enqueues dropped rows");
	}

	/* SU within a partial scroll region does NOT enqueue. */
	{
		struct vt100 *V = vt100_new(4, 4);

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD");
		feed(V, "\x1b[2;3r"); /* region rows 1..2 (0-based) */
		feed(V, "\x1b[S");    /* SU 1 within partial region */
		tap_check(vt100_scrolled_count(V) == 0);
		tap_check(vt100_cell(V, 0, 0)->cp == 'A'); /* outside region */
		tap_check(vt100_cell(V, 1, 0)->cp == 'C'); /* shifted up */
		tap_check(vt100_cell(V, 2, 0)->cp == 0);   /* blanked */
		tap_check(vt100_cell(V, 3, 0)->cp == 'D'); /* outside region */
		vt100_free(V);
		tap_done("SU on partial region does not enqueue");
	}

	/* SD never enqueues; it just blanks the top of the region. */
	{
		struct vt100 *V = vt100_new(4, 4);

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD");
		feed(V, "\x1b[2T");
		tap_check(vt100_scrolled_count(V) == 0);
		tap_check(vt100_cell(V, 0, 0)->cp == 0);
		tap_check(vt100_cell(V, 1, 0)->cp == 0);
		tap_check(vt100_cell(V, 2, 0)->cp == 'A');
		tap_check(vt100_cell(V, 3, 0)->cp == 'B');
		vt100_free(V);
		tap_done("SD scrolls down without enqueuing");
	}

	/* IL inserts a blank line; lower lines shift down; bottom drops. */
	{
		struct vt100 *V = vt100_new(4, 4);

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD");
		feed(V, "\x1b[2;1H"); /* (1, 0) */
		feed(V, "\x1b[L");
		tap_check(vt100_cell(V, 0, 0)->cp == 'A');
		tap_check(vt100_cell(V, 1, 0)->cp == 0);
		tap_check(vt100_cell(V, 2, 0)->cp == 'B');
		tap_check(vt100_cell(V, 3, 0)->cp == 'C');
		tap_check(vt100_cursor_col(V) == 0);
		vt100_free(V);
		tap_done("IL inserts blank line, shifts down, drops bottom");
	}

	/* DL deletes a line; lower lines shift up; bottom blanks. */
	{
		struct vt100 *V = vt100_new(4, 4);

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD");
		feed(V, "\x1b[2;1H"); /* (1, 0) */
		feed(V, "\x1b[M");
		tap_check(vt100_cell(V, 0, 0)->cp == 'A');
		tap_check(vt100_cell(V, 1, 0)->cp == 'C');
		tap_check(vt100_cell(V, 2, 0)->cp == 'D');
		tap_check(vt100_cell(V, 3, 0)->cp == 0);
		vt100_free(V);
		tap_done("DL deletes line, shifts up, blanks bottom");
	}

	/* ICH inserts blanks at cursor; right side shifts right, falls off. */
	{
		struct vt100 *V = vt100_new(2, 6);

		feed(V, "abcdef");
		feed(V, "\x1b[3G"); /* col 3 -> cur_col=2 */
		feed(V, "\x1b[2@");
		tap_check(vt100_cell(V, 0, 0)->cp == 'a');
		tap_check(vt100_cell(V, 0, 1)->cp == 'b');
		tap_check(vt100_cell(V, 0, 2)->cp == 0);
		tap_check(vt100_cell(V, 0, 3)->cp == 0);
		tap_check(vt100_cell(V, 0, 4)->cp == 'c');
		tap_check(vt100_cell(V, 0, 5)->cp == 'd');
		vt100_free(V);
		tap_done("ICH inserts blanks at cursor, shifts right");
	}

	/* DCH removes chars at cursor; right side shifts left, blanks tail. */
	{
		struct vt100 *V = vt100_new(2, 6);

		feed(V, "abcdef");
		feed(V, "\x1b[3G");
		feed(V, "\x1b[2P");
		tap_check(vt100_cell(V, 0, 0)->cp == 'a');
		tap_check(vt100_cell(V, 0, 1)->cp == 'b');
		tap_check(vt100_cell(V, 0, 2)->cp == 'e');
		tap_check(vt100_cell(V, 0, 3)->cp == 'f');
		tap_check(vt100_cell(V, 0, 4)->cp == 0);
		tap_check(vt100_cell(V, 0, 5)->cp == 0);
		vt100_free(V);
		tap_done("DCH deletes chars, shifts left, blanks tail");
	}

	/* LF at scroll_bottom on a full-screen region triggers an
	 * implicit scroll that enqueues the top row. */
	{
		struct vt100 *V = vt100_new(3, 4);
		const struct vt100_cell *r;

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\n");
		tap_check(vt100_scrolled_count(V) == 1);
		r = vt100_scrolled_row(V, 0, NULL);
		tap_check(r[0].cp == 'A');
		tap_check(vt100_cell(V, 0, 0)->cp == 'B');
		tap_check(vt100_cell(V, 1, 0)->cp == 'C');
		tap_check(vt100_cell(V, 2, 0)->cp == 0);
		tap_check(vt100_cursor_row(V) == 2);
		vt100_free(V);
		tap_done("LF at scroll_bottom triggers implicit scroll");
	}

	/* LF at the bottom of a partial scroll region scrolls only
	 * within the region, leaving rows outside untouched. */
	{
		struct vt100 *V = vt100_new(5, 4);

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE");
		feed(V, "\x1b[2;4r"); /* region rows 1..3 */
		feed(V, "\x1b[4;1H"); /* (3, 0) -- region bottom */
		feed(V, "\n");
		tap_check(vt100_scrolled_count(V) == 0);
		tap_check(vt100_cell(V, 0, 0)->cp == 'A');
		tap_check(vt100_cell(V, 1, 0)->cp == 'C');
		tap_check(vt100_cell(V, 2, 0)->cp == 'D');
		tap_check(vt100_cell(V, 3, 0)->cp == 0);
		tap_check(vt100_cell(V, 4, 0)->cp == 'E');
		vt100_free(V);
		tap_done(
		    "LF inside partial region scrolls within the region only");
	}

	/* Autowrap past last column at scroll_bottom triggers scroll. */
	{
		struct vt100 *V = vt100_new(2, 4);
		const struct vt100_cell *r;

		feed(V, "abcd");
		feed(V, "efgh");
		tap_check(vt100_scrolled_count(V) == 0);
		feed(V, "X");
		tap_check(vt100_scrolled_count(V) == 1);
		r = vt100_scrolled_row(V, 0, NULL);
		tap_check(r[0].cp == 'a');
		tap_check(r[3].cp == 'd');
		tap_check(vt100_cell(V, 0, 0)->cp == 'e');
		tap_check(vt100_cell(V, 0, 3)->cp == 'h');
		tap_check(vt100_cell(V, 1, 0)->cp == 'X');
		tap_check(vt100_cell(V, 1, 1)->cp == 0);
		vt100_free(V);
		tap_done(
		    "autowrap past last col at scroll_bottom triggers scroll");
	}

	/* DECSET ?47 / ?1047 / ?1049 toggle the alt_screen flag. */
	{
		struct vt100 *V = vt100_new(4, 4);

		tap_check(!vt100_alt_screen(V));
		feed(V, "\x1b[?1049h");
		tap_check(vt100_alt_screen(V));
		feed(V, "\x1b[?1049l");
		tap_check(!vt100_alt_screen(V));
		feed(V, "\x1b[?47h");
		tap_check(vt100_alt_screen(V));
		feed(V, "\x1b[?1047l");
		tap_check(!vt100_alt_screen(V));
		vt100_free(V);
		tap_done("?47 / ?1047 / ?1049 toggle alt_screen flag");
	}

	/* Alt-screen disables the scrollback enqueue path. */
	{
		struct vt100 *V = vt100_new(2, 4);

		feed(V, "\x1b[?1049h");
		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\n");
		tap_check(vt100_scrolled_count(V) == 0);
		vt100_free(V);
		tap_done("alt_screen disables scrollback enqueue");
	}

	/* vt100_drain_scrolled empties the queue. */
	{
		struct vt100 *V = vt100_new(2, 4);

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\n");
		tap_check(vt100_scrolled_count(V) > 0);
		vt100_drain_scrolled(V);
		tap_check(vt100_scrolled_count(V) == 0);
		vt100_free(V);
		tap_done("vt100_drain_scrolled empties the queue");
	}

	/* Acceptance: a 100-line printf stream produces a 77-row
	 * scrolled-off queue + 23-row visible content + 1 blank trailing
	 * row on a 24-row grid (the first 23 lines fill rows 0..22 with
	 * no scroll; lines i=23..99 each scroll on their trailing \n). */
	{
		struct vt100 *V = vt100_new(24, 80);
		char line[16];
		const struct vt100_cell *r;

		for (int i = 0; i < 100; i++) {
			int n = snprintf(line, sizeof(line), "hello %d\r\n", i);
			vt100_input(V, line, (size_t)n);
		}

		tap_check(vt100_scrolled_count(V) == 77);

		int cols;
		r = vt100_scrolled_row(V, 0, &cols);
		tap_check(cols == 80);
		tap_check(r[0].cp == 'h');
		tap_check(r[6].cp == '0');
		tap_check(r[7].cp == 0);

		r = vt100_scrolled_row(V, 76, NULL);
		tap_check(r[0].cp == 'h');
		tap_check(r[6].cp == '7');
		tap_check(r[7].cp == '6');
		tap_check(r[8].cp == 0);

		tap_check(vt100_cell(V, 0, 0)->cp == 'h');
		tap_check(vt100_cell(V, 0, 6)->cp == '7');
		tap_check(vt100_cell(V, 0, 7)->cp == '7');

		tap_check(vt100_cell(V, 22, 0)->cp == 'h');
		tap_check(vt100_cell(V, 22, 6)->cp == '9');
		tap_check(vt100_cell(V, 22, 7)->cp == '9');

		tap_check(vt100_cell(V, 23, 0)->cp == 0);
		tap_check(vt100_cursor_row(V) == 23);
		tap_check(vt100_cursor_col(V) == 0);

		vt100_drain_scrolled(V);
		tap_check(vt100_scrolled_count(V) == 0);
		vt100_free(V);
		tap_done(
		    "100-line stream → 77 scrolled + 23 visible + 1 blank");
	}

	/* ------------------------------------------------------------ */
	/*  Phase 4: DSR synth + reply drain                            */
	/* ------------------------------------------------------------ */

	/* DSR-status (\x1b[5n) returns "\x1b[0n" -- terminal OK. */
	{
		struct vt100 *V = vt100_new(24, 80);
		struct sbuf *r = vt100_reply(V);

		feed(V, "\x1b[5n");
		tap_check(r->len == 4);
		tap_check(memcmp(r->buf, "\x1b[0n", 4) == 0);
		vt100_free(V);
		tap_done("DSR 5 returns terminal-OK reply");
	}

	/* DSR-CPR (\x1b[6n) at home returns 1-based cursor position. */
	{
		struct vt100 *V = vt100_new(24, 80);
		struct sbuf *r = vt100_reply(V);

		feed(V, "\x1b[6n");
		tap_check(r->len == 6);
		tap_check(memcmp(r->buf, "\x1b[1;1R", 6) == 0);
		vt100_free(V);
		tap_done("DSR 6 at home returns \\x1b[1;1R");
	}

	/* Plan test: \x1b[999C clamps to col 79; CPR then reports
	 * 1-based col 80. */
	{
		struct vt100 *V = vt100_new(24, 80);
		struct sbuf *r = vt100_reply(V);

		feed(V, "\x1b[999C\x1b[6n");
		tap_check(vt100_cursor_col(V) == 79);
		tap_check(r->len == 7);
		tap_check(memcmp(r->buf, "\x1b[1;80R", 7) == 0);
		vt100_free(V);
		tap_done("CUF 999 clamps; CPR reports clamped col");
	}

	/* DSR-CPR mid-screen reflects the live cursor position. */
	{
		struct vt100 *V = vt100_new(24, 80);
		struct sbuf *r = vt100_reply(V);

		feed(V, "\x1b[12;34H\x1b[6n");
		tap_check(r->len == 8);
		tap_check(memcmp(r->buf, "\x1b[12;34R", 8) == 0);
		vt100_free(V);
		tap_done("DSR 6 reports the live cursor position");
	}

	/* Linenoise getColumns probe sequence: query, clamp, query,
	 * restore.  Verify each DSR reply matches what linenoise reads. */
	{
		struct vt100 *V = vt100_new(24, 80);
		struct sbuf *r = vt100_reply(V);

		/* Step 1: query initial cursor at (0,0). */
		feed(V, "\x1b[6n");
		tap_check(r->len == 6 && memcmp(r->buf, "\x1b[1;1R", 6) == 0);
		sbuf_reset(r);

		/* Step 2: move right (clamps), query the rightmost col. */
		feed(V, "\x1b[999C\x1b[6n");
		tap_check(r->len == 7 && memcmp(r->buf, "\x1b[1;80R", 7) == 0);
		sbuf_reset(r);

		/* Step 3: restore cursor (CR + CUF n).  No reply. */
		feed(V, "\r\x1b[1C");
		tap_check(r->len == 0);
		tap_check(vt100_cursor_col(V) == 1);
		vt100_free(V);
		tap_done(
		    "linenoise getColumns probe yields expected DSR replies");
	}

	/* DSR with a private marker (e.g. \x1b[?6n) is ignored. */
	{
		struct vt100 *V = vt100_new(24, 80);
		struct sbuf *r = vt100_reply(V);

		feed(V, "\x1b[?6n");
		tap_check(r->len == 0);
		vt100_free(V);
		tap_done("private-marker DSR is ignored");
	}

	/* Reply sbuf caps: each \x1b[0n is 4 bytes; 64 fit in 256, the
	 * 65th drops the oldest 4.  After 65 the buffer is still 256 B
	 * and the trailing bytes are still "\x1b[0n". */
	{
		struct vt100 *V = vt100_new(24, 80);
		struct sbuf *r = vt100_reply(V);

		for (int i = 0; i < 64; i++)
			feed(V, "\x1b[5n");
		tap_check(r->len == 256);
		feed(V, "\x1b[5n");
		tap_check(r->len == 256);
		tap_check(memcmp(r->buf + 252, "\x1b[0n", 4) == 0);
		vt100_free(V);
		tap_done("reply sbuf caps and drops oldest on overflow");
	}

	/* ------------------------------------------------------------ */
	/*  Phase 5: serializer + scrollback bridge                     */
	/* ------------------------------------------------------------ */

	/* Serializer: a plain row trims trailing blanks and emits no SGR. */
	{
		struct vt100_cell row[8];

		memset(row, 0, sizeof(row));
		row[0].cp = 'h';
		row[1].cp = 'i';
		for (int i = 0; i < 8; i++) {
			row[i].sgr.fg = VT100_DEFAULT_COLOR;
			row[i].sgr.bg = VT100_DEFAULT_COLOR;
		}

		struct sbuf out = SBUF_INIT;
		vt100_serialize_row(&out, row, 8);
		tap_check(strcmp(out.buf, "hi") == 0);
		sbuf_release(&out);
		tap_done("serializer trims trailing blanks; no SGR for default "
			 "cells");
	}

	/* Serializer: an embedded blank cell becomes a literal space. */
	{
		struct vt100_cell row[5];

		memset(row, 0, sizeof(row));
		row[0].cp = 'a';
		row[1].cp = 0;
		row[2].cp = 'b';
		for (int i = 0; i < 5; i++) {
			row[i].sgr.fg = VT100_DEFAULT_COLOR;
			row[i].sgr.bg = VT100_DEFAULT_COLOR;
		}

		struct sbuf out = SBUF_INIT;
		vt100_serialize_row(&out, row, 5);
		tap_check(strcmp(out.buf, "a b") == 0);
		sbuf_release(&out);
		tap_done("serializer renders embedded blank cells as space");
	}

	/* Serializer: an SGR run emits a transition + trailing reset. */
	{
		struct vt100_cell row[7];

		memset(row, 0, sizeof(row));
		for (int i = 0; i < 7; i++) {
			row[i].cp = (uint32_t)('a' + i);
			row[i].sgr.fg = VT100_DEFAULT_COLOR;
			row[i].sgr.bg = VT100_DEFAULT_COLOR;
		}
		/* Cells 2..4 red. */
		for (int i = 2; i < 5; i++)
			row[i].sgr.fg = 1;

		struct sbuf out = SBUF_INIT;
		vt100_serialize_row(&out, row, 7);
		tap_check(strcmp(out.buf, "ab\x1b[0;31mcde\x1b[0mfg") == 0);
		sbuf_release(&out);
		tap_done("serializer emits SGR transitions on run boundaries");
	}

	/* Serializer: bold + underline + reverse + bright bg compose. */
	{
		struct vt100_cell row[1] = {
		    {.cp = 'X',
		     .sgr = {.fg = 11,
			     .bg = 12,
			     .attrs = VT100_ATTR_BOLD | VT100_ATTR_REVERSE,
			     ._pad = 0}}};
		struct sbuf out = SBUF_INIT;

		vt100_serialize_row(&out, row, 1);
		/* fg 11 = bright (8 + 3) → 93; bg 12 = bright (8 + 4) → 104. */
		tap_check(strcmp(out.buf, "\x1b[0;1;7;93;104mX\x1b[0m") == 0);
		sbuf_release(&out);
		tap_done("serializer composes attrs with bright fg/bg");
	}

	/* Acceptance: 100 lines of "hello %d\\r\\n" on a 24×80 grid; the
	 * serialized scrolled rows + serialized visible content rows
	 * reproduce exactly the line stream a \\n-split would yield. */
	{
		struct vt100 *V = vt100_new(24, 80);
		char line[16];

		for (int i = 0; i < 100; i++) {
			int n = snprintf(line, sizeof(line), "hello %d\r\n", i);
			vt100_input(V, line, (size_t)n);
		}
		tap_check(vt100_scrolled_count(V) == 77);

		struct sbuf row = SBUF_INIT;

		/* Indexes 0..76 come from the scrolled-off queue. */
		for (int i = 0; i < 77; i++) {
			int cols;
			const struct vt100_cell *cells =
			    vt100_scrolled_row(V, i, &cols);
			char want[16];

			snprintf(want, sizeof(want), "hello %d", i);
			sbuf_reset(&row);
			vt100_serialize_row(&row, cells, cols);
			tap_check(strcmp(row.buf, want) == 0);
		}

		/* Indexes 77..99 come from visible rows 0..22. */
		struct vt100_cell tmp[80];
		for (int r = 0; r < 23; r++) {
			for (int co = 0; co < 80; co++)
				tmp[co] = *vt100_cell(V, r, co);
			char want[16];

			snprintf(want, sizeof(want), "hello %d", 77 + r);
			sbuf_reset(&row);
			vt100_serialize_row(&row, tmp, 80);
			tap_check(strcmp(row.buf, want) == 0);
		}

		/* Visible row 23 (parking row) is blank. */
		for (int co = 0; co < 80; co++)
			tmp[co] = *vt100_cell(V, 23, co);
		sbuf_reset(&row);
		vt100_serialize_row(&row, tmp, 80);
		tap_check(row.len == 0);

		sbuf_release(&row);
		vt100_free(V);
		tap_done("100-line printf stream serializes bit-identical to "
			 "\\n-split");
	}

	/* Bridge: tui_log_pull_from_vt100 drains the queue and pushes
	 * each serialized row into the tui_log line ring. */
	{
		struct tui_log L;

		tui_log_init(&L, 256);
		struct vt100 *V = vt100_new(4, 8);

		feed(V, "AAAA\r\nBBBB\r\nCCCC\r\nDDDD\r\nEEEE\r\nFFFF\r\n");
		/* 6 lines on a 4-row grid: 3 lines fill rows 0..2 with
		 * no scroll; lines i=3..5 each scroll, so 3 rows in the
		 * scrolled-off queue. */
		tap_check(vt100_scrolled_count(V) == 3);
		tui_log_pull_from_vt100(&L, V);

		tap_check(vt100_scrolled_count(V) == 0);
		tap_check(L.n_lines == 3);
		tap_check(strcmp(L.lines[0], "AAAA") == 0);
		tap_check(strcmp(L.lines[1], "BBBB") == 0);
		tap_check(strcmp(L.lines[2], "CCCC") == 0);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("tui_log_pull_from_vt100 drains queue into line ring");
	}

	/* Bridge: pulling on an empty queue is a no-op. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		struct vt100 *V = vt100_new(4, 8);

		tui_log_pull_from_vt100(&L, V); /* nothing to drain */
		tap_check(L.n_lines == 0);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("tui_log_pull_from_vt100 is a no-op on empty queue");
	}

	/* ------------------------------------------------------------ */
	/*  Phase 6: renderer integration with attached vt100           */
	/* ------------------------------------------------------------ */

	/* Without a grid attached, tui_log_render still works as before. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);
		tui_log_append(&L, "hello\n", 6);

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(strstr(out.buf, "hello") != NULL);
		/* No grid attached → cursor stays hidden, no \x1b[?25h. */
		tap_check(strstr(out.buf, "\x1b[?25h") == NULL);
		sbuf_release(&out);
		tui_log_release(&L);
		tap_done(
		    "tui_log_render works unchanged when no grid attached");
	}

	/* With a grid attached and tailing, the live grid is rendered at
	 * the bottom of the body and the cursor is shown at the grid's
	 * coordinates. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);
		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);

		feed(V, "hello world");
		tap_check(vt100_cursor_row(V) == 0);
		tap_check(vt100_cursor_col(V) == 11);

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(strstr(out.buf, "hello world") != NULL);
		tap_check(strstr(out.buf, "\x1b[?25h") != NULL);
		/* Cursor positioned at body_bottom - grid_rows + 1 + 0 = 7;
		 * column = cur_col + 1 = 12. */
		tap_check(strstr(out.buf, "\x1b[7;12H\x1b[?25h") != NULL);
		sbuf_release(&out);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("attached grid renders below ring; cursor positioned");
	}

	/* When the chip hides the cursor (DECSET ?25l), the renderer
	 * leaves it hidden -- no "\x1b[?25h" anywhere in the frame. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);
		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);

		feed(V, "hello\x1b[?25l");
		tap_check(!vt100_cursor_visible(V));

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(strstr(out.buf, "hello") != NULL);
		tap_check(strstr(out.buf, "\x1b[?25h") == NULL);
		sbuf_release(&out);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("DECSET ?25l keeps the rendered cursor hidden");
	}

	/* When scrolled (anchor >= 0), the grid is hidden -- the body is
	 * fully ring-only so the user can navigate scrollback without the
	 * bottom of the frame being pinned to a moving target. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);
		for (int i = 0; i < 20; i++) {
			char line[16];
			int n = snprintf(line, sizeof(line), "line %d\n", i);
			tui_log_append(&L, line, (size_t)n);
		}
		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);
		feed(V, "GRID_TEXT");

		/* Tailing: GRID_TEXT visible. */
		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(strstr(out.buf, "GRID_TEXT") != NULL);
		sbuf_release(&out);

		/* Scroll up by sending TK_HOME via a synthetic event. */
		struct term_event ev = {.key = TK_HOME};
		tui_log_on_event(&L, &ev);
		tap_check(!tui_log_is_tailing(&L));

		out = (struct sbuf)SBUF_INIT;
		tui_log_render(&out, &L);
		/* Grid hidden; no GRID_TEXT in the frame; no cursor show. */
		tap_check(strstr(out.buf, "GRID_TEXT") == NULL);
		tap_check(strstr(out.buf, "\x1b[?25h") == NULL);
		sbuf_release(&out);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("scrolled mode hides the grid and its cursor");
	}

	/* Detaching the grid (set NULL) restores no-grid behavior. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);
		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);
		feed(V, "GRID");
		tui_log_set_grid(&L, NULL);

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(strstr(out.buf, "GRID") == NULL);
		tap_check(strstr(out.buf, "\x1b[?25h") == NULL);
		sbuf_release(&out);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("tui_log_set_grid(L, NULL) detaches the grid");
	}

	/* Phase 7 follow-on regressions: grid is now part of the
	 * addressable line space, so decorator + search + navigation
	 * all reach it. */

	/* Decorator runs on grid rows -- the live grid feeds through the
	 * same render pipeline as ring entries, so chip output gets
	 * keyword-colored just like scrollback. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);
		tui_log_set_decorator(&L, test_decorator, NULL);

		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);
		feed(V, "GRID_HELLO");

		decorator_calls = 0;
		decorator_grid_calls = 0;

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(decorator_calls >= 1);
		tap_check(decorator_grid_calls >= 1);
		sbuf_release(&out);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("decorator runs on grid rows");
	}

	/* Search hits matches in the live grid (no freeze). */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);

		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);
		feed(V, "ERROR foo\r\nbar\r\nERROR baz");

		tap_check(tui_log_search_set(&L, "ERROR") == 0);
		tap_check(tui_log_search_total(&L) == 2);
		tui_log_search_clear(&L);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("search hits matches in the live grid");
	}

	/* Inspect (freeze) captures a stable snapshot; search hits both
	 * ring lines and snapshot rows. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);
		tui_log_append(&L, "ERROR ring-line\n", 16);

		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);
		feed(V, "ERROR grid-line");

		tui_log_freeze(&L);
		tap_check(tui_log_is_frozen(&L));

		tap_check(tui_log_search_set(&L, "ERROR") == 0);
		tap_check(tui_log_search_total(&L) == 2);

		/*
		 * Mutate the live grid -- the snapshot should not move,
		 * so the count stays at 2.
		 */
		feed(V, "\r\nERROR new-line");
		tap_check(tui_log_search_total(&L) == 2);

		tui_log_search_clear(&L);
		tui_log_unfreeze(&L);
		vt100_free(V);
		tui_log_release(&L);
		tap_done("inspect snapshot is stable; search counts ring + "
			 "snapshot");
	}

	/* TK_UP / TK_DOWN navigation walks the unified addressable
	 * space -- the user can scroll back through grid rows before
	 * reaching ring entries. */
	{
		struct tui_log L;

		tui_log_init(&L, 32);
		tui_log_resize(&L, 40, 10);

		for (int i = 0; i < 5; i++) {
			char line[16];
			int n = snprintf(line, sizeof(line), "RING_%d\n", i);
			tui_log_append(&L, line, (size_t)n);
		}

		struct vt100 *V = vt100_new(4, 20);
		tui_log_set_grid(&L, V);
		feed(V, "GRID_A\r\nGRID_B\r\nGRID_C\r\nGRID_D");

		/*
		 * Tail mode: bottom shows the last grid row.  TK_UP
		 * moves the anchor back through the grid; one step lands
		 * on "GRID_C" (third grid row, second from the bottom).
		 */
		struct term_event ev = {.key = TK_UP};
		tui_log_on_event(&L, &ev);
		tap_check(!tui_log_is_tailing(&L));

		struct sbuf out = SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(strstr(out.buf, "GRID_C") != NULL);
		sbuf_release(&out);

		/* Walk further up; eventually reach the ring entries. */
		for (int k = 0; k < 8; k++)
			tui_log_on_event(&L, &ev);

		out = (struct sbuf)SBUF_INIT;
		tui_log_render(&out, &L);
		tap_check(strstr(out.buf, "RING_") != NULL);
		sbuf_release(&out);

		vt100_free(V);
		tui_log_release(&L);
		tap_done("TK_UP navigation crosses grid → ring");
	}

	return tap_result();
}
