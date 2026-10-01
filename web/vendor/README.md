# Vendored browser libraries

Unmodified release files, embedded into the plugin at build time by `embed_assets.sh`.
The build needs no npm, and the dashboard needs no internet access.

| File | Package | Version | License | Global |
| ---- | ------- | ------- | ------- | ------ |
| `preact.min.umd.js` | [preact](https://www.npmjs.com/package/preact) `dist/preact.min.umd.js` | 10.29.8 | MIT | `preact` |
| `preact-hooks.umd.js` | [preact](https://www.npmjs.com/package/preact) `hooks/dist/hooks.umd.js` | 10.29.8 | MIT | `preactHooks` |
| `htm.umd.js` | [htm](https://www.npmjs.com/package/htm) `dist/htm.umd.js` | 3.1.1 | Apache-2.0 | `htm` |
| `uPlot.iife.min.js` | [uplot](https://www.npmjs.com/package/uplot) `dist/uPlot.iife.min.js` | 1.6.32 | MIT | `uPlot` |
| `uPlot.min.css` | [uplot](https://www.npmjs.com/package/uplot) `dist/uPlot.min.css` | 1.6.32 | MIT | — |

License texts are in `licenses/`.

## Updating

Download the package tarball from `https://registry.npmjs.org/<package>/-/<package>-<version>.tgz`,
check it against the `integrity` value in the registry metadata, and copy the files listed above
from `package/`. Before committing, check that no file contains `</script`, which would end the
inline `<script>` block early, or `TR_WEB_DELIM`, the raw-string delimiter used by `embed_assets.sh`.

`embed_assets.sh` loads the scripts in this order: Preact, then the hooks (which need `preact`),
then htm, then uPlot.
