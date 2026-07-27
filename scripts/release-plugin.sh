#!/usr/bin/env bash
# Release a single plugin as a GitHub release. Produces:
#   - tag  : {slug}/v{version}
#   - release with {plugin}.o attached
#
# Convention matches other multi-plugin NT repos (e.g. NerdRoger/disting_nt_plugins)
# so the nt_helper gallery can point downloadUrl at a specific .o.
#
# Usage: scripts/release-plugin.sh <plugin-slug> <version> [commit-ish]
#   plugin-slug   The URL slug for the tag prefix, e.g. "fugue-nt".
#                 Also names the module folder as modules/{camelSlug}NT/.
#   version       Semver without leading 'v', e.g. "1.0.0".
#   commit-ish    Optional git ref for the tag (default: HEAD).
#
# Example:
#   scripts/release-plugin.sh shift-nt 1.0.0

set -euo pipefail

if [[ $# -lt 2 ]]; then
	echo "Usage: $0 <plugin-slug> <version> [commit-ish]" >&2
	echo "  e.g. $0 fugue-nt 1.0.0" >&2
	exit 2
fi

SLUG="$1"
VERSION="$2"
COMMIT="${3:-HEAD}"

# Map plugin-slug (kebab) → module folder (camel). "fugue-nt" → "fugueNT".
# Splits on '-', preserves first word, then capitalises each subsequent word's
# first letter, joins. We also uppercase a trailing "nt" → "NT".
MODULE_DIR="$(
	python3 -c "
import sys
parts = sys.argv[1].split('-')
out = parts[0] + ''.join(w[:1].upper() + w[1:] for w in parts[1:])
if out.endswith('Nt'): out = out[:-2] + 'NT'
print(out)
" "$SLUG"
)"

MODULE_PATH="modules/${MODULE_DIR}"
if [[ ! -d "$MODULE_PATH" ]]; then
	echo "error: no such module folder: $MODULE_PATH" >&2
	exit 3
fi

OBJ="${MODULE_PATH}/plugins/${MODULE_DIR}.o"
TAG="${SLUG}/v${VERSION}"

# Rebuild clean so the release asset is deterministic from source.
echo "==> Building ${MODULE_DIR}"
(cd "$MODULE_PATH" && rm -rf plugins && make >/dev/null)
if [[ ! -f "$OBJ" ]]; then
	echo "error: build did not produce $OBJ" >&2
	exit 4
fi

# Tag (if it doesn't already exist).
if git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null; then
	echo "==> Tag ${TAG} already exists — reusing"
else
	echo "==> Tagging ${TAG} at $(git rev-parse --short "$COMMIT")"
	git tag -a "$TAG" "$COMMIT" -m "${MODULE_DIR} v${VERSION}"
	git push origin "$TAG"
fi

# Create the release. If it already exists, upload the .o (replacing any prior).
if gh release view "$TAG" >/dev/null 2>&1; then
	echo "==> Release ${TAG} exists — replacing asset"
	gh release upload "$TAG" "$OBJ" --clobber
else
	NOTES="${MODULE_DIR} v${VERSION}. See docs/${SLUG}.md for details.

**Install**: copy \`${MODULE_DIR}.o\` to your Disting NT MicroSD card at
\`programs/plug-ins/\` (or a subfolder). Requires firmware supporting
API version 13 or newer."

	echo "==> Creating release ${TAG}"
	gh release create "$TAG" "$OBJ" \
		--title "${MODULE_DIR} v${VERSION}" \
		--notes "$NOTES"
fi

echo "==> Done: https://github.com/stuart78/SignalFunctionSet-DistingNT/releases/tag/${TAG}"
