#pragma once
// #37 plugin-architecture spike: load a position-independent plugin blob into SDRAM at runtime
// (from an embedded copy and from PLUGINS/spike.dlp on the card), run it against the same kernel
// compiled into the firmware, and report bit-exactness plus cold/warm cycles per 128-sample block
// over the sysex debug console. See contrib/plugin_spike/ and docs/dev/plugin_spike.md.
void pluginSpikeRequest();
void pluginSpikeRoutine();

// Boot-time facts captured by deluge_main() and reported on console attach (no SD writes needed).
struct BootLog {
	unsigned haveOled;
	unsigned picFirmwareVersion;
	unsigned picSaysOledPresent;
	unsigned oledDcAcks; // PIC responses 246..251 seen during the boot handshake
	unsigned breaks;
	unsigned bootResponses;
	unsigned otherResponses;
};
extern BootLog bootLog;
