#!/usr/bin/env python3
"""Preview a MIDI file on the PC, before converting it for audio/midi.

    python3 tools/midiplay.py song.mid [--board] [--wav song.wav]
                                       [--start 10] [--seconds 30]

Both modes play the song as tools/midi2h.py sees it: the same parser, so the
same notes, velocities (with channel volume folded in) and dropped drums.

By default the song is played with the AMY synthesizer: each channel's
General MIDI instrument with the closest of AMY's built-in DX7 presets, and
channel 10 with AMY's GM drum kit.  This previews the song itself, its
arrangement and how busy it is, with richer sounds than the board makes.
AMY's Python module is compiled from the AMY sources;
tools/install-amy-python.sh installs it into .venv in the repository:

    tools/install-amy-python.sh
    .venv/bin/python tools/midiplay.py song.mid

With --board, the song is rendered exactly as the Black Pill plays it: the
audio/midi sketch's own synthesis code (the part between its "portable
synthesis" markers) is compiled on the PC together with LEAF, with the same
patches, drums, 8 voices, voice stealing, sample rate and output level.
It needs a C/C++ compiler (gcc and g++, or $CC and $CXX) and LEAF
(tools/install-leaf.sh), but not AMY; LEAF is compiled once and cached in
build/midiplay-board.  The output is not normalized, so loudness and any
clipping are the board's.

Either way the song is rendered offline, written to a WAV file and played
with the system's player (paplay, aplay or ffplay on Linux; afplay on
macOS).  AMY's Python build does not support Windows; --board might, with
MinGW, but is untested there.
"""

import argparse
import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
import wave

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import midi2h  # noqa: E402  (same directory)

amy = None
np = None


def load_amy():
    """Imports AMY and numpy, which only the AMY mode needs."""
    global amy, np
    try:
        import amy as amy_module
        import numpy as numpy_module
    except ImportError as e:
        sys.exit(f"{e}.  Install AMY's Python module first: tools/install-amy-python.sh, "
                 "then run this with .venv/bin/python (or use --board)")
    amy, np = amy_module, numpy_module

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


def render_amy(song, start_s, seconds):
    """Returns (16-bit PCM bytes, sample rate, channels, voices per channel)."""
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
    peak = float(np.abs(audio).max()) or 1.0
    pcm = (audio * (0.89 * 32767 / peak)).astype(np.int16)  # normalize to -1 dBFS
    return pcm.tobytes(), rate, amy.AMY_NCHANS, voices


# ---------------------------------------------------------------------------
# --board: the audio/midi sketch's own synthesis, compiled on the PC

SKETCH = os.path.join(REPO, "audio", "midi", "midi.ino")
BOARD_BUILD = os.path.join(REPO, "build", "midiplay-board")
LEAF_SRC = os.environ.get("LEAF_SRC",
                          os.path.expanduser("~/Arduino/libraries/LEAF/src"))
PORTABLE_BEGIN = "// >>> portable synthesis"
PORTABLE_END = "// <<< portable synthesis"

