/*
 * Copyright © 2026 Synthstrom Audible Ltd
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Tier 2: plugin kernels loaded from PLUGINS/ on the card.
 *
 * At boot, every PLUGINS/*.dlp is read into SDRAM, validated (plugin_blob.h), and - if it names a plugin the
 * firmware has built in - swapped in for that built-in: the file's descriptor becomes the one menus, XML and the
 * audio engine read, and its code runs from SDRAM (proven bit-exact and warm-invariant by the #37 spike).
 * A blob that is damaged, built against another ABI, or names a plugin this firmware does not have is skipped and
 * the built-in stays; a plugin can never be missing because of what is on the card.
 *
 * Replacing a built-in is all this does. Bringing a *new* FX or oscillator type - one with no built-in behind it -
 * needs the registry to become dynamic, which is step 4 of fork issue #41.
 */
#pragma once
#include <cstdint>
#include <span>

namespace deluge::plugin {

/// Why a file in PLUGINS/ is or is not running. Ordered roughly "the file is wrong" -> "the file is fine but this
/// firmware cannot use it".
enum class PluginLoadStatus : uint8_t {
	loaded = 0,   ///< bound and swapped in for the built-in of the same name
	unreadable,   ///< the card would not give us the bytes
	tooBig,       ///< larger than kMaxPluginFileBytes
	noMemory,     ///< no SDRAM to put it in
	notAPlugin,   ///< no "DLPB" magic: some other file that happens to end in .dlp
	badFormat,    ///< a container version this firmware does not know
	badAbi,       ///< built against a different plugin ABI (the handshake that keeps stale blobs out)
	damaged,      ///< CRC mismatch, or an offset or entry point that does not fit the file
	unknown,      ///< valid, but names no built-in plugin: nothing to replace (issue #41 step 4)
	incompatible, ///< names a built-in, but does not fit the host (param table, state size, block size...)
	duplicate,    ///< a second file claiming a plugin an earlier one already replaced
};

/// How far the loader got with a file. It is written into the record *before* each step, so if a plugin brings the
/// firmware down the last record says which file and which step - the breadcrumb a fault inside a blob needs.
enum class PluginLoadStage : uint8_t {
	none = 0,
	reading,    ///< pulling the bytes off the card
	validating, ///< checking the container (plugin_blob.h)
	binding,    ///< turning offsets into a descriptor
	revealing,  ///< cache maintenance that makes the image fetchable as code
	running,    ///< first call into the blob (the self-check)
	installed,  ///< handed to the host
};

/// One line of the boot scan's report, per file seen. Plain fixed-size data with no pointers into the blob, so it
/// stays readable from a debugger (or the emulator's `xp`) long after the scan, with no UI involved.
struct PluginLoadRecord {
	char file[24]; ///< file name in PLUGINS/, truncated
	char name[16]; ///< the plugin's own name, if the file got far enough to have one
	PluginLoadStatus status;
	PluginLoadStage stage; ///< the last step started; short of `installed` on the last record means it faulted there
	uint8_t kind;          ///< DELUGE_PLUGIN_BLOB_KIND_*, 0 if unknown
	uint8_t abiVersion;    ///< what the file was built against, 0 if unknown
	uint32_t fileSize;     ///< bytes on the card
	uint32_t imageSize;    ///< bytes of code and rodata
	uint32_t imageAddress; ///< where that code ended up in SDRAM, 0 if it never got there
	/// A fixed block rendered through the loaded kernel and through the built-in it replaced (plugin_host.h
	/// selfCheck), both 0 if it never ran. Equal means the blob is bit-identical to the built-in over that block -
	/// which is the point when the card is carrying a copy of a kernel the firmware already has.
	uint32_t checksum;
	uint32_t builtinChecksum;
};

/// The most one .dlp may be, and the most files the scan looks at (both generous: the two reference kernels are
/// 1.6 KB and 9.5 KB).
inline constexpr uint32_t kMaxPluginFileBytes = 64 * 1024;
inline constexpr uint32_t kMaxPluginFiles = 8;
/// Most models a loaded source plugin may declare (the built-in drum plugin has 6).
inline constexpr uint32_t kMaxLoadedSourceModels = 16;

/// Scan PLUGINS/ and install what it finds. Call once at boot, after the card is readable and before anything has
/// asked for a descriptor - no voice, FX chain or menu may exist yet.
void loadPluginsFromCard();

/// What the scan did, in the order the files were read.
std::span<const PluginLoadRecord> pluginLoadReport();

} // namespace deluge::plugin
