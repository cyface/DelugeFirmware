#!/usr/bin/env python3
# Copyright © 2026 Synthstrom Audible Ltd
#
# This file is part of The Synthstrom Audible Deluge Firmware.
#
# The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free Software Foundation,
# either version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this program. If not,
# see <https://www.gnu.org/licenses/>.
"""Build a plugin kernel into a .dlp blob for the SD card (tier 2, fork issue #41).

    src/deluge/plugin/tools/pack_dlp.py --all --out build/plugins
    src/deluge/plugin/tools/pack_dlp.py Tape --out /Volumes/DELUGE/PLUGINS/tape.dlp

What it does, per plugin:

  1. asks the firmware's own descriptors what the plugin is - dump_builtin_descriptors.cpp is built
     and run so that names, param and model tables, sizes and entry-point symbols come from
     builtin_fx.h / builtin_sources.h rather than a second copy that could drift;
  2. re-checks every `sizeof` the descriptor reports by compiling a _Static_assert for the *target*,
     since the dumper ran on the development machine;
  3. cross-compiles and links the kernel with plugin.ld into a relocation-free image at address 0,
     and refuses anything with an import, a leftover relocation or a writable section;
  4. writes the .dlp: header, descriptor, tables and strings as offsets (plugin_blob.h), then image;
  5. reads the file back through the host's own parser (read_dlp.c -> plugin_blob.h) and diffs the
     descriptor it yields against the one from step 1, field by field.

Step 5 is the point of the exercise: what the loader builds on the Deluge has to be
indistinguishable from the built-in descriptor it replaces.
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
PLUGIN_DIR = ROOT / "src" / "deluge" / "plugin"
TOOLS_DIR = PLUGIN_DIR / "tools"
TOOLCHAIN_BIN = ROOT / "toolchain" / "current" / "arm-none-eabi-gcc" / "bin"

# Target flags: the firmware's, plus what a blob needs (position independent, freestanding, no libc).
# Shared with check_freestanding.sh through --print-flags so the two can never disagree about how a
# kernel is built.
TARGET_FLAGS = [
    "-mcpu=cortex-a9",
    "-mfpu=neon",
    "-mfloat-abi=hard",
    "-mthumb",
    "-O2",
    "-fPIC",
    "-mpic-data-is-text-relative",
    "-ffreestanding",
    "-nostdlib",
    "-fno-builtin",
    "-fno-exceptions",
    "-fno-asynchronous-unwind-tables",
    "-fno-math-errno",
    "-ffunction-sections",
    "-fdata-sections",
    "-fvisibility=hidden",
    "-Wall",
    "-Werror",
    "-std=gnu23",
    "-I",
    str(ROOT / "src" / "deluge"),
]

MAGIC = 0x42504C44  # "DLPB"
FORMAT_VERSION = 1
IMAGE_ALIGN = 32
KIND_FX = 1
KIND_SOURCE = 2
TOOL_VERSION = 1

HEADER_FORMAT = "<12I"  # see DelugePluginBlobHeader
FX_DESC_FORMAT = "<6I"  # see DelugeFxBlobDesc
FX_PARAM_FORMAT = "<3Ii"
SOURCE_DESC_FORMAT = "<9I"  # see DelugeSourceBlobDesc
SOURCE_MODEL_FORMAT = "<9I"  # name(2) + fileName(1) + macros(3 * 2)

# Header field offsets, for the corruption tests (see DelugePluginBlobHeader).
FIELD = {
    name: 4 * index
    for index, name in enumerate(
        [
            "magic",
            "formatVersion",
            "abiVersion",
            "kind",
            "fileSize",
            "crc32",
            "imageOffset",
            "imageSize",
            "descOffset",
            "descSize",
            "nameOffset",
            "toolVersion",
        ]
    )
}


def run(command, **kwargs):
    """Run a command, showing what failed rather than a bare traceback."""
    result = subprocess.run(
        command, capture_output=True, text=True, check=False, **kwargs
    )
    if result.returncode != 0:
        printable = " ".join(map(str, command))
        sys.exit(
            f"pack_dlp: command failed: {printable}\n{result.stdout}{result.stderr}"
        )
    return result.stdout


def tool(name):
    path = TOOLCHAIN_BIN / name
    if not path.exists():
        sys.exit(f"pack_dlp: {path} not found - run ./dbt to fetch the toolchain")
    return str(path)


def native_compiler(language):
    """The development machine's compiler: for the offline tools only, never for Deluge code."""
    if language == "c++":
        return os.environ.get("CXX", "c++")
    return os.environ.get("CC", "cc")


