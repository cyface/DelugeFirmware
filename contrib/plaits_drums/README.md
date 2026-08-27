# Plaits drum-model kits

Example kits for the `Drum` oscillator type (the Plaits analog drum models). `make_kits.py`
generates `KITS/808 Models.XML` and `KITS/909 Models.XML`; copy them into the `KITS` folder of
the SD card. Every row is a synth drum - no samples are needed.

Enable **Drum Models** in the community features menu to change a row's model or macros
(Tone / Decay / Drive-Snappy-Noise, in the oscillator 1 menu); the kits load and play either way.

Rows, bottom to top: kick, long/punchy kick, snare, snappy snare, three toms (the snare model
with the noise turned off), closed and open hi-hat (choked against each other), and a long
cymbal / ride. Edit the `KIT_808` / `KIT_909` tables in the script to re-tune them.
