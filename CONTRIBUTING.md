# Contributing to esp-ice

Contributions are welcome.  This file covers how to set up a
development environment, the invariants that govern where code lives,
and the conventions for wiring up commands.  Read it end to end
before making non-trivial changes.

## Development setup

Clone the repository and install the pre-commit hooks:

```bash
git clone https://github.com/fhrbata/esp-ice.git
cd esp-ice
pip install pre-commit
pre-commit install -t pre-commit -t commit-msg
```

The hooks run automatically before each commit and enforce code
formatting and commit-message standards.  Configuration lives in
[`.pre-commit-config.yaml`](.pre-commit-config.yaml).

Building:

```bash
make                    # dev build
make test               # run the TAP test suite
make STATIC=1           # release-style self-contained binary
```

See [README.md](README.md) for cross-compilation, build variables,
and other targets.

## Code standards

- Follow the existing C style (enforced by `clang-format`).
- Commits follow [Conventional Commits](https://www.conventionalcommits.org/).
- Run `make test` before submitting a pull request.
- Lint locally with `make clang-format` and `make clang-tidy`.

## Submitting changes

1. Create a feature branch.
2. Make your changes; ensure pre-commit checks pass.
3. Submit a pull request with a clear description of the *why*.

## Repository layout

```
esp-ice/
├── <root>/           generic code and helpers (sbuf, svec, csv, json,
│                     elf, map, http, reader, partition_table, ...)
├── cmd/              command implementations; one directory per
│                     `ice <command>` (see below)
├── platform/         OS-conditional code (POSIX and Windows)
│   ├── posix/
│   └── win/
├── platform.h        platform abstraction API consumed by root + cmd
├── vendor/           third-party source, always built from source
├── deps/             link-time libraries, only needed for STATIC=1
├── t/                cross-cutting TAP tests for root-level modules
├── docs/             design notes and planning documents
└── toolchains/       cross-compile toolchains (fetched on demand)
```

Two rules make navigating the tree predictable:

1. **Root is for generic code.**  Library-style modules (parsers,
   containers, I/O primitives, string/path helpers) live at the
   project root and are linked into `libice.a`.  They know nothing
   about specific `ice <command>` semantics.
2. **Command logic lives under `cmd/<path>/`.**  Anything that is the
   implementation of a user-facing `ice <command>` belongs in the
   directory that matches the command path; it consumes root helpers,
   not the other way around.

If you find yourself adding command-specific branches to a root
module, the module's abstraction is wrong — push the specialisation
into the caller under `cmd/<path>/` and keep the root module generic.

## Platform abstraction

All OS-conditional code lives in [`platform/`](platform/) behind the
API declared in [`platform.h`](platform.h).  The rule, quoted from
`platform.h` itself:

> If an implementation requires `#ifdef _WIN32`, it belongs here;
> if it's ordinary C that only calls into the primitives below, it
> doesn't.

Concretely:

- **No `#ifdef _WIN32` / `#ifdef __linux__` outside `platform/`.**
  Platform-varying structs use opaque pointers; the layout lives in
  the matching `platform/<os>/` translation unit.
- **No raw POSIX syscalls outside `platform/`.**  Root and `cmd/`
  code uses `platform.h` primitives (`process_run`, `dir_foreach`,
  `rename_w`, UTF-8 path helpers, …), not `select`, `fcntl`,
  `waitpid`, `readlink`, `popen`, etc.
- **If a primitive you need is missing, extend `platform.h` first.**
  Add the signature, implement in both `platform/posix/` and
  `platform/win/`, then use it.  If the design tempts you to reach
  for `#ifdef` in a caller, re-shape the abstraction instead.

The portable helpers built on top of the platform primitives
(`fs_mkdirp`, `rmtree`, `write_file_atomic`, CSV readers, …) live at
the root and have no `#ifdef`s.  Keep it that way.

### Prefer POSIX names over new `ice_*` APIs

When a POSIX function exists for what you need, extend it to Windows
via a `foo_w()` shim (UTF-8 → `wchar_t` via `mbs_to_wcs`, then call
the `_w`-suffixed Win32 CRT function) and `#define foo foo_w` in
`platform.h`.  Call sites then read as plain POSIX C on both sides.
The canonical shims are `fopen_w`, `access_w`, `mkdir_w`, `open_w`,
`unlink_w`, `rmdir_w`, `rename_w`, `chdir_w`, `chmod_w`, `getcwd_w`,
`symlink_w`, `link_w`; follow this pattern.

Reach for a new ice-named helper (`process_start`, `dir_foreach`,
`pipe_read_timed`, …) only when no POSIX function fits at all.  If
you find yourself inventing `ice_path_*` or `ice_console_*` to hide
a single `#ifdef` branch, you almost certainly want a `_w` shim
instead.

### UTF-8 path invariant

**Every path-accepting POSIX call on Windows MUST route through a
`_w` shim.**  Without it, non-ASCII paths are silently mangled via
the system ANSI code page.  Before adding a call to a POSIX function
that takes a path argument (`stat`, `truncate`, `utime`, `freopen`,
…), check that `platform.h` already maps the bare name to a UTF-8
shim; if not, add the shim before the call site.

Equivalently: raw `chdir(path)`, `chmod(path, …)`, `remove(path)`,
`getcwd(buf, n)`, etc. in root or `cmd/` code is a bug even if the
CI build passes — the bug only manifests when a user has a non-ASCII
character in a path on Windows.

### Before-submitting smell tests

Run these greps against your changes:

- `#ifdef _WIN32` / `#ifndef _WIN32` outside `platform/` → push into
  a shim, or inline an unconditional version that is a no-op on the
  platform where the check is irrelevant.
- `#include <dirent.h>` / `<sys/select.h>` / `<sys/wait.h>` /
  `<termios.h>` outside `platform/` → `platform.h` should provide
  the primitive instead.
- Forward declarations of POSIX functions in a `.c` file
  (`int fileno(FILE *);`, `int dup2(int, int);`, …) → move the
  declaration to `platform.h` so every caller picks it up.
- `popen` / `pclose` → use `struct process` with `pipe_in` /
  `use_shell`.
- Raw `chdir`, `chmod`, `getcwd`, `remove` → either the file is
  missing `#include "ice.h"` / `#include "platform.h"` (so the
  shim macros aren't visible), or the shim is missing and should
  be added.

## `cmd/` layout

Every `ice <command>` — leaf or namespace — has exactly one directory
under `cmd/`, and the on-disk path mirrors the CLI path: `ice a b c`
lives at `cmd/a/b/c/c.c`.  `ice_subs[]` in [`ice.c`](ice.c) lists the
top-level descriptors; a single `ice_dispatch()` walks the tree,
matching `argv[0]` at each level, similar to how git dispatches
`commit`, `log`, etc.

```
cmd/
├── build/  clean/  flash/  init/          ← porcelain:  project-aware wrappers
├── menuconfig/  status/                       (read .ice/, derive args)
├── completion/  config/  help/             ← framework:  ice itself
├── idf/                                    ← namespace:  bundled ESP-IDF host tools
│   ├── idf.c                                   (dispatcher + manual)
│   ├── configdep/configdep.c                   (`ice idf configdep`)
│   ├── ldgen/ldgen.c                           (`ice idf ldgen`)
│   ├── partition-table/partition-table.c       (`ice idf partition-table`)
│   └── size/size.c                             (`ice idf size`)
├── image/                                  ← namespace:  host-only image ops
│   ├── image.c
│   ├── create/create.c                         (`ice image create`)
│   ├── info/info.c                             (`ice image info`)
│   └── merge/merge.c                           (`ice image merge`)
├── repo/                                   ← namespace:  ESP-IDF checkouts
├── target/                                 ← namespace:  chip-bound ops (esptool)
│   ├── target.c                                (dispatcher + chip tables)
│   ├── list/list.c                             (`ice target list`)
│   └── monitor/monitor.c                       (`ice target monitor`)
└── tools/                                  ← namespace:  bundled toolchains
    ├── tools.c
    ├── install/install.c                       (`ice tools install`)
    ├── list/list.c                             (`ice tools list`)
    └── info/info.c                             (`ice tools info`)
```

Two tiers, matched to intent:

- **Top-level** (`cmd/<name>/`) holds two kinds of commands:
  - *Porcelain* — project-aware wrappers that read `.ice/` and call
    into plumbing with arguments derived from the active profile
    (`ice build`, `ice flash`).
  - *Framework* — ice itself (`ice config`, `ice help`,
    `ice completion`).
- **Namespaces** (`cmd/<ns>/<sub>/`) hold *plumbing* — commands that
  take their inputs explicitly on the command line and do not
  auto-discover from a profile.  The namespace's own `.manual`
  (in `cmd/<ns>/<ns>.c`) states the policy for what belongs there;
  when in doubt, read it.

Same CLI verb at both tiers is fine — `ice flash` (porcelain) vs
`ice target flash` (plumbing) — because the path distinguishes them.

Every subcommand, even a one-screen leaf, lives in its own
subdirectory (`cmd/image/create/create.c`, not `cmd/image/create.c`;
`cmd/target/list/list.c`, not inlined in `target.c`).  The path is
always `cmd/<ns>/<sub>/<sub>.c`.  Per-command helper files go in the
same subdirectory as the entry point (`cmd/idf/size/chip.[ch]` is an
existing example), leaving room to add tests and supplementary code
without touching sibling commands.

If a set of helpers is shared by multiple subcommands in a namespace,
put their declarations in `cmd/<ns>/<ns>.h` and the implementation in
`cmd/<ns>/<ns>.c`; subcommands include them via
`#include "cmd/<ns>/<ns>.h"` (the project root is on the include
path, so full-path quoted includes are the idiom).  `cmd/repo/repo.h`
is the canonical example: it exposes `repo_run_git`, path helpers,
and locking primitives to every `cmd/repo/<sub>/<sub>.c`.

Per-command wiring:

- A single entry point `int cmd_<path>(int argc, const char **argv)`.
- A per-command `static const struct cmd_manual <path>_manual` powers
  `--help` and `ice help`.
- A `static const struct option cmd_<path>_opts[]` drives option
  parsing, `-h` usage, `--help` synopsis, and `--ice-complete` shell
  completion — all from one declaration.
- A file-scope `const struct cmd_desc cmd_<path>_desc` ties the three
  together.
- `<path>` is the CLI path joined with underscores: `cmd_build_desc`
  for a top-level command, `cmd_image_create_desc` for a namespace
  subcommand.  The descriptor's `.name` stores only the final CLI
  token (`"create"`), so the dispatcher matches `argv[0]` against it
  directly.

The wiring is fully explicit: no build-time scan, no constructor
magic, no code generation.

## Adding a top-level command

The walkthrough below adds an artificial `ice greet` subcommand to
demonstrate every piece of the wiring.

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
  Add one `OPT_POSITIONAL` per slot for multi-positional commands.

### 2. Declare the descriptor in `ice.h`

Add the extern next to the other top-level descriptors so `ice.c`
can reference it:

```c
extern const struct cmd_desc cmd_greet_desc;
```

The option table and manual stay `static` to `greet.c`.  The
`cmd_greet()` function only needs a local forward declaration
before the descriptor initializer (see the image/create pattern)
unless it is called from another translation unit.

### 3. Register in `ice_subs[]` (in `ice.c`)

Add the descriptor pointer in alphabetical order:

```c
&cmd_greet_desc,
```

### 4. Add to the Makefile

Extend `LIB_SRCS` in alphabetical order within its tier:

```makefile
cmd/greet/greet.c \
```

## Adding a namespace subcommand

The differences from a top-level command:

- Path is `cmd/<ns>/<name>/<name>.c`.
- Symbols use the full CLI path: `cmd_<ns>_<name>_desc`,
  `<ns>_<name>_manual`, `cmd_<ns>_<name>_opts`, and
  `int cmd_<ns>_<name>(int, const char **)`.
- The descriptor is **not** declared in `ice.h`.  Instead, the
  namespace dispatcher (`cmd/<ns>/<ns>.c`) declares it `extern`
  locally and lists it in its `<ns>_subs[]` array.
- Tier in the Makefile's `LIB_SRCS` is `cmd/<ns>/<name>/<name>.c`.

Example: adding `ice image verify` would create
`cmd/image/verify/verify.c` with `cmd_image_verify_desc`, then add

```c
extern const struct cmd_desc cmd_image_verify_desc;
```

to `cmd/image/image.c` and include `&cmd_image_verify_desc` in
`image_subs[]`.  Nothing in `ice.h` or `ice.c` changes.

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

If `ice foo` is not a builtin and not an alias, ice searches `PATH`
for an executable named `ice-foo` and runs it, passing all remaining
arguments.  This allows third-party extensions without modifying the
ice binary.

## Testing

Tests live alongside the command they exercise, in
`cmd/<path>/t/` — top-level (`cmd/<name>/t/`) and namespace
subcommand (`cmd/<ns>/<name>/t/`) are both discovered.  `make test`
walks `cmd/*/t/` and `cmd/*/*/t/` (plus the top-level [`t/`](t/) for
cross-cutting tests) through `prove`; each `.t` file is an executable
that prints [TAP](https://testanything.org/).

Shared helpers live at the project root:

- [`t/tap.sh`](t/tap.sh) — bash: `tap_setup`, `tap_check`, `tap_done`,
  `tap_result`.
- [`t/tap.h`](t/tap.h) — C: same surface as macros, for unit tests
  that compile against project sources.

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

Remember to `chmod +x` the `.t` file — `prove` runs executables
directly.  `tap_setup` creates a scratch directory at
`$T_OUT/$(basename $0 .t)` and `cd`s into it, so tests don't pollute
the working tree.

Run a subset via the `T` variable:

```bash
make test                                        # every cmd/*/t/ + t/
make test T=cmd/greet/t                          # one command's tests
make test T=cmd/greet/t/0001-basic.t             # a single file
```

[`cmd/completion/t/0001-completion.t`](cmd/completion/t/0001-completion.t)
is a real end-to-end example exercising `ice completion <shell>` and
the hidden `__complete` backend.

C unit tests for the utility modules in the project root (sbuf, svec,
json, cmakecache, …) live in the top-level [`t/`](t/) instead of
under `cmd/<name>/t/`, since they are not tied to any subcommand.
Each is a small `.t` shell wrapper that compiles a sibling `test_*.c`
file linked against the relevant project sources, then runs the
resulting binary; see [`t/0001-sbuf.t`](t/0001-sbuf.t) and
[`t/test_sbuf.c`](t/test_sbuf.c) for the canonical pair.

## Templates to copy

- [`cmd/build/build.c`](cmd/build/build.c) /
  [`cmd/flash/flash.c`](cmd/flash/flash.c) — minimal porcelain
  one-liner around `run_cmake_target()`.
- [`cmd/clean/clean.c`](cmd/clean/clean.c) — smallest standalone
  command, no options.
- [`cmd/config/config.c`](cmd/config/config.c) — multiple boolean
  flags plus positional arguments with completion callback.
- [`cmd/image/image.c`](cmd/image/image.c) — namespace dispatcher
  wiring in subcommands that live in their own subdirectories
  (`create/`, `info/`, `merge/`).
- [`cmd/repo/repo.h`](cmd/repo/repo.h) /
  [`cmd/repo/repo.c`](cmd/repo/repo.c) — namespace-shared helpers
  (git primitives, path helpers, locking) consumed by every
  `cmd/repo/<sub>/<sub>.c`.
- [`cmd/idf/size/size.c`](cmd/idf/size/size.c) — namespace subcommand
  with string-valued options (`--target`, `--format`) and a
  positional file argument.