def descriptors(work):
    """Build and run dump_builtin_descriptors: ask the firmware what its built-in plugins are."""
    kernels = sorted((PLUGIN_DIR / "fx").glob("*.c"))
    kernels += sorted((PLUGIN_DIR / "source").glob("*.c"))
    include = str(ROOT / "src" / "deluge")
    objects = []
    for kernel in kernels:
        obj = work / (kernel.stem + ".native.o")
        command = [native_compiler("c"), "-std=gnu23", "-O1", "-c", str(kernel)]
        run([*command, "-I", include, "-o", str(obj)])
        objects.append(str(obj))
    binary = work / "dump_builtin_descriptors"
    command = [native_compiler("c++"), "-std=c++20", "-O1", "-I", include]
    command.append(str(TOOLS_DIR / "dump_builtin_descriptors.cpp"))
    run([*command, *objects, "-o", str(binary)])
    return json.loads(run([str(binary)]))


def check_target_sizes(plugin, work):
    """Re-evaluate the descriptor's sizeof() for the target: the dumper ran on another machine."""
    header = plugin["header"].replace("src/deluge/", "")
    lines = [f'#include "{header}"']
    for expression, size in [
        ("stateSizeExpr", "stateSize"),
        ("voiceStateSizeExpr", "voiceStateSize"),
    ]:
        if expression in plugin:
            message = f"{size} disagrees with the target's {plugin[expression]}"
            lines.append(
                f'_Static_assert({plugin[expression]} == {plugin[size]}u, "{message}");'
            )
    source = work / "size_check.c"
    source.write_text("\n".join(lines) + "\n")
    obj = str(work / "size_check.o")
    run([tool("arm-none-eabi-gcc"), *TARGET_FLAGS, "-c", str(source), "-o", obj])


def link_image(plugin, work):
    """Compile and link the kernel into the bytes copied to SDRAM, and prove they need no address."""
    source = ROOT / plugin["source"]
    obj = work / (source.stem + ".o")
    elf = work / (source.stem + ".elf")
    image = work / (source.stem + ".bin")
    gcc = tool("arm-none-eabi-gcc")
    run([gcc, *TARGET_FLAGS, "-c", str(source), "-o", str(obj)])
    link = [
        f"-Wl,-T,{PLUGIN_DIR / 'plugin.ld'}",
        "-Wl,-e,0",
        "-Wl,--build-id=none",
        f"-Wl,-Map,{work / (source.stem + '.map')}",
    ]
    run([gcc, *TARGET_FLAGS, *link, str(obj), "-o", str(elf)])

    name = plugin["name"]
    undefined = run([tool("arm-none-eabi-nm"), "-u", str(elf)]).strip()
    if undefined:
        sys.exit(
            f"pack_dlp: {name} imports symbols, which a blob cannot resolve:\n{undefined}"
        )
    relocations = [
        line
        for line in run([tool("arm-none-eabi-readelf"), "-r", str(elf)]).splitlines()
        if re.match(r"^[0-9a-f]{8} ", line)
    ]
    if relocations:
        listing = "\n".join(relocations)
        sys.exit(f"pack_dlp: {name} still has relocations after the link:\n{listing}")

    run([tool("arm-none-eabi-objcopy"), "-O", "binary", str(elf), str(image)])
    # readelf, not nm: the Thumb bit lives in st_value (AAELF), and nm strips it for display.
    symbols = {}
    for line in run([tool("arm-none-eabi-readelf"), "-sW", str(elf)]).splitlines():
        fields = line.split()
        if len(fields) == 8 and fields[3] == "FUNC":
            symbols[fields[7]] = int(fields[1], 16)
    return image.read_bytes(), symbols


