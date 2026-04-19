# cmd/

Each subdirectory here is one subcommand of the top-level `ice` binary.
`ice_subs[]` in [`../ice.c`](../ice.c) lists the top-level descriptor
for every `ice <name>`; dispatch walks that tree via `ice_dispatch()`,
similar to how git dispatches `commit`, `log`, etc.

## Layout

- One subdirectory per subcommand: `cmd/<name>/<name>.c`.
- A single entry point `int cmd_<name>(int argc, const char **argv)`.
- A per-command `static const struct cmd_manual <name>_manual` powers
  `ice <name> --help` and `ice help <name>`.
- A `static const struct option cmd_<name>_opts[]` drives option
  parsing, `-h` usage, `--help` manual synopsis, and `--ice-complete`
  shell completion -- all from one declaration.
- A file-scope `const struct cmd_desc cmd_<name>_desc` ties the three
  together and is referenced from `ice_subs[]`.

The wiring is fully explicit: there is no build-time scan, constructor
magic, or code generation.  Adding a command touches exactly three
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

static const struct cmd_manual greet_manual = {
	.name = "ice greet",

	.description =
	H_PARA("Prints a friendly greeting for @b{<name>}.  Use "
	       "@b{--loud} to SHOUT."),

	.examples =
	H_EXAMPLE("ice greet world")
	H_EXAMPLE("ice greet --loud world"),
};

static int opt_loud;

static const struct option cmd_greet_opts[] = {
	OPT_BOOL(0, "loud", &opt_loud, "shout the greeting"),
	OPT_POSITIONAL("name", NULL),
	OPT_END(),
};

const struct cmd_desc cmd_greet_desc = {
	.name   = "greet",
	.fn     = cmd_greet,
	.opts   = cmd_greet_opts,
	.manual = &greet_manual,
};

int cmd_greet(int argc, const char **argv)
{
	const char *name;

	argc = parse_options(argc, argv, &cmd_greet_desc);
	if (argc != 1)
		die("missing <name> argument");

	name = argv[0];
	if (opt_loud)
		printf("HELLO, %s!\n", name);
	else
		printf("Hello, %s.\n", name);
	return 0;
}
```

Everything is driven by the option table:

- `-h` auto-generates: `usage: greet [<options>] <name>`
- `--help` renders the full manual with OPTIONS auto-listed
- `--ice-complete` dumps `--loud`, `-h`, `--help` for shell TAB
- `OPT_POSITIONAL("name", NULL)` names a positional arg slot in
  the usage line; the second argument is an optional completion
  callback (NULL lets the shell fall through to file completion).
  Add one OPT_POSITIONAL per slot for multi-positional commands.

### 2. Declare the handler and descriptor in `ice.h`

Add the prototypes next to the other `cmd_*` / `cmd_*_desc`
declarations:

```c
int cmd_greet(int argc, const char **argv);
extern const struct cmd_desc cmd_greet_desc;
```

The option table and manual stay `static` to `greet.c`.

### 3. Register in `ice_subs[]` (in `ice.c`)

Add the descriptor pointer in alphabetical order:

```c
&cmd_greet_desc,
```

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
$ ./build/<triple>/ice greet -h
usage: greet [<options>] <name>

        --loud               shout the greeting
$ ./build/<triple>/ice greet --<TAB>
--loud  --help  -h
$ ./build/<triple>/ice help greet
NAME
    ice-greet - print a greeting
...
```

The command now:

- appears in `ice --help` and the `ice <TAB>` completion list,
- has a manual page via `ice help greet` / `ice greet --help`,
- auto-completes its `--loud` flag,
- auto-generates its usage line,
- follows the same conventions as every other command.

## Option table quick reference

