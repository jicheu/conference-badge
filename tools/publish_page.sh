#!/bin/sh
# Builds docs/index.html from tools/badge_ble.html, with a version stamp
# (date + short content hash). Serve the docs/ folder with GitHub Pages
# ("Deploy from a branch", folder /docs), or copy docs/index.html anywhere
# that serves https. No GitHub Actions needed.
set -e
cd "$(dirname "$0")/.."
mkdir -p docs
hash=$(sha1sum tools/badge_ble.html | cut -c1-7)
stamp="$hash $(date -u +'%Y-%m-%d %H:%M') UTC"
sed "s/__BUILD__/$stamp/" tools/badge_ble.html > docs/index.html
touch docs/.nojekyll
echo "docs/index.html written, version: $stamp"
