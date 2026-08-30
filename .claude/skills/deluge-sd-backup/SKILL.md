---
name: deluge-sd-backup
description: Backup the physical Deluge SD card to ~/Documents/Deluge/TimCardBU (rsync mirror of the card root, additive by default), refusing DelugEmu's look-alike disk image
allowed-tools: Bash, Read
---

# deluge-sd-backup

Copies the mounted Deluge SD card into `~/Documents/Deluge/TimCardBU` as a flat mirror of the
card root (the existing layout in that directory). Everything lives in
`.claude/skills/deluge-sd-backup/deluge_sd_backup.sh`:

```bash
bash .claude/skills/deluge-sd-backup/deluge_sd_backup.sh              # backup (additive)
bash .claude/skills/deluge-sd-backup/deluge_sd_backup.sh --dry-run    # preview only
bash .claude/skills/deluge-sd-backup/deluge_sd_backup.sh --mirror     # exact mirror: also delete backup files gone from the card
bash .claude/skills/deluge-sd-backup/deluge_sd_backup.sh --eject      # eject the card when done
```

Options combine; `--source PATH` overrides card detection.

## Behaviour

- Finds the card at `/Volumes/DELUGE`, or failing that scans `/Volumes` for a volume with a
  `SONGS/` or `SETTINGS/` directory. Refuses a volume backed by a **disk image** (checked via
  `diskutil info`) because DelugEmu's SD image also mounts as `DELUGE` — only the physical card
  should land in TimCardBU.
- `rsync -a` card → backup, excluding macOS junk (`.Spotlight-V100`, `.Trashes`, `.fseventsd`,
  `.DS_Store`, `._*` AppleDouble files). Only changed files are copied, so re-runs are fast.
- **Additive by default**: nothing is ever deleted from the backup. After the copy it lists any
  files that exist only in the backup (deleted/renamed on the card) so you can decide; `--mirror`
  is the explicit opt-in that removes them. Ask before using `--mirror` unless Tim asked for an
  exact mirror.
- Prints total size and file count at the end. `--dry-run` shows what would change without
  touching anything.

## Gotchas

- Card timestamps: the Deluge has no RTC, so firmware-written files carry 1969 dates — that's
  normal, rsync still syncs them correctly by size/mtime.
- If no card is found, the error lists what *is* mounted; the card only appears in `/Volumes`
  when plugged into the Mac (a Deluge connected over USB does not expose its card).

Skill files stay on `local-fixes` only — like `CLAUDE.md`, do not merge them into fix branches
destined for upstream.
