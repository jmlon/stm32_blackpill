#!/usr/bin/env python3
"""Preview a MIDI file on the PC with the AMY synthesizer, before converting it.

    python tools/midiplay.py song.mid [--wav song.wav] [--start 10] [--seconds 30]

Plays the song as tools/midi2h.py sees it: the same parser, so the same
notes, velocities (with channel volume folded in) and dropped drums.  Each
channel's General MIDI instrument is played with the closest of AMY's
built-in DX7 presets, and channel 10 with AMY's GM drum kit.  So this is a
preview of the song itself, its arrangement and how busy it is, not of the
2-operator FM patches the Black Pill will use (audio/midi).

The song is rendered offline, which takes a fraction of a second, then
normalized, written to a WAV file and played with the system's player
(paplay, aplay or ffplay on Linux; afplay on macOS).

AMY's Python module is compiled from the AMY sources; tools/install-amy-python.sh
installs it into a virtual environment, .venv, in the repository:

    tools/install-amy-python.sh
    .venv/bin/python tools/midiplay.py song.mid

AMY's Python build does not support Windows; there, convert with midi2h.py
and preview on the board.
"""

import argparse
import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import midi2h  # noqa: E402  (same directory)

try:
    import amy
    import numpy as np
except ImportError as e:
    sys.exit(f"{e}.  Install AMY's Python module first: tools/install-amy-python.sh, "
             "then run this with .venv/bin/python")

DRUM_CHANNEL = midi2h.DRUM_CHANNEL
DRUM_KIT_PATCH = 384  # AMY's first Gamma9001 General MIDI drum kit

# General MIDI program -> AMY DX7 preset (128 + the DX7 ROM number).  The
# names are the DX7's.
DX7 = {
    "BRASS 1": 128, "BRASS 2": 129, "BRASS 3": 130, "STRINGS 1": 131,
    "STRINGS 2": 132, "STRINGS 3": 133, "ORCHESTRA": 134, "PIANO 1": 135,
    "PIANO 2": 136, "E.PIANO 1": 138, "GUITAR 1": 139, "GUITAR 2": 140,
    "SYN-LEAD 1": 141, "BASS 1": 142, "BASS 2": 143, "E.ORGAN 1": 144,
    "PIPES 1": 145, "HARPSICH 1": 146, "CLAV 1": 147, "VIBE 1": 148,
    "MARIMBA": 149, "KOTO": 150, "FLUTE 1": 151, "TUB BELLS": 153,
    "STEEL DRUM": 154, "TIMPANI": 155, "VOICE 1": 157, "TRAIN": 158,
    "TAKE OFF": 159, "E.PIANO 2": 162, "CELESTE": 166, "TOY PIANO": 167,
    "E.ORGAN 2": 172, "ACCORDION": 180, "SITAR": 181, "GUITAR 3": 182,
    "GUITAR 4": 183, "BANJO": 187, "HARP 1": 188, "BASS 3": 190,
    "BASS 4": 191, "PICCOLO": 192, "FLUTE 2": 193, "OBOE": 194,
    "CLARINET": 195, "SAX BC": 196, "BASSOON": 197, "STRINGS 4": 198,
    "STRINGS 5": 199, "STRINGS 6": 200, "BRASS 4": 203, "BRASS 5": 204,
    "RECORDER": 208, "HARMONICA1": 209, "VOICE 2": 211, "VOICE 3": 212,
    "GLOKENSPL": 213, "XYLOPHONE": 215, "BELLS": 219, "COW BELL": 220,
    "BLOCK": 221, "LOG DRUM": 223, "SYN-LEAD 2": 224, "SYN-LEAD 3": 225,
    "SYN-LEAD 4": 226, "SYNBRASS 1": 232, "SYNBRASS 2": 233,
    "SYN-VOX": 236, "SYN-ORCH": 237, "SYN-BASS 1": 238, "SYN-BASS 2": 239,
    "CHIME-STRG": 244, "SHIMMER": 246, "EVOLUTION": 247, "WATER GDN": 248,
    "LASER GUN": 250, "EXPLOSION": 255,
}

