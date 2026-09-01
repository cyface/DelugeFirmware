# gdb python probe for the sustain-pedal test suite.
# Prints one line: PROBE:{json} with the firmware's live sustain + voice state.
import json

import gdb


def vec_size(v):
    impl = v["_M_impl"]
    return int(impl["_M_finish"] - impl["_M_start"])


def vec_elem(v, i):
    return (v["_M_impl"]["_M_start"] + i).dereference()


out = {}

ph = gdb.parse_and_eval("playbackHandler")
out["held"] = int(ph["numHeldSustainNotes"])
held = []
for i in range(out["held"]):
    h = ph["heldSustainNotes"][i]
    held.append(
        {"ch": int(h["channel"]), "note": int(h["note"]), "vel": int(h["velocity"])}
    )
out["heldNotes"] = held

# Sustain pedal flags on the DIN cable (the emulator's UDP MIDI is the DIN port).
din = gdb.parse_and_eval("MIDIDeviceManager::dinMIDIPorts")
out["pedal"] = [int(din["inputChannels"][c]["sustainPedalDown"]) for c in range(16)]


# Total active synth voices across all sounds, plus which note codes sound.
def voice_ptr(val):
    """Unwrap ActiveVoice (unique_ptr<Voice, deleter>) to the raw Voice*."""
    t = val.type.strip_typedefs()
    if t.code == gdb.TYPE_CODE_PTR:
        tgt = t.target().strip_typedefs()
        if tgt.code != gdb.TYPE_CODE_FUNC and "Voice" in str(tgt):
            return val
        return None
    if t.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
        for f in t.fields():
            try:
                r = voice_ptr(val[f])
            except gdb.error:
                continue
            if r is not None:
                return r
    return None


sounds = gdb.parse_and_eval("AudioEngine::sounds")
n = vec_size(sounds)
total = 0
per = []
notes = []
for i in range(n):
    s = vec_elem(sounds, i)
    nv = vec_size(s["voices_"])
    total += nv
    per.append(nv)
    for j in range(nv):
        vp = voice_ptr(vec_elem(s["voices_"], j))
        if vp is not None:
            notes.append(int(vp.dereference()["noteCodeAfterArpeggiation"]))
out["voices"] = total
out["voicesPerSound"] = per
out["notes"] = notes

# The learnable SUSTAIN command binding (channelOrZone, noteOrCC).
try:
    sc = gdb.parse_and_eval(
        "midiEngine.globalMIDICommands[(int)GlobalMIDICommand::SUSTAIN]"
    )
    out["sustainCmd"] = [int(sc["channelOrZone"]), int(sc["noteOrCC"])]
except gdb.error:
    out["sustainCmd"] = None

# MIDI follow channel A (sanity that our routing config loaded).
try:
    mf = gdb.parse_and_eval("midiEngine.midiFollowChannelType[0].channelOrZone")
    out["followA"] = int(mf)
except gdb.error:
    out["followA"] = None

print("PROBE:" + json.dumps(out))
