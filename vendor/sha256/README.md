# sha256

Portable SHA-256 implementation, imported from Brad Conte's
[crypto-algorithms](https://github.com/B-Con/crypto-algorithms)
collection.  Used wherever ice needs SHA-256 without pulling in a
crypto library — most notably the `ice image elf2image` command (ELF
hash patched into `esp_app_desc_t`, optional whole-image digest
appended).

## Upstream

- Repository: https://github.com/B-Con/crypto-algorithms
- Commit:     `cfbde48414baacf51fc7c74f275190881f037d32`
- Date:       2026-04-15
- License:    Public domain (see [LICENSE](LICENSE) and upstream README)

## Files imported

```
sha256.c    Implementation
sha256.h    Public API (SHA256_CTX, sha256_init/update/final, SHA256_BLOCK_SIZE)
```

Note that upstream's `SHA256_BLOCK_SIZE` macro is misnamed — it holds
the output digest size (32 bytes), not the block size (64 bytes).
Callers should treat it as "digest length."

## Local patches

1. `sha256.c`: `#include <memory.h>` replaced with `#include <string.h>`
   for musl and broader POSIX portability.  `<memory.h>` is a legacy
   glibc header not provided by musl (used for the STATIC=1 Linux
   builds), macOS, or strict POSIX systems.

## Re-sync

```
SHA=<new upstream sha>
curl -fsSL "https://raw.githubusercontent.com/B-Con/crypto-algorithms/$SHA/sha256.c" \
    -o vendor/sha256/sha256.c
curl -fsSL "https://raw.githubusercontent.com/B-Con/crypto-algorithms/$SHA/sha256.h" \
    -o vendor/sha256/sha256.h
# Re-apply the local patch (memory.h -> string.h) in sha256.c.
# Update the "Upstream" section above with the new SHA and date.
make test
```