GM_TO_DX7 = [
    # 0-7 pianos
    "PIANO 1", "PIANO 1", "PIANO 2", "PIANO 2", "E.PIANO 1", "E.PIANO 2",
    "HARPSICH 1", "CLAV 1",
    # 8-15 chromatic percussion
    "CELESTE", "GLOKENSPL", "TOY PIANO", "VIBE 1", "MARIMBA", "XYLOPHONE",
    "TUB BELLS", "KOTO",
    # 16-23 organs
    "E.ORGAN 1", "E.ORGAN 1", "E.ORGAN 1", "PIPES 1", "E.ORGAN 2",
    "ACCORDION", "HARMONICA1", "ACCORDION",
    # 24-31 guitars
    "GUITAR 1", "GUITAR 1", "GUITAR 2", "GUITAR 2", "GUITAR 2", "GUITAR 3",
    "GUITAR 3", "GUITAR 4",
    # 32-39 basses
    "BASS 1", "BASS 2", "BASS 2", "BASS 3", "BASS 4", "BASS 4",
    "SYN-BASS 1", "SYN-BASS 2",
    # 40-47 strings
    "STRINGS 1", "STRINGS 1", "STRINGS 1", "STRINGS 1", "STRINGS 2",
    "STRINGS 3", "HARP 1", "TIMPANI",
    # 48-55 ensembles
    "STRINGS 1", "STRINGS 2", "STRINGS 4", "STRINGS 5", "VOICE 1",
    "VOICE 2", "SYN-VOX", "ORCHESTRA",
    # 56-63 brass
    "BRASS 1", "BRASS 2", "BRASS 3", "BRASS 4", "BRASS 5", "BRASS 1",
    "SYNBRASS 1", "SYNBRASS 2",
    # 64-71 reeds
    "SAX BC", "SAX BC", "SAX BC", "SAX BC", "OBOE", "OBOE", "BASSOON",
    "CLARINET",
    # 72-79 pipes
    "PICCOLO", "FLUTE 1", "RECORDER", "FLUTE 2", "FLUTE 2", "FLUTE 2",
    "FLUTE 2", "FLUTE 2",
    # 80-87 synth leads
    "SYN-LEAD 1", "SYN-LEAD 2", "SYN-LEAD 3", "SYN-LEAD 4", "SYN-LEAD 1",
    "SYN-VOX", "SYN-LEAD 2", "SYN-LEAD 3",
    # 88-95 synth pads
    "SHIMMER", "STRINGS 5", "SYN-ORCH", "VOICE 3", "STRINGS 6",
    "CHIME-STRG", "WATER GDN", "EVOLUTION",
    # 96-103 synth effects
    "SHIMMER", "EVOLUTION", "BELLS", "WATER GDN", "SHIMMER", "EVOLUTION",
    "CHIME-STRG", "SYN-ORCH",
    # 104-111 ethnic
    "SITAR", "BANJO", "KOTO", "KOTO", "MARIMBA", "ACCORDION", "STRINGS 1",
    "OBOE",
    # 112-119 percussive
    "BELLS", "COW BELL", "STEEL DRUM", "BLOCK", "LOG DRUM", "LOG DRUM",
    "LOG DRUM", "EXPLOSION",
    # 120-127 sound effects
    "GUITAR 1", "FLUTE 2", "EXPLOSION", "SHIMMER", "BELLS", "TAKE OFF",
    "TRAIN", "LASER GUN",
]
assert len(GM_TO_DX7) == 128

# AMY runs out of oscillators beyond this (250 by default, minus a margin
# for program changes, which reallocate a channel's voices).  A DX7 voice
# takes 8; a voice of the GM drum kit holds the whole kit, 38.  AMY gives
# each voice a contiguous run of oscillators, so the drum kit is set up
# first, while the pool is still in one piece.
MAX_OSCS = 230
DX7_OSCS_PER_VOICE = 8
DRUM_KIT_OSCS_PER_VOICE = 38
MAX_DRUM_VOICES = 2  # hits of different drums overlap; more costs melodic voices


def voices_needed(events):
    """Most notes each channel holds at once, from the converted events."""
    held = {}
    peak = {}
    for _, channel, note, velocity in events:
        if channel & midi2h.SONG_PROGRAM:
            continue
        keys = held.setdefault(channel, set())
        if velocity:
            keys.add(note)
            peak[channel] = max(peak.get(channel, 0), len(keys))
        else:
            keys.discard(note)
    return peak


def plan_voices(peaks):
    """Voices per channel: a melodic channel's peak plus one for release
    tails, the drums' peak (up to MAX_DRUM_VOICES), trimmed from the busiest
    melodic channels if they would not fit in AMY."""
    voices = {ch: min(p + 1, 8) for ch, p in peaks.items() if ch != DRUM_CHANNEL}
    if DRUM_CHANNEL in peaks:
        voices[DRUM_CHANNEL] = min(peaks[DRUM_CHANNEL], MAX_DRUM_VOICES)
    budget = MAX_OSCS - voices.get(DRUM_CHANNEL, 0) * DRUM_KIT_OSCS_PER_VOICE
    melodic = {ch: n for ch, n in voices.items() if ch != DRUM_CHANNEL}
    while sum(melodic.values()) * DX7_OSCS_PER_VOICE > budget:
        busiest = max(melodic, key=melodic.get)
        if melodic[busiest] == 1:
            break
        melodic[busiest] -= 1
    melodic.update({ch: n for ch, n in voices.items() if ch == DRUM_CHANNEL})
    return melodic


@contextlib.contextmanager
def quiet_c_stderr():
    """Hides AMY's own diagnostics (printed by its C code to stderr, e.g.
    "note off for 0/49 does not match note on" for drum note-offs, which
    GM drums ignore anyway) while leaving Python's errors alone."""
    sys.stderr.flush()
    saved = os.dup(2)
    devnull = os.open(os.devnull, os.O_WRONLY)
    os.dup2(devnull, 2)
    try:
        yield
    finally:
        os.dup2(saved, 2)
        os.close(devnull)
        os.close(saved)


