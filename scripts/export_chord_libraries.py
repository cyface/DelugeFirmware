#!/usr/bin/env python3
"""Write the built-in chord libraries out as CHORDS/<name>.XML files.

Parses the Chord definitions in src/deluge/gui/ui/keyboard/chords.cpp so the files always match what the
firmware ships, and emits the same format readChordLibraryFile() reads (see chord_library_file.h). The page
names for the jazz set are the ones the firmware shows for it.

Usage: scripts/export_chord_libraries.py [output-dir]   (default contrib/sd_card/CHORDS)
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src/deluge/gui/ui/keyboard/chords.cpp")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "contrib/sd_card/CHORDS")
FLAT = "\x81"

OCT = 12
INTERVALS = {
    "ROOT": 0,
    "MIN2": 1,
    "MAJ2": 2,
    "MIN3": 3,
    "MAJ3": 4,
    "P4": 5,
    "AUG4": 6,
    "DIM5": 6,
    "P5": 7,
    "AUG5": 8,
    "MIN6": 8,
    "MAJ6": 9,
    "DIM7": 9,
    "MIN7": 10,
    "DOM7": 10,
    "MAJ7": 11,
    "OCT": OCT,
    "MIN9": 1 + OCT,
    "MAJ9": 2 + OCT,
    "MIN10": 3 + OCT,
    "MAJ10": 4 + OCT,
    "P11": 5 + OCT,
    "AUG11": 6 + OCT,
    "DIM12": 6 + OCT,
    "P12": 7 + OCT,
    "MIN13": 8 + OCT,
    "MAJ13": 9 + OCT,
    "MIN14": 10 + OCT,
    "MAJ14": 11 + OCT,
}
# Preferred spelling for each semitone count when writing
CANONICAL = {}
for name, value in INTERVALS.items():
    CANONICAL.setdefault(value, name)
CANONICAL[6] = "AUG4"
CANONICAL[8] = "AUG5"
CANONICAL[9] = "MAJ6"
CANONICAL[10] = "MIN7"
CANONICAL[18] = "AUG11"
CANONICAL[20] = "MIN13"
CANONICAL[21] = "MAJ13"
CANONICAL[22] = "MIN14"

PAGE_NAMES = {"jazzChordLibrary": ["Minor", "Major", "Altered"]}
FILE_NAMES = {"defaultChordLibrary": "Default", "jazzChordLibrary": "Jazz"}


def evaluate(expr):
    expr = expr.strip()
    if expr == "NONE":
        return None
    return eval(expr, {"__builtins__": {}}, INTERVALS)


def spell(value):
    if value < 0:
        return "-" + spell(-value)
    return CANONICAL.get(value, str(value))


def parse_string(text):
    """A C string literal sequence like "-7" FLAT_CHAR_STR "5" -> python string."""
    out = ""
    for token in re.findall(r'"((?:[^"\\]|\\.)*)"|(FLAT_CHAR_STR)', text):
        out += FLAT if token[1] else token[0]
    return out


def parse_chords(source):
    chords = {}
    pattern = re.compile(r"const Chord (k\w+) = \{(.*?)\}\};", re.DOTALL)
    for match in pattern.finditer(source):
        ident, body = match.group(1), match.group(2) + "}}"
        # name is everything up to the first NoteSet(
        name_part, rest = body.split("NoteSet(", 1)
        name = parse_string(name_part)
        voicings_part = rest.split("),", 1)[1]
        voicings = []
        # A voicing is {offsets...} optionally followed by its name, which is a run of string literals and
        # FLAT_CHAR_STR macros
        for v in re.finditer(
            r"\{([^{}]*)\}((?:\s*,?\s*(?:\"(?:[^\"\\]|\\.)*\"|FLAT_CHAR_STR))*)",
            voicings_part,
        ):
            offsets = [evaluate(o) for o in v.group(1).split(",") if o.strip()]
            notes = [o for o in offsets if o is not None]
            # A voicing with no notes (or only zeros) is an unused slot in the C++ table
            if not notes or all(n == 0 for n in notes):
                continue
            voicings.append((notes, parse_string(v.group(2) or "")))
        chords[ident] = (name, voicings)
    return chords


def parse_libraries(source):
    libraries = {}
    for match in re.finditer(
        r"std::array<const Chord, \w+> (\w+) = \{(.*?)\};", source, re.DOTALL
    ):
        body = re.sub(r"//.*", "", match.group(2))
        libraries[match.group(1)] = [i.strip() for i in body.split(",") if i.strip()]
    return libraries


def xml_escape(text):
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace(FLAT, "b")
    )


def write_library(path, name, chord_ids, chords, page_names):
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<chordLibrary name="{xml_escape(name)}">',
    ]
    for page in range(0, len(chord_ids), 8):
        page_name = page_names[page // 8] if page // 8 < len(page_names) else None
        lines.append(
            f'\t<page name="{xml_escape(page_name)}">' if page_name else "\t<page>"
        )
        for ident in chord_ids[page : page + 8]:
            chord_name, voicings = chords[ident]
            lines.append(f'\t\t<chord name="{xml_escape(chord_name)}">')
            for notes, supplemental in voicings:
                attrs = f' name="{xml_escape(supplemental)}"' if supplemental else ""
                lines.append(
                    f'\t\t\t<voicing{attrs} notes="{", ".join(spell(n) for n in notes)}" />'
                )
            lines.append("\t\t</chord>")
        lines.append("\t</page>")
    lines.append("</chordLibrary>")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    with open(SRC, encoding="utf-8") as f:
        source = f.read()
    chords = parse_chords(source)
    libraries = parse_libraries(source)
    os.makedirs(OUT, exist_ok=True)
    for ident, file_name in FILE_NAMES.items():
        path = os.path.join(OUT, file_name + ".XML")
        write_library(
            path, file_name, libraries[ident], chords, PAGE_NAMES.get(ident, [])
        )
        print(f"wrote {path}: {len(libraries[ident])} chords")


if __name__ == "__main__":
    main()
