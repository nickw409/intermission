#!/usr/bin/env bash
# Download a MovieLens dataset into data/raw/<name>/ and verify its published MD5.
# Usage: tools/download_movielens.sh <ml-latest-small|ml-32m>
# Data is licensed by GroupLens for non-commercial use; never commit it.
set -euo pipefail

name="${1:-}"
case "$name" in
  ml-latest-small|ml-32m) ;;
  *) echo "usage: $0 <ml-latest-small|ml-32m>" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$root/data/raw"
base="https://files.grouplens.org/datasets/movielens"
mkdir -p "$dest"
tmp="$(mktemp -d "$dest/.dl-XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

if [[ -f "$dest/$name/.verified" ]]; then
  echo "$name already downloaded and verified"; exit 0
fi

echo "downloading $name ..."
curl -fsSL --retry 3 -o "$tmp/$name.zip" "$base/$name.zip"
curl -fsSL --retry 3 -o "$tmp/$name.zip.md5" "$base/$name.zip.md5"

expected="$(grep -Eo '[0-9a-f]{32}' "$tmp/$name.zip.md5" | head -1)"
actual="$(md5sum "$tmp/$name.zip" | cut -d' ' -f1)"
if [[ -z "$expected" || "$expected" != "$actual" ]]; then
  echo "checksum mismatch for $name: expected '$expected', got '$actual'" >&2
  exit 1
fi
echo "checksum OK ($actual)"

rm -rf "${dest:?}/$name"
unzip -q "$tmp/$name.zip" -d "$tmp/x"
mv "$tmp/x/$name" "$dest/$name"
echo "$actual" > "$dest/$name/.verified"
echo "extracted to data/raw/$name"
