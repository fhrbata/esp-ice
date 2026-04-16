# esp-serial-flasher

Portable C library for flashing Espressif SoCs from a host, imported as
a git subtree under `src/`.  Built as a static library (`libflasher.a`)
with `PORT=USER_DEFINED` — ice provides its own port implementation
bridging to its platform serial abstraction.

## Upstream

- Repository: https://github.com/espressif/esp-serial-flasher
- Commit:     `6c2bcc406b80`
- Date:       2026-04-16
- License:    Apache-2.0 (see `src/LICENSE`)

## Build

```
make -C vendor/esp-serial-flasher
```

or, from the project root, `make vendor` builds all vendor libraries.

## Re-sync

Update the subtree to a new upstream revision (tag or branch):

```
git subtree pull --prefix=vendor/esp-serial-flasher/src \
    https://github.com/espressif/esp-serial-flasher.git <ref> --squash
```

Then rebuild and run the test suite:

```
make mrproper
make test
```

Update the "Upstream" section above with the new commit and date.