# Stands in for the sketch's setup() and loop(): renders the song from the
# start, writes the frames from start_s on, and prints what the board's
# serial report would.  KEY is never pressed, so it stops after one pass.
RENDERER = r"""
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "leaf.h"
#include "song.h"

#ifndef PI
#define PI 3.14159265358979f
#endif
#define USER_BTN 0
#define LOW 0
#define HIGH 1
static int digitalRead(int) { return HIGH; }

namespace {
constexpr uint32_t SAMPLE_RATE = BOARD_SAMPLE_RATE;
constexpr uint32_t SAMPLES_PER_MS = SAMPLE_RATE / 1000;
constexpr uint32_t BLOCK_FRAMES = BOARD_BLOCK_FRAMES;
#include "portable.inc"
}  // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s out.raw start_s seconds\n", argv[0]);
    return 2;
  }
  FILE *out = fopen(argv[1], "wb");
  if (!out) {
    perror(argv[1]);
    return 1;
  }
  const uint64_t startFrame = (uint64_t)(atof(argv[2]) * SAMPLE_RATE);
  const double seconds = atof(argv[3]);
  const uint64_t lastFrame =
      seconds > 0 ? startFrame + (uint64_t)(seconds * SAMPLE_RATE)
                  : (uint64_t)(SONG_LENGTH_MS / 1000 + 60) * SAMPLE_RATE;

  LEAF_init(&leaf, SAMPLE_RATE, leafMemory, sizeof(leafMemory), randomNumber);
  for (Voice &v : voices) v.init();
  for (DrumVoice &d : drums) d.init();
  startSong();

  int16_t block[BLOCK_FRAMES * 2];
  uint64_t frame = 0;
  uint32_t clipped = 0;
  unsigned peak = 0;
  while (playing && frame < lastFrame) {
    renderBlock(block);
    if (frame + BLOCK_FRAMES > startFrame) {
      fwrite(block, sizeof(block[0]), BLOCK_FRAMES * 2, out);
    }
    frame += BLOCK_FRAMES;
    clipped += clippedSamples;
    clippedSamples = 0;
    if (peakVoicesInUse > peak) peak = peakVoicesInUse;
    peakVoicesInUse = 0;
  }
  fclose(out);
  printf("rate %u\nstolen %u\nclipped %u\npeak_voices %u\nvoices %u\npool %u %u\n",
         (unsigned)SAMPLE_RATE, (unsigned)stolenVoices, (unsigned)clipped, peak,
         (unsigned)MELODIC_VOICES, (unsigned)leaf_pool_get_used(&leaf),
         (unsigned)sizeof(leafMemory));
  return 0;
}
"""


def sketch_parameters(text):
    """The sketch's sample rate (from its I2S clock settings) and block size."""
    import re

    def const(name):
        m = re.search(rf"constexpr uint32_t {name} = (\d+);", text)
        if not m:
            sys.exit(f"{SKETCH}: cannot find {name}")
        return int(m.group(1))

    m = re.search(r"I2S_CLK_HZ = (\d+) / PLLI2S_M", text)
    if not m:
        sys.exit(f"{SKETCH}: cannot find I2S_CLK_HZ")
    i2s_clock = int(m.group(1)) // const("PLLI2S_M") * const("PLLI2S_N") // const("PLLI2S_R")
    rate = i2s_clock // (32 * (2 * const("I2S_DIV") + const("I2S_ODD")))
    return rate, const("BLOCK_FRAMES")


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode:
        sys.exit(f"{' '.join(cmd[:2])}... failed:\n{result.stderr[-3000:]}")
    return result.stdout


def leaf_objects(cc):
    """Compiles LEAF's C sources once; recompiles when they change."""
    sources = sorted(
        os.path.join(d, f)
        for d in (os.path.join(LEAF_SRC, "Src"), os.path.join(LEAF_SRC, "Externals"))
        if os.path.isdir(d) for f in os.listdir(d) if f.endswith(".c"))
    if not sources:
        sys.exit(f"LEAF not found in {LEAF_SRC}: run tools/install-leaf.sh "
                 "(or set LEAF_SRC)")
    obj_dir = os.path.join(BOARD_BUILD, "leaf")
    os.makedirs(obj_dir, exist_ok=True)
    stamp = f"{LEAF_SRC}\n{max(os.path.getmtime(s) for s in sources)}\n{cc}\n"
    stamp_path = os.path.join(obj_dir, "stamp")
    objects = [os.path.join(obj_dir, os.path.basename(s)[:-2] + ".o") for s in sources]
    if (os.path.exists(stamp_path) and open(stamp_path).read() == stamp
            and all(os.path.exists(o) for o in objects)):
        return objects
    print(f"compiling LEAF ({len(sources)} files, once)...")
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor() as pool:
        list(pool.map(lambda so: run([cc, "-O2", "-w", "-I", LEAF_SRC, "-c", so[0], "-o", so[1]]),
                      zip(sources, objects)))
    with open(stamp_path, "w") as f:
        f.write(stamp)
    return objects


