/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/docs/getting-started/getting-started.c
 * @brief `ice docs getting-started` -- first-run onboarding guide.
 *
 * Help-only leaf: the full guide lives in the manual's
 * @c .description; the @c fn just renders the manual via
 * @c print_manual().  Same output whether the user runs
 * @c ice @c docs @c getting-started or
 * @c ice @c docs @c getting-started @c --help.
 */
#include "ice.h"

int cmd_docs_getting_started(int argc, const char **argv);

/*
 * The full guide prose below concatenates into a single string literal
 * that exceeds C99's 4095-byte minimum string length.  Every compiler
 * we actually target (gcc, clang, mingw-gcc) supports arbitrarily long
 * literals; the warning is @c -pedantic-only.  Suppress it locally so
 * the rest of the TU still gets pedantic checking.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"

/* clang-format off */
static const struct cmd_manual getting_started_manual = {
	.name = "ice docs getting-started",
	.summary = "first-run onboarding guide",

	.description =
	H_PARA("This guide walks through your first project with @b{ice}: "
	       "enabling tab completion, getting an ESP-IDF source tree, "
	       "binding a project to a chip, and building + flashing + "
	       "monitoring the bundled @b{hello_world} example.")

	H_SECTION("BEFORE YOU START")
	H_PARA("You'll need:")
	H_PARA("- An ESP32-family board connected over USB, once you're "
	       "ready to flash.  The serial port shows up as "
	       "@b{/dev/ttyUSB0} (Linux), @b{/dev/cu.usbserial-*} (macOS), "
	       "or @b{COM3} (Windows) -- @b{ice} tries to detect it "
	       "automatically.")
	H_PARA("- Roughly 2 GB of free disk space for the ESP-IDF clone, "
	       "plus another ~500 MB for the toolchain.  Both are "
	       "downloaded once per IDF version and cached under "
	       "@b{~/.ice/}.")
	H_PARA("Tooling that @b{ice} can provide or fall back on:")
	H_ITEM("git",
	       "Only required if you want @b{ice} to fetch and manage "
	       "ESP-IDF for you (Options A and C in Step 1).  If you "
	       "already have an ESP-IDF checkout and will point @b{ice} "
	       "at it (Option B), you don't need @b{git} at all.")
	H_ITEM("cmake and ninja",
	       "@b{ice} can install both via @b{ice tools} so you don't "
	       "have to.  If the versions on your @b{PATH} satisfy "
	       "ESP-IDF's minimum requirements, @b{ice} will happily use "
	       "them instead.")
	H_ITEM("A C/C++ cross-compiler",
	       "Always installed by @b{ice} on @b{ice init} (downloaded "
	       "into @b{~/.ice/tools/} and reused across projects).")
	H_ITEM("Python",
	       "Still used internally at build time for a few IDF tools "
	       "not yet reimplemented in C.  @b{ice} manages the Python "
	       "side itself, including a small internal environment; you "
	       "don't source @b{export.sh} or run @b{pip install} "
	       "yourself.")
	H_PARA("You do not need to install ESP-IDF, source @b{export.sh}, "
	       "or set up a Python virtual environment yourself.  "
	       "@b{ice} handles all of that.")

	H_SECTION("FIRST -- ENABLE TAB COMPLETION")
	H_PARA("Do this before anything else.  @b{ice} ships rich "
	       "tab-completion for subcommands, flags, chip targets, "
	       "config keys, aliases, and profile names -- every @b{TAB} "
	       "makes the next step of this guide easier.")
	H_LINE("@y{$} @b{eval \"$(ice completion bash)\"}   # or zsh, fish, powershell")
	H_RAW("")
	H_PARA("To persist it across sessions, append the same line to "
	       "your shell rc file (@b{~/.bashrc}, @b{~/.zshrc}, "
	       "@b{~/.config/fish/config.fish}, or your PowerShell "
	       "@b{$PROFILE}).  Completion stays in sync with the binary "
	       "automatically -- there's nothing to regenerate after an "
	       "upgrade.")
	H_LINE("@y{$} @b{echo 'eval \"$(ice completion bash)\"' >> ~/.bashrc}")
	H_RAW("")

	H_SECTION("STEP 1 -- PROVIDE AN ESP-IDF SOURCE TREE")
	H_PARA("Every project is bound to an ESP-IDF version.  There are "
	       "three ways to make one available; pick whichever fits "
	       "your situation.")

	H_PARA("@b{Option A -- let ice manage everything (recommended).}  "
	       "If you don't yet have ESP-IDF on disk, this is the "
	       "simplest path.  @b{ice} clones a single reference repo "
	       "into @b{~/.ice/esp-idf/} and creates cheap working "
	       "checkouts under @b{~/.ice/checkouts/<name>/} that share "
	       "git objects with the reference.")
	H_LINE("@y{$} @b{ice repo clone}                 one-time: clones into ~/.ice/esp-idf/")
	H_LINE("@y{$} @b{ice repo checkout v5.4}         creates ~/.ice/checkouts/v5.4/")
	H_RAW("")
	H_PARA("A bare @b{<name>} argument to @b{ice repo checkout} lands "
	       "at @b{~/.ice/checkouts/<name>/}.  Pass an explicit path "
	       "to drop the checkout anywhere you like instead:")
	H_LINE("@y{$} @b{ice repo checkout v5.4 ~/work/esp-idf-5.4}    absolute path")
	H_LINE("@y{$} @b{ice repo checkout release/v5.2 ./idf-v5.2}    relative path")
	H_RAW("")
	H_PARA("@b{ice repo list} shows the available branches and tags.  "
	       "Create as many checkouts as you need -- each one is "
	       "cheap because objects are shared with the reference.")

	H_PARA("@b{Option B -- point ice at an existing ESP-IDF "
	       "checkout.}  If you already have an ESP-IDF clone you "
	       "want to keep working in, just pass its path to "
	       "@b{ice init} in Step 2:")
	H_LINE("@y{$} @b{ice init esp32 ~/work/esp-idf}")
	H_RAW("")
	H_PARA("Anything that looks like a path (contains a @b{/} or "
	       "starts with @b{~}) is used verbatim instead of being "
	       "resolved under @b{~/.ice/checkouts/}.  Nothing is copied "
	       "or moved -- @b{ice} reads from your existing tree "
	       "directly.")

	H_PARA("@b{Option C -- borrow from an existing clone, then let "
	       "ice manage it.}  You have an ESP-IDF clone, but you want "
	       "@b{ice} to manage version checkouts going forward "
	       "without re-downloading every git object.  Use "
	       "@b{--reference} on @b{ice repo clone} to borrow objects "
	       "from your existing clone:")
	H_LINE("@y{$} @b{ice repo clone --reference ~/work/esp-idf}")
	H_RAW("")
	H_PARA("The new reference at @b{~/.ice/esp-idf/} borrows objects "
	       "from @b{~/work/esp-idf} instead of re-fetching them.  "
	       "Add @b{--dissociate} to copy the borrowed objects "
	       "locally afterwards, so the @b{ice}-managed reference no "
	       "longer depends on your original clone:")
	H_LINE("@y{$} @b{ice repo clone --reference ~/work/esp-idf --dissociate}")
	H_RAW("")
	H_PARA("After cloning the reference, create checkouts the same "
	       "way as Option A (@b{ice repo checkout v5.4}).")

	H_SECTION("STEP 2 -- SET UP YOUR PROJECT")
	H_PARA("@b{ice init} binds the current directory to a chip "
	       "target and an ESP-IDF version.  Run it from the root of "
	       "an ESP-IDF project (any directory whose top-level "
	       "@b{CMakeLists.txt} declares an IDF app).")
	H_PARA("For your first run, use the @b{hello_world} example that "
	       "ESP-IDF ships:")
	H_LINE("@y{$} @b{cd ~/.ice/checkouts/v5.4/examples/get-started/hello_world}")
	H_LINE("@y{$} @b{ice init esp32 v5.4}")
	H_RAW("")
	H_PARA("Replace @b{esp32} with your chip target (@b{esp32s3}, "
	       "@b{esp32c6}, @b{esp32h2}, ...).  Replace @b{v5.4} with "
	       "the IDF version you checked out -- or with a path if "
	       "you took Option B.")
	H_PARA("@b{ice init} will:")
	H_PARA("1. Wipe any previous build directory in this project.")
	H_PARA("2. Install the toolchain for the target chip (downloaded "
	       "into @b{~/.ice/tools/} and reused across projects).")
	H_PARA("3. Run cmake to configure the build.")
	H_PARA("4. Persist the configuration in @b{.ice/config} so "
	       "subsequent commands know which chip and IDF to use.")
	H_PARA("Run @b{ice status} afterwards to confirm what's bound:")
	H_LINE("@y{$} @b{ice status}")
	H_RAW("")

	H_SECTION("STEP 3 -- BUILD, FLASH, MONITOR")
	H_LINE("@y{$} @b{ice build}       compile")
	H_LINE("@y{$} @b{ice flash}       upload over the detected serial port")
	H_LINE("@y{$} @b{ice monitor}     tail serial output (Ctrl-] to exit)")
	H_RAW("")
	H_PARA("@b{ice flash} will rebuild the project first if anything "
	       "changed -- this matches @b{idf.py flash} behaviour and "
	       "is controlled by the @b{core.build-always} config key "
	       "(default @b{true}).  The explicit @b{ice build} above is "
	       "shown for clarity but isn't strictly required.  Set "
	       "@b{core.build-always = false} if you'd rather have "
	       "@b{ice flash} refuse to act on a stale or unbuilt "
	       "project.")
	H_PARA("If the serial port can't be auto-detected, set it "
	       "explicitly:")
	H_LINE("@y{$} @b{ice config serial.port /dev/ttyUSB0}   write")
	H_LINE("@y{$} @b{ice config serial.port}                read back")
	H_RAW("")
	H_PARA("@b{hello_world} prints chip info, free heap, and a "
	       "10-second countdown to a restart.  If you see that on "
	       "the monitor, your toolchain, flashing, and serial wiring "
	       "all work end-to-end.")

	H_SECTION("WHERE ICE KEEPS THINGS")
	H_ITEM("~/.ice/esp-idf/",
	       "Managed ESP-IDF reference clone.  Don't work in it "
	       "directly.")
	H_ITEM("~/.ice/checkouts/<name>/",
	       "Per-version working checkouts.  Borrow git objects from "
	       "the reference.")
	H_ITEM("~/.ice/tools/",
	       "Downloaded toolchains, shared across all projects.")
	H_ITEM("<project>/.ice/config",
	       "Per-project configuration: chip, IDF path, sdkconfig, "
	       "profiles.")
	H_ITEM("<project>/build/",
	       "Build artefacts.  @b{ice clean} clears this.")

	H_SECTION("COMMON NEXT STEPS")
	H_PARA("@b{Multiple chips in one project.}  Use named profiles "
	       "to keep build artefacts from different chips apart:")
	H_LINE("@y{$} @b{ice init esp32   v5.4 dev}")
	H_LINE("@y{$} @b{ice init esp32s3 v5.4 prod}")
	H_LINE("@y{$} @b{ice --profile=prod build}")
	H_RAW("")
	H_PARA("See @b{ice help init} for the full list of per-profile "
	       "knobs.")
	H_PARA("@b{Inspect or change configuration.}  @b{ice menuconfig} "
	       "opens the sdkconfig UI; @b{ice config --list} dumps "
	       "every effective setting with its source scope; "
	       "@b{ice status} shows the effective state for the active "
	       "profile.")
	H_PARA("@b{See firmware size.}  @b{ice size} summarises memory "
	       "usage; pass @b{--archives} or @b{--files} to drill in.")

	H_SECTION("TROUBLESHOOTING")
	H_ITEM("ice: command not found",
	       "The install directory isn't on your @b{PATH}.  See the "
	       "install script's output for the exact @b{export PATH} "
	       "line, or re-run the installer.")
	H_ITEM("failed to detect serial port",
	       "Pass it explicitly: @b{ice flash --port /dev/ttyUSB0}, "
	       "or persist it with @b{ice config serial.port "
	       "/dev/ttyUSB0}.  On Linux, you may need to add yourself "
	       "to the @b{dialout} group "
	       "(@b{sudo usermod -aG dialout $USER}, then log out and "
	       "back in).")
	H_ITEM("monitor won't exit",
	       "Press @b{Ctrl-]} (Control + right square bracket).  "
	       "This matches @b{idf.py monitor}'s convention.")
	H_ITEM("something else",
	       "Run with @b{-v} for full command output, or @b{ice log} "
	       "to inspect the captured logs of the last few tool runs.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice help <command>",
	       "Full help for any subcommand.")
	H_ITEM("ice help init",
	       "Chip and IDF binding, sdkconfig options, profile knobs.")
	H_ITEM("ice help repo",
	       "Managing the ESP-IDF source tree.")
	H_ITEM("ice help config",
	       "How configuration entries and scopes work.")
	H_ITEM("ice completion --help",
	       "Per-shell completion setup details."),
};
/* clang-format on */

#pragma GCC diagnostic pop

static const struct option cmd_docs_getting_started_opts[] = {OPT_END()};

const struct cmd_desc cmd_docs_getting_started_desc = {
    .name = "getting-started",
    .fn = cmd_docs_getting_started,
    .opts = cmd_docs_getting_started_opts,
    .manual = &getting_started_manual,
};

int cmd_docs_getting_started(int argc, const char **argv)
{
	parse_options(argc, argv, &cmd_docs_getting_started_desc);
	print_manual(cmd_docs_getting_started_desc.manual->name,
		     &cmd_docs_getting_started_desc);
	return 0;
}
