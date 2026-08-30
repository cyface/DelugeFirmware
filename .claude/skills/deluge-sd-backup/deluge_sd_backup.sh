#!/bin/bash
# Backup the Deluge SD card to ~/Documents/Deluge/TimCardBU.
# Default is additive (never deletes from the backup); --mirror makes it exact.
set -euo pipefail

DEST="$HOME/Documents/Deluge/TimCardBU"
SRC=""
MIRROR=0
DRYRUN=0
EJECT=0

usage() {
	cat <<'EOF'
usage: deluge_sd_backup.sh [--mirror] [--dry-run|-n] [--eject] [--source PATH]

  --mirror       also delete files from the backup that are no longer on the card
  --dry-run, -n  show what would be copied (and deleted, with --mirror) without doing it
  --eject        eject the card after a successful backup
  --source PATH  card mount point (default: /Volumes/DELUGE, else auto-detect)
EOF
	exit 1
}

while [ $# -gt 0 ]; do
	case "$1" in
	--mirror) MIRROR=1 ;;
	--dry-run | -n) DRYRUN=1 ;;
	--eject) EJECT=1 ;;
	--source)
		shift
		SRC="${1:?--source needs a path}"
		;;
	-h | --help) usage ;;
	*)
		echo "unknown option: $1" >&2
		usage
		;;
	esac
	shift
done

looks_like_card() { [ -d "$1/SONGS" ] || [ -d "$1/SETTINGS" ]; }

# Find the card.
if [ -z "$SRC" ]; then
	if [ -d /Volumes/DELUGE ] && looks_like_card /Volumes/DELUGE; then
		SRC=/Volumes/DELUGE
	else
		for v in /Volumes/*/; do
			v="${v%/}"
			[ "$v" = "/Volumes/Macintosh HD" ] && continue
			if looks_like_card "$v"; then
				SRC="$v"
				break
			fi
		done
	fi
fi
if [ -z "$SRC" ] || [ ! -d "$SRC" ]; then
	echo "ERROR: no Deluge SD card found. Mounted volumes:" >&2
	ls /Volumes >&2
	exit 1
fi
if ! looks_like_card "$SRC"; then
	echo "ERROR: $SRC has no SONGS/ or SETTINGS/ directory - not a Deluge card?" >&2
	exit 1
fi

# Refuse a mounted disk image (DelugEmu's SD image also mounts as DELUGE).
dev=$(df "$SRC" | awk 'NR==2{print $1}')
if diskutil info "$dev" 2>/dev/null | grep -q "Disk Image"; then
	echo "ERROR: $SRC ($dev) is a mounted disk image (DelugEmu?), not the physical card." >&2
	echo "Pass --source explicitly if you really mean to back it up." >&2
	exit 1
fi

mkdir -p "$DEST"

EXCLUDES=(--exclude=.Spotlight-V100 --exclude=.Trashes --exclude=.fseventsd
	--exclude=.TemporaryItems --exclude=.DS_Store --exclude='._*')
RSYNC_ARGS=(-a -v "${EXCLUDES[@]}")
[ "$MIRROR" = 1 ] && RSYNC_ARGS+=(--delete)
[ "$DRYRUN" = 1 ] && RSYNC_ARGS+=(--dry-run)

echo "Backing up: $SRC ($dev) -> $DEST"
[ "$DRYRUN" = 1 ] && echo "(dry run - nothing will be changed)"
rsync "${RSYNC_ARGS[@]}" "$SRC/" "$DEST/"

# In additive mode, report what an exact mirror would remove so stale files are visible.
if [ "$MIRROR" = 0 ]; then
	stale=$(rsync -a -n -v --delete "${EXCLUDES[@]}" "$SRC/" "$DEST/" 2>/dev/null |
		grep '^deleting ' || true)
	if [ -n "$stale" ]; then
		echo
		echo "In the backup but no longer on the card (kept; re-run with --mirror to remove):"
		echo "$stale" | sed 's/^deleting /  /'
	fi
fi

if [ "$DRYRUN" = 0 ]; then
	echo
	echo "Backup size: $(du -sh "$DEST" | cut -f1), $(find "$DEST" -type f ! -name '._*' ! -name .DS_Store | wc -l | tr -d ' ') files"
	if [ "$EJECT" = 1 ]; then
		diskutil eject "$SRC"
	fi
	echo "Done."
fi