```c
/* Plain options -- value goes straight into a C variable */
OPT_BOOL('v', "verbose", &flag, "be noisy"),
OPT_STRING('o', "output", &path, "path", "output file", NULL),
OPT_INT('n', "count", &n, "n", "how many", NULL),
OPT_STRING_LIST('D', "define", &list, "key=val", "repeatable", NULL),

/*
 * _CFG variants -- same C-variable storage, plus defaults seeded
 * from a config key and/or an env var before the CLI flag is parsed.
 * Precedence:  C init  <  config_get(cfg)  <  getenv(env)  <  CLI flag.
 * The optional config_help string is used in the auto-generated
 * CONFIG / ENVIRONMENT manual sections; NULL falls back to help.
 */
OPT_BOOL_CFG('v', "verbose", &flag,
             "core.verbose", NULL,
             "be noisy", NULL),
OPT_STRING_CFG('p', "port", &path, "path",
               "serial.port", "ESPPORT",
               "serial port", "Serial device path.", NULL),
OPT_INT_CFG('b', "baud", &baud, "rate",
            "serial.baud", "ESPBAUD",
            "baud rate", NULL, NULL),
OPT_STRING_LIST_CFG('D', "define", &list, "key=val",
                    "cmake.define", NULL,
                    "repeatable", NULL, NULL),

/* Positional slots (zero or more, in order) */
OPT_POSITIONAL("file", NULL),           /* positional arg name, no callback */
OPT_POSITIONAL("target", complete_fn),  /* positional arg + completion */
OPT_POSITIONAL("[<extra>]", NULL),      /* optional slot (literal brackets) */

/* Terminator */
OPT_END(),
```

The last argument on value-taking options (`OPT_STRING`, `OPT_INT`,
and their `_CFG` variants) is a completion callback for that
option's value, or NULL to let the shell handle it (file completion).

## Positional completion

For commands whose positional argument is drawn from a known set
(like `ice target set <chip>`), add a completion callback and
attach it to an `OPT_POSITIONAL` slot:

```c
static void complete_targets(void)
{
	for (const char *const *t = ice_supported_targets; *t; t++)
		printf("%s\n", *t);
}

static const struct option opts[] = {
	OPT_BOOL(0, "preview", &preview, "allow preview targets"),
	OPT_POSITIONAL("target", complete_targets),
	OPT_END(),
};
```

For multiple positionals, list one `OPT_POSITIONAL` per slot in
order; the parser dispatches to the right callback based on which
positional the cursor is on.

No changes to the completion backend needed -- `parse_options`
handles it automatically via `--ice-complete`.

## Namespace-level extra completion

To inject extra candidates alongside subcommand and flag listings
at a namespace level (e.g. user-defined alias names at the root),
set `cmd_desc.extra_complete` rather than declaring a positional:

```c
const struct cmd_desc ice_root_desc = {
    .name           = "ice",
    .opts           = ice_global_opts,
    .subcommands    = ice_subs,
    .extra_complete = complete_aliases,
};
```

## Hidden commands

Subcommands whose name starts with `_` are dispatchable but hidden
from `ice --help` and `ice <TAB>` listings.  Used today for the
`__complete` backend; handy for other internal helpers.

## External commands

If `ice foo` is not a builtin and not an alias, ice searches PATH
for an executable named `ice-foo` and runs it, passing all remaining
arguments.  This allows third-party extensions without modifying
the ice binary.

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

C unit tests for the utility modules in the project root (sbuf, svec,
json, cmakecache, ...) live in the top-level [`../t/`](../t/) instead
of under `cmd/<name>/t/`, since they are not tied to any subcommand.
Each is a small `.t` shell wrapper that compiles a sibling `test_*.c`
file linked against the relevant project sources, then runs the
resulting binary; see [`../t/0001-sbuf.t`](../t/0001-sbuf.t) and
[`../t/test_sbuf.c`](../t/test_sbuf.c) for the canonical pair.

## Templates to copy

- [`build/build.c`](build/build.c) / [`flash/flash.c`](flash/flash.c) --
  minimal one-liner around `run_cmake_target()`.
- [`clean/clean.c`](clean/clean.c) -- smallest standalone command, no
  options.
- [`config/config.c`](config/config.c) -- multiple boolean flags plus
  positional arguments with completion callback.
- [`target/target.c`](target/target.c) -- namespace command whose
  `cmd_target_desc` lists set/list/info leaves in `target_subs[]`.
- [`size/size.c`](size/size.c) -- string-valued options (`--target`,
  `--format`) plus a positional file argument.
