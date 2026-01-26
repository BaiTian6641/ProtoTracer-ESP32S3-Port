#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [DIR]
Generates .md5 checksum files for files under DIR (default: .)

Behavior:
  - For files ending with .json the script will write a sibling file with the .md5 extension
    (e.g. "device_animation.json" -> "device_animation.md5").
  - For other files the script will append ".md5" to the filename (e.g. "file.bin" -> "file.bin.md5").

Options:
  -n          Dry run (show actions but don't write)
  -f          Force overwrite every .md5 file
  -x EXT      Also replace extension EXT with .md5 (can be used multiple times). Default: json
  -h          Show this help

Examples:
  # Dry run in current directory
  ./generate_md5s.sh -n

  # Generate/overwrite .md5 files under /path/to/dir (json -> .md5)
  ./generate_md5s.sh /path/to/dir

  # Also replace .bin extension with .md5
  ./generate_md5s.sh -x bin /path/to/dir
EOF
}

DRY_RUN=0
FORCE=0
DIR="."

# default extension replacement list
replace_exts=(json)

while getopts ":nfx:h" opt; do
  case "$opt" in
    n) DRY_RUN=1 ;;
    f) FORCE=1 ;;
    x) replace_exts+=("$OPTARG") ;;
    h) usage; exit 0 ;;
    :) echo "Option -$OPTARG requires an argument." >&2; usage; exit 1 ;;
    *) usage; exit 1 ;;
  esac
done
shift $((OPTIND-1))

[ $# -ge 1 ] && DIR="$1"

if ! command -v md5sum >/dev/null 2>&1; then
  echo "Error: md5sum not found. Install coreutils or use a Linux environment." >&2
  exit 2
fi

# helper: test if extension (case-insensitive) is in the list
extension_in_list() {
  local ext="$1"
  for e in "${replace_exts[@]}"; do
    if [[ "${e,,}" == "${ext,,}" ]]; then
      return 0
    fi
  done
  return 1
}

# Walk files safely (handles spaces/newlines) and skip existing .md5 files
find "$DIR" -type f ! -name '*.md5' -print0 |
while IFS= read -r -d '' file; do
  checksum=$(md5sum "$file" | awk '{print $1}')
  filename=$(basename -- "$file")
  dir=$(dirname -- "$file")

  # Determine output path: replace extension if matches list, else append .md5
  out=""
  if [[ "$filename" == *.* ]]; then
    ext="${filename##*.}"
    base="${filename%.*}"
    if extension_in_list "$ext"; then
      out="$dir/$base.md5"
    else
      out="$file.md5"
    fi
  else
    out="$file.md5"
  fi

  if [ -f "$out" ] && [ "$FORCE" -eq 0 ]; then
    existing=$(tr -d '\n\r' < "$out" 2>/dev/null || echo "")
    if [ "$existing" = "$checksum" ]; then
      echo "OK  (unchanged) : $file -> $(basename "$out")"
      continue
    fi
  fi

  if [ "$DRY_RUN" -eq 1 ]; then
    printf "WRITE : %s -> %s (checksum: %s)\n" "$file" "$out" "$checksum"
  else
    printf "%s\n" "$checksum" > "$out"
    echo "WROTE: $out"
  fi
done