def entry_offsets(plugin, symbols, image_size):
    """Each entry point as the offset the host adds to the image base, Thumb bit and all."""
    name = plugin["name"]
    offsets = {}
    for role, symbol in plugin["entries"].items():
        if symbol is None:
            offsets[role] = None
            continue
        if symbol not in symbols:
            sys.exit(
                f"pack_dlp: {name}: entry point {role} ({symbol}) is not in the linked kernel"
            )
        offset = symbols[symbol]
        if not offset & 1:
            sys.exit(
                f"pack_dlp: {name}: {symbol} is not Thumb code (offset {offset:#x}); "
                "the host calls it with the Thumb bit"
            )
        if offset & ~1 >= image_size:
            sys.exit(f"pack_dlp: {name}: {symbol} lies outside the image")
        offsets[role] = offset
    return offsets


class Strings:
    """The blob's string table: every name once, addressed by its offset from the start of file."""

    def __init__(self, base):
        self.base = base
        self.data = bytearray()
        self.offsets = {}

    def add(self, text):
        if text is None:
            return 0  # offset 0 is the header's magic, so it is never a string: the ABI's NULL
        if text not in self.offsets:
            self.offsets[text] = self.base + len(self.data)
            self.data += text.encode("utf-8") + b"\0"
        return self.offsets[text]


def pack(plugin, image, entries):
    """Lay out the file: header, descriptor, tables, strings, then the image on a cache line."""
    header_size = struct.calcsize(HEADER_FORMAT)
    is_fx = plugin["kind"] == "fx"
    desc_size = struct.calcsize(FX_DESC_FORMAT if is_fx else SOURCE_DESC_FORMAT)
    table_offset = header_size + desc_size
    rows = plugin["params"] if is_fx else plugin["models"]
    row_size = struct.calcsize(FX_PARAM_FORMAT if is_fx else SOURCE_MODEL_FORMAT)
    strings = Strings(table_offset + row_size * len(rows))

    name_offset = strings.add(plugin["name"])
    if is_fx:
        table = b"".join(
            struct.pack(
                FX_PARAM_FORMAT,
                strings.add(param["name"]),
                strings.add(param["shortName"]),
                strings.add(param["fileName"]),
                param["defaultValue"],
            )
            for param in rows
        )
        desc = struct.pack(
            FX_DESC_FORMAT,
            len(rows),
            table_offset,
            plugin["stateSize"],
            entries["reset"],
            entries["isActive"] or 0,
            entries["render"],
        )
    else:
        table = b""
        for model in rows:
            macros = []
            for macro in model["macros"]:
                macros += [strings.add(macro["name"]), strings.add(macro["shortName"])]
            table += struct.pack(
                SOURCE_MODEL_FORMAT,
                strings.add(model["name"]["name"]),
                strings.add(model["name"]["shortName"]),
                strings.add(model["fileName"]),
                *macros,
            )
        desc = struct.pack(
            SOURCE_DESC_FORMAT,
            len(rows),
            table_offset,
            plugin["numMacros"],
            plugin["voiceStateSize"],
            plugin["scratchSize"],
            plugin["maxBlockSize"],
            entries["init"],
            entries["trigger"],
            entries["render"],
        )

    metadata = desc + table + bytes(strings.data)
    padding = -(header_size + len(metadata)) % IMAGE_ALIGN
    image_offset = header_size + len(metadata) + padding
    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        plugin["abiVersion"],
        KIND_FX if is_fx else KIND_SOURCE,
        image_offset + len(image),
        0,
        image_offset,
        len(image),
        header_size,
        desc_size,
        name_offset,
        TOOL_VERSION,
    )
    blob = bytearray(header + metadata + bytes(padding) + image)
    struct.pack_into("<I", blob, FIELD["crc32"], zlib.crc32(blob) & 0xFFFFFFFF)
    return bytes(blob)


