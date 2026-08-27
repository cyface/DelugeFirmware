"""Shared bits for the worked examples: import the module from the skill dir, pick a staging dir."""

import os
import sys

SKILL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if SKILL_DIR not in sys.path:
    sys.path.insert(0, SKILL_DIR)

import deluge_preset as dp  # noqa: F401  (re-exported for the examples)


def staging_root():
    """Where the examples write. Mirrors the SD card: KITS/, SYNTHS/, SAMPLES/ underneath.

    Override with DELUGE_STAGING=/path (e.g. the DelugEmu card at
    ~/Library/Application Support/DelugEmu/sdcard_rw to test-load straight away).
    """
    root = os.environ.get("DELUGE_STAGING") or os.path.expanduser("~/deluge-staging")
    os.makedirs(root, exist_ok=True)
    return root


def report(rep, path):
    print(rep)
    if not rep.ok:
        sys.exit(f"validation failed for {path}")
    print(f"wrote {path}")