def render_board(song, midi_path, start_s, seconds):
    """Returns (16-bit PCM bytes, sample rate, channels, report)."""
    cc = os.environ.get("CC", "gcc")
    cxx = os.environ.get("CXX", "g++")
    for tool in (cc, cxx):
        if not shutil.which(tool):
            sys.exit(f"--board needs a C/C++ compiler: {tool} not found (set CC/CXX)")

    text = open(SKETCH).read()
    if text.count(PORTABLE_BEGIN) != 1 or text.count(PORTABLE_END) != 1:
        sys.exit(f"{SKETCH}: expected one '{PORTABLE_BEGIN}' and one '{PORTABLE_END}' line")
    portable = text[text.index(PORTABLE_BEGIN):text.index(PORTABLE_END)]
    rate, block = sketch_parameters(text)

    objects = leaf_objects(cc)
    os.makedirs(BOARD_BUILD, exist_ok=True)
    midi2h.write_header(song, midi_path, os.path.join(BOARD_BUILD, "song.h"))
    with open(os.path.join(BOARD_BUILD, "portable.inc"), "w") as f:
        f.write(portable)
    renderer_cpp = os.path.join(BOARD_BUILD, "renderer.cpp")
    with open(renderer_cpp, "w") as f:
        f.write(RENDERER)
    exe = os.path.join(BOARD_BUILD, "renderer.exe" if sys.platform == "win32" else "renderer")
    run([cxx, "-O2", "-w", f"-DBOARD_SAMPLE_RATE={rate}", f"-DBOARD_BLOCK_FRAMES={block}",
         "-I", LEAF_SRC, "-I", BOARD_BUILD, renderer_cpp, *objects, "-lm", "-o", exe])

    raw = os.path.join(BOARD_BUILD, "out.raw")
    report = {}
    for line in run([exe, raw, str(start_s), str(seconds or 0)]).splitlines():
        key, *values = line.split()
        report[key] = [int(v) for v in values]
    with open(raw, "rb") as f:
        pcm = f.read()
    os.remove(raw)
    return pcm, report["rate"][0], 2, report


def write_wav(path, pcm, rate, channels):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


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
    parser.add_argument("--board", action="store_true",
                        help="render with the audio/midi sketch's own synthesis")
    parser.add_argument("--wav", help="keep the rendered audio in this WAV file")
    parser.add_argument("--start", type=float, default=0, help="start at this second")
    parser.add_argument("--seconds", type=float, help="play only this many seconds")
    parser.add_argument("--no-play", action="store_true", help="only render (with --wav)")
    args = parser.parse_args()

    try:
        song = midi2h.convert(args.midi)
    except (OSError, ValueError, IndexError) as e:
        sys.exit(f"{args.midi}: {e}")

    print(f"{song['title']}: {song['length_ms'] / 1000:.1f} s, "
          f"at most {song['polyphony']} notes at once "
          f"(the Black Pill plays 8 melodic voices)")
    if args.board:
        pcm, rate, channels, report = render_board(song, args.midi, args.start, args.seconds)
        for channel in sorted(song["notes"]):
            name = ("drums" if channel == DRUM_CHANNEL
                    else midi2h.GM_PROGRAMS[song["programs"].get(channel, 0)])
            print(f"  channel {channel + 1:2d}: {name}")
        print(f"  board: {rate} Hz, {report['peak_voices'][0]} of {report['voices'][0]} "
              f"voices in use at most, {report['stolen'][0]} notes stolen, "
              f"{report['clipped'][0]} samples clipped, "
              f"LEAF pool {report['pool'][0]} of {report['pool'][1]} bytes")
    else:
        load_amy()
        with quiet_c_stderr():
            pcm, rate, channels, voices = render_amy(song, args.start, args.seconds)
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
    write_wav(path, pcm, rate, channels)
    if args.wav:
        print(f"wrote {path}")
    try:
        if not args.no_play:
            print(f"playing {len(pcm) / (2 * channels * rate):.1f} s (Ctrl+C to stop)")
            play(path)
    except KeyboardInterrupt:
        pass
    finally:
        if not args.wav:
            os.remove(path)


if __name__ == "__main__":
    main()
