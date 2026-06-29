#!/bin/bash
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL=$(uname -r)
CONFIG_SRC="/boot/config-${KERNEL}"
CONFIG_DST="${REPO_DIR}/kernel.config"
MSG="${1:-${KERNEL}}"

# Copy running kernel config
if [ -f "$CONFIG_SRC" ]; then
    cp "$CONFIG_SRC" "$CONFIG_DST"
    echo "Copied $CONFIG_SRC -> kernel.config"
else
    echo "Warning: $CONFIG_SRC not found, skipping config copy"
fi

cd "$REPO_DIR"
git add -A
git commit -m "$MSG"
