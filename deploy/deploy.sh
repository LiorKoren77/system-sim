#!/usr/bin/env bash
# Builds the web app here and copies it to the server. Nothing is built on the server: the site is
# static files (docs/deploy.md says how the server is set up).
#
# Usage: deploy/deploy.sh [--dry-run] [--skip-precompute]
#   --dry-run          list what rsync would change; change nothing on the server
#   --skip-precompute  reuse the datasets already in web/app/public/datasets (they only change when
#                      a model or a scenario does; the precompute takes about 3 minutes)
#
# Environment: SYSSIM_HOST (ssh alias of the server, default `lightsail`), SYSSIM_DEST (folder on
# the server, default /var/www/syssim), SIMSYS (the Release binary, default build/gcc14-release/simsys).
set -euo pipefail

host="${SYSSIM_HOST:-lightsail}"
dest="${SYSSIM_DEST:-/var/www/syssim}"
dry_run=()
precompute=yes
for arg in "$@"; do
  case "$arg" in
    --dry-run) dry_run=(-n) ;;
    --skip-precompute) precompute=no ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

cd "$(dirname "$0")/.."
simsys="$(realpath "${SIMSYS:-build/gcc14-release/simsys}")" || true
if [ "$precompute" = yes ] && [ ! -x "$simsys" ]; then
  echo "no simsys binary at $simsys: run  cmake --preset gcc14-release && cmake --build --preset gcc14-release" >&2
  exit 1
fi

# nvm loads only in an interactive shell (CONTRIBUTING.md, section 5), so a script loads it itself.
# shellcheck disable=SC1091
[ -s "$HOME/.nvm/nvm.sh" ] && source "$HOME/.nvm/nvm.sh"

(
  cd web
  npm ci
  if [ "$precompute" = yes ]; then npm run precompute -- --simsys "$simsys"; fi
  npm run build
)

# Trailing slashes: copy the folder's contents. --delete removes files the build no longer has.
rsync -av --delete "${dry_run[@]}" web/app/dist/ "$host:$dest/"
echo "deployed to $host:$dest (check the site; if it shows old data, purge Cloudflare's cache)"