def expected_descriptor(plugin, entries, image_size):
    """The descriptor the firmware must end up with: the built-in one, minus the offline-only bits."""
    common = {
        "kind": plugin["kind"],
        "name": plugin["name"],
        "abiVersion": plugin["abiVersion"],
        "imageSize": image_size,
        "entries": entries,
    }
    if plugin["kind"] == "fx":
        return {**common, "stateSize": plugin["stateSize"], "params": plugin["params"]}
    return {
        **common,
        "voiceStateSize": plugin["voiceStateSize"],
        "scratchSize": plugin["scratchSize"],
        "maxBlockSize": plugin["maxBlockSize"],
        "numMacros": plugin["numMacros"],
        "models": plugin["models"],
    }


def reader(work):
    """The offline half of the loader: read_dlp parses a .dlp with plugin_blob.h and prints it."""
    binary = work / "read_dlp"
    if not binary.exists():
        command = [native_compiler("c"), "-std=gnu23", "-O1", "-Wall"]
        command += ["-I", str(ROOT / "src" / "deluge"), str(TOOLS_DIR / "read_dlp.c")]
        run([*command, "-o", str(binary)])
    return binary


def verify(path, expected, work):
    """Parse the file back with the loader's own code and diff what it binds against the original."""
    actual = json.loads(run([str(reader(work)), str(path)]))
    if actual != expected:
        keys = sorted(set(expected) | set(actual))
        differences = ", ".join(k for k in keys if expected.get(k) != actual.get(k))
        sys.exit(
            f"pack_dlp: {path.name} does not read back as it was packed; "
            f"fields differ: {differences}\n"
            f"  packed: {json.dumps(expected, sort_keys=True)}\n"
            f"  read:   {json.dumps(actual, sort_keys=True)}"
        )


def restamped(blob, edits, truncate=0):
    """A copy of `blob` with header fields patched and a valid CRC, so a test reaches its own check."""
    data = bytearray(blob)
    for offset, value in edits:
        struct.pack_into("<I", data, offset, value)
    if truncate:
        del data[-truncate:]
    struct.pack_into("<I", data, FIELD["crc32"], 0)
    struct.pack_into("<I", data, FIELD["crc32"], zlib.crc32(data) & 0xFFFFFFFF)
    return bytes(data)


def corruptions(plugin, blob):
    """The ways a card can hand the Deluge a bad blob, and what plugin_blob.h must say about each."""
    header = struct.unpack(HEADER_FORMAT, blob[: struct.calcsize(HEADER_FORMAT)])
    image_size = header[7]
    desc_offset = header[8]
    render_entry = desc_offset + (20 if plugin["kind"] == "fx" else 32)
    flipped = bytes(blob[:-1]) + bytes([blob[-1] ^ 0xFF])
    return [
        ("a byte of the image flipped", flipped, "CRC mismatch"),
        (
            "not a plugin file",
            restamped(blob, [(FIELD["magic"], 0x21212121)]),
            "bad magic",
        ),
        (
            "a newer container format",
            restamped(blob, [(FIELD["formatVersion"], FORMAT_VERSION + 1)]),
            "unknown container format",
        ),
        (
            "built against another ABI",
            restamped(blob, [(FIELD["abiVersion"], plugin["abiVersion"] + 1)]),
            "different plugin ABI",
        ),
        (
            "an unknown kind",
            restamped(blob, [(FIELD["kind"], 99)]),
            "unknown plugin kind",
        ),
        ("truncated on the card", restamped(blob, [], truncate=1), "truncated"),
        (
            "the name pointing past the end",
            restamped(blob, [(FIELD["nameOffset"], len(blob) + 8)]),
            "outside the file",
        ),
        (
            "an entry point outside the image",
            restamped(blob, [(render_entry, image_size | 1)]),
            "entry point",
        ),
        (
            "an ARM (non-Thumb) entry point",
            restamped(blob, [(render_entry, 4)]),
            "entry point",
        ),
    ]


