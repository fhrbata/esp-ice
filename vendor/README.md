# vendor/

Third-party source code imported verbatim (or with minimal, documented
local patches) from upstream projects.  Code here is **not** maintained
by ice; the canonical copy lives upstream.  Include paths from ice
sources use the explicit `vendor/<name>/` prefix so it is immediately
visible at the call site that the header is imported, not native:

```c
#include "vendor/sha256/sha256.h"
```

## Layout

Each vendored module lives in its own subdirectory and ships with a
`README.md` documenting:

- Upstream URL
- Pinned commit SHA (or release tag)
- Date imported
- License
- Any local patches applied after import
- Re-sync procedure (typically: overwrite files from upstream, then
  re-apply the documented local patches)

A `LICENSE` file captures the upstream licensing terms when the source
does not carry an SPDX header inline.

## Re-sync

For plain-vendored modules (most of the current contents), re-syncing
to a newer upstream revision means:

1. Fetch the files listed in the module's README from the new SHA.
2. Re-apply the local patches listed in the module's README.
3. Update the README with the new SHA and date.
4. Run the project's test suite.
