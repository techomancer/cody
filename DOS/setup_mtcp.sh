#!/bin/sh
# setup_mtcp.sh — Extract mTCP source zip with all-lowercase names, then create
# symlinks for any mixed-case #include references that still exist in the source.
#
# Usage:  sh setup_mtcp.sh   (from the DOS/ directory)
#         sh setup_mtcp.sh /path/to/mTCP-src_2025-01-10.zip

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ZIP="${1:-$SCRIPT_DIR/mTCP-src_2025-01-10.zip}"
DEST="$SCRIPT_DIR/mTCP"

if [ ! -f "$ZIP" ]; then
    echo "ERROR: zip not found: $ZIP"
    exit 1
fi

echo "Removing old mTCP directory..."
rm -rf "$DEST"

echo "Extracting $ZIP with lowercase names..."
unzip -LL -q "$ZIP" -d "$DEST"

# If the zip extracts into a single subdirectory, flatten it to $DEST/tcpinc etc.
subdirs=$(find "$DEST" -mindepth 1 -maxdepth 1 -type d | wc -l)
if [ "$subdirs" -eq 1 ]; then
    subdir=$(find "$DEST" -mindepth 1 -maxdepth 1 -type d)
    echo "Flattening $subdir -> $DEST ..."
    # Move contents up one level
    find "$subdir" -mindepth 1 -maxdepth 1 | while IFS= read -r item; do
        mv "$item" "$DEST/"
    done
    rmdir "$subdir"
fi

echo "Scanning for mixed-case #include references..."
# Collect every quoted include target from all mTCP .h and .cpp files
TMPFILE="$(mktemp)"
find "$DEST" -type f \( -name "*.h" -o -name "*.cpp" -o -name "*.cfg" \) \
  -exec grep -oh '#include "[^"]*"' {} \; \
  | sed 's/#include "//;s/"//' \
  | sort -u > "$TMPFILE"

# For each referenced name, if it doesn't exist as-is but a lowercase version does,
# create a symlink from the referenced name to the lowercase version (in every dir
# that contains the lowercase file).
created=0
while IFS= read -r ref; do
    lower="$(echo "$ref" | tr '[:upper:]' '[:lower:]')"
    if [ "$ref" = "$lower" ]; then
        continue  # already lowercase, no action needed
    fi
    # Find directories that contain the lowercase version
    while IFS= read -r lpath; do
        dir="$(dirname "$lpath")"
        target="$dir/$ref"
        if [ ! -e "$target" ]; then
            ln -s "$lower" "$target"
            echo "  symlink: $target -> $lower"
            created=$((created + 1))
        fi
    done < <(find "$DEST" -type f -name "$lower" 2>/dev/null)
done < "$TMPFILE"

rm -f "$TMPFILE"

echo ""
echo "Done. mTCP extracted to: $DEST"
echo "Symlinks created: $created"