def render(song, start_s, seconds):
    events = song["events"]
    peaks = voices_needed(events)
    voices = plan_voices(peaks)

    amy.restart(0)  # a fresh AMY without its default synths

    def set_program(channel, program):
        amy.send(synth=channel + 1, num_voices=voices.get(channel, 2),
                 patch=DX7[GM_TO_DX7[program]])

    if DRUM_CHANNEL in voices:
        amy.send(synth=DRUM_CHANNEL + 1, num_voices=voices[DRUM_CHANNEL],
                 patch=DRUM_KIT_PATCH)
    for channel in voices:
        if channel != DRUM_CHANNEL:
            set_program(channel, 0)

    rate = amy.AMY_SAMPLE_RATE
    block = amy.AMY_BLOCK_SIZE
    start_block = int(start_s * rate / block)
    end_ms = song["length_ms"] + 3000  # let the last notes ring
    if seconds:
        end_ms = min(end_ms, (start_s + seconds) * 1000)
    end_block = int(end_ms / 1000 * rate / block)

    out = []
    rendered = 0
    time_ms = 0
    events = iter(events)
    pending = next(events, None)
    while rendered < end_block:
        # Apply every event due by the start of this block.
        while pending is not None:
            due_ms = time_ms + pending[0]
            if due_ms * rate / 1000 > rendered * block:
                break
            time_ms = due_ms
            _, channel, note, velocity = pending
            if channel == midi2h.SONG_NOP:
                pass
            elif channel & midi2h.SONG_PROGRAM:
                channel &= 0x0F
                if channel != DRUM_CHANNEL:
                    set_program(channel, note)
            else:
                amy.send(synth=channel + 1, note=note, vel=velocity / 127)
            pending = next(events, None)
        samples = amy._amy.render_to_list()
        if rendered >= start_block:
            out.append(samples)
        rendered += 1

    audio = np.array(out, dtype=np.float32).reshape(-1, amy.AMY_NCHANS)
    return audio, rate, voices


def write_wav(path, audio, rate):
    peak = float(np.abs(audio).max()) or 1.0
    scaled = (audio * (0.89 * 32767 / peak)).astype(np.int16)  # -1 dBFS
    with wave.open(path, "wb") as w:
        w.setnchannels(audio.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(scaled.tobytes())


def play(path):
    if sys.platform == "win32":
        import winsound
        winsound.PlaySound(path, winsound.SND_FILENAME)
        return
    players = [["afplay"]] if sys.platform == "darwin" else [
        ["paplay"], ["aplay", "-q"], ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"]]
    for player in players:
        if shutil.which(player[0]):
            subprocess.run(player + [path], check=False)
            return
    print(f"no audio player found; open {path} yourself")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("midi", help="Standard MIDI File (.mid)")
    parser.add_argument("--wav", help="keep the rendered audio in this WAV file")
    parser.add_argument("--start", type=float, default=0, help="start at this second")
    parser.add_argument("--seconds", type=float, help="play only this many seconds")
    parser.add_argument("--no-play", action="store_true", help="only render (with --wav)")
    args = parser.parse_args()

    try:
        song = midi2h.convert(args.midi)
    except (OSError, ValueError, IndexError) as e:
        sys.exit(f"{args.midi}: {e}")

    with quiet_c_stderr():
        audio, rate, voices = render(song, args.start, args.seconds)

    print(f"{song['title']}: {song['length_ms'] / 1000:.1f} s, "
          f"at most {song['polyphony']} notes at once "
          f"(the Black Pill plays 8 melodic voices)")
    for channel in sorted(song["notes"]):
        if channel == DRUM_CHANNEL:
            print(f"  channel {channel + 1:2d}: drums -> AMY GM drum kit, "
                  f"{voices.get(channel)} voices")
            continue
        program = song["programs"].get(channel, 0)
        print(f"  channel {channel + 1:2d}: {midi2h.GM_PROGRAMS[program]}"
              f" -> DX7 {GM_TO_DX7[program]}, {voices.get(channel)} voices")
    for text, count in song["warnings"].items():
        print(f"  warning: {text} ({count}x)")
    if song["dropped_drums"]:
        notes = ", ".join(f"{n} ({c}x)" for n, c in sorted(song["dropped_drums"].items()))
        print(f"  warning: unsupported drum notes dropped: {notes}")

    path = args.wav
    if not path:
        handle, path = tempfile.mkstemp(suffix=".wav")
        os.close(handle)
    write_wav(path, audio, rate)
    if args.wav:
        print(f"wrote {path}")
    try:
        if not args.no_play:
            print(f"playing {len(audio) / rate:.1f} s (Ctrl+C to stop)")
            play(path)
    except KeyboardInterrupt:
        pass
    finally:
        if not args.wav:
            os.remove(path)


if __name__ == "__main__":
    main()