def self_test(plugin, blob, work, quiet):
    """A valid blob is not enough: a damaged one has to be refused, and by the right reason.

    Tier 2 falls back to the built-in and tells the user why, so each of these has to be
    distinguishable: a file truncated on the card, a plugin built against a newer firmware, or
    something that is not a plugin at all.
    """
    path = work / "corrupt.dlp"
    cases = corruptions(plugin, blob)
    for description, corrupt, expected in cases:
        path.write_bytes(corrupt)
        result = subprocess.run(
            [str(reader(work)), str(path)], capture_output=True, text=True, check=False
        )
        if result.returncode == 0 or expected not in result.stderr:
            sys.exit(
                f"pack_dlp: {plugin['name']}: a blob with {description} was not rejected as "
                f"{expected!r} (exit {result.returncode}: {result.stderr.strip()})"
            )
    if not quiet:
        kind, name = plugin["kind"], plugin["name"]
        print(
            f"     {kind:<6} {name:<8} rejects all {len(cases)} corruptions "
            "(CRC, magic, format, ABI, kind, truncation, offsets)"
        )


def build(plugin, out_path, work, quiet, run_self_test=False):
    check_target_sizes(plugin, work)
    image, symbols = link_image(plugin, work)
    entries = entry_offsets(plugin, symbols, len(image))
    blob = pack(plugin, image, entries)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    verify(out_path, expected_descriptor(plugin, entries, len(image)), work)
    if not quiet:
        if plugin["kind"] == "fx":
            detail = f"{len(plugin['params'])} params"
        else:
            detail = f"{len(plugin['models'])} models"
        kind, name = plugin["kind"], plugin["name"]
        print(
            f"OK   {kind:<6} {name:<8} {len(blob):5} bytes ({len(image)} image + "
            f"{len(blob) - len(image)} header/tables/strings), {detail}, "
            f"ABI v{plugin['abiVersion']}, reads back identical"
        )
    if run_self_test:
        self_test(plugin, blob, work, quiet)


def main():
    parser = argparse.ArgumentParser(
        description="Build a Deluge plugin kernel into a .dlp blob."
    )
    parser.add_argument(
        "plugins", nargs="*", help="plugin display names (default: every built-in)"
    )
    parser.add_argument("--all", action="store_true", help="every built-in plugin")
    parser.add_argument(
        "--out",
        default="build/plugins",
        help="output directory, or file name when packing a single plugin",
    )
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="also check that a corrupted blob is rejected, by name, for each reason",
    )
    parser.add_argument(
        "--print-flags",
        action="store_true",
        help="print the target build flags (check_freestanding.sh uses these)",
    )
    arguments = parser.parse_args()
    if arguments.print_flags:
        print(" ".join(TARGET_FLAGS))
        return

    work = Path(tempfile.mkdtemp(prefix="pack_dlp."))
    try:
        available = descriptors(work)
        if arguments.all or not arguments.plugins:
            wanted = available
        else:
            wanted = [p for p in available if p["name"] in arguments.plugins]
        missing = set(arguments.plugins) - {p["name"] for p in wanted}
        if missing:
            have = ", ".join(p["name"] for p in available)
            sys.exit(
                f"pack_dlp: no such built-in plugin: {', '.join(sorted(missing))} (have: {have})"
            )
        out = Path(arguments.out)
        single_file = len(wanted) == 1 and out.suffix == ".dlp"
        for plugin in wanted:
            path = out if single_file else out / (Path(plugin["source"]).stem + ".dlp")
            build(plugin, path, work, arguments.quiet, arguments.self_test)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
