# cmd/

Each subdirectory here is one subcommand of the top-level `ice` binary.
The dispatch table in [`../ice.c`](../ice.c) maps `ice <name>` to the
handler in `cmd/<name>/<name>.c`, similar to how `git.c` dispatches
`commit`, `log`, etc.

## Layout

- One subdirectory per subcommand: `cmd/<name>/<name>.c`.
- A single entry point `int cmd_<name>(int argc, const char **argv)`.
- A per-command `static const struct cmd_manual manual` powers
  `ice <name> --help` and `ice help <name>`.
- Options (if any) live in a file-scope `const struct option
  cmd_<name>_opts[]` so the completion backend can walk them.

The wiring is fully explicit: there is no build-time scan, constructor
magic, or code generation.  Adding a command touches exactly four
places.

## Adding a new command

The full walkthrough below adds an artificial `ice greet` subcommand
to demonstrate every piece of the wiring.

### 1. Implement the handler

Create `cmd/greet/greet.c`:

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ice.h"

static const struct cmd_manual manual = {
	.description =
	H_PARA("Prints a friendly greeting for @b{<name>}.  Use "
	       "@b{--loud} to SHOUT."),

	.examples =
	H_EXAMPLE("ice greet world")
	H_EXAMPLE("ice greet --loud world"),
};

/* File scope so the completion backend can see the option table. */
static int opt_loud;

const struct option cmd_greet_opts[] = {
	OPT_BOOL(0, "loud", &opt_loud, "shout the greeting"),
	OPT_END(),
};

int cmd_greet(int argc, const char **argv)
{
	const char *usage[] = {"ice greet [--loud] <name>", NULL};
	const char *name;

	argc = parse_options_manual(argc, argv, cmd_greet_opts, usage,
				    &manual);
	if (argc != 1)
		die("usage: ice greet [--loud] <name>");

	name = argv[0];
	if (opt_loud)
		printf("HELLO, %s!\n", name);
	else
		printf("Hello, %s.\n", name);
	return 0;
}
```

### 2. Declare the handler in `ice.h`

Add the prototype next to the other `cmd_*` declarations, and -- since
this command has options -- the extern for the option table:

```c
int cmd_greet(int argc, const char **argv);

extern const struct option cmd_greet_opts[];
```

### 3. Register in `ice_commands[]` (in `ice.c`)

Add an entry in alphabetical order:

```c
{.name = "greet",       .fn = cmd_greet,
 .summary = "print a greeting",
 .opts = cmd_greet_opts},
```

Omit `.opts` for commands with no options.

### 4. Add to the Makefile

Extend the `SRCS` list alphabetically:

```makefile
cmd/greet/greet.c \
```

### Try it out

```console
$ make
$ ./build/<triple>/ice greet world
Hello, world.
$ ./build/<triple>/ice greet --loud world
HELLO, WORLD!
$ ./build/<triple>/ice greet --<TAB>
--loud
$ ./build/<triple>/ice help greet
NAME
    ice-greet - print a greeting
...
```

The command now:

- appears in `ice --help` and the `ice <TAB>` completion list,
- has a manual page via `ice help greet` / `ice greet --help`,
- auto-completes its `--loud` flag from the shared completion backend,
- follows the same conventions as every other command.

## Optional extras

### Positional completion

File arguments fall through to the shell's default file completion,
which is usually what you want.  For a command with a positional drawn
from a known set (like `ice set-target <chip>`), add one branch in
[`completion/completion.c`](completion/completion.c):

```c
else if (!strcmp(sub, "greet"))
	complete_greet_arg(cur);
```

### Hidden commands

Set `.hidden = 1` in the `ice_commands[]` entry to keep the command
dispatchable but out of `ice --help` and `ice <TAB>` listings.  Used
today for the `__complete` backend; handy for other internal helpers.

## Testing

Tests live alongside the command they exercise, in `cmd/<name>/t/`.
`make test` walks `cmd/*/t/` (plus the top-level `t/` for cross-cutting
tests) through `prove`; each `.t` file is an executable that prints
[TAP](https://testanything.org/).

Shared helpers live at the project root:

- [`../t/tap.sh`](../t/tap.sh) -- bash: `tap_setup`, `tap_check`,
  `tap_done`, `tap_result`.
- [`../t/tap.h`](../t/tap.h) -- C: same surface as macros, for unit
  tests that compile against project sources.

A minimal bash test (`cmd/greet/t/0001-basic.t`):

```bash
#!/usr/bin/env bash
. t/tap.sh
tap_setup

"$BINARY" greet world >out
tap_check grep -qx 'Hello, world.' out
tap_done "default greeting"

"$BINARY" greet --loud world >out
tap_check grep -qx 'HELLO, WORLD!' out
tap_done "--loud uppercases"

tap_result
```

Remember to `chmod +x` the `.t` file -- `prove` runs executables
directly.  `tap_setup` creates a scratch directory at
`$T_OUT/$(basename $0 .t)` and `cd`s into it, so tests don't pollute
the working tree.

Run a subset via the `T` variable:

```bash
make test                                        # every cmd/*/t/ + t/
make test T=cmd/greet/t                          # one command's tests
make test T=cmd/greet/t/0001-basic.t             # a single file
```

[`completion/t/0001-completion.t`](completion/t/0001-completion.t) is
a real end-to-end example exercising `ice completion <shell>` and the
hidden `__complete` backend.

## Templates to copy

- [`build/build.c`](build/build.c) / [`flash/flash.c`](flash/flash.c) --
  minimal one-liner around `run_cmake_target()`.
- [`clean/clean.c`](clean/clean.c) -- smallest standalone command, no
  options.
- [`config/config.c`](config/config.c) -- multiple boolean flags plus
  positional arguments; shows the file-scope option-table pattern.
- [`size/size.c`](size/size.c) -- string-valued options (`--target`,
  `--format`) plus a positional file argument.
- [`set-target/set-target.c`](set-target/set-target.c) -- shared data
  (`ice_supported_targets[]`) exported to the rest of the tree.
