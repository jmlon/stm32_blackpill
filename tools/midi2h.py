#!/usr/bin/env python3
"""Convert a Standard MIDI File into a C header for audio/midi.

    python3 tools/midi2h.py song.mid [-o audio/midi/song.h]

Needs only the Python standard library, so it runs as is on Linux, macOS or
Windows ("py tools\\midi2h.py song.mid").

The header holds the song as one flat, time-ordered event list, so the
sketch needs no MIDI parser:

    struct SongEvent {
      uint16_t deltaMs;  // time since the previous event
      uint8_t channel;   // 0-15; SONG_PROGRAM (0x80) set: a program change;
                         // SONG_NOP (0xFF): only a time step
      uint8_t note;      // MIDI note, or the program number
      uint8_t velocity;  // 1-127 note-on, 0 note-off
    };

What the conversion does:
- merges all tracks (format 0 and 1) and turns ticks into milliseconds with
  the file's tempo map;
- folds each channel's volume (CC7) and expression (CC11) into the note-on
  velocities, since the synth has no per-channel gain;
- keeps program changes, and lists each channel's instrument;
- makes overlapping notes of one key well-formed: a repeated note-on
  retriggers the key, and it is released by the last of its note-offs;
- drops everything else (pan, pitch bend, sustain pedal, sysex, meta
  events), printing a warning for what might change how the song sounds.
"""

import argparse
import collections
import os
import struct
import sys

SONG_PROGRAM = 0x80
SONG_NOP = 0xFF
DRUM_CHANNEL = 9

# GM drum notes the sketch plays (kicks, snares, clap, hi-hats, toms,
# cymbals); anything else on channel 10 is dropped with a warning.
SUPPORTED_DRUMS = {35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
                   49, 50, 51, 52, 55, 57, 59}

GM_PROGRAMS = [
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano",
    "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord",
    "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
    "Marimba", "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ",
    "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ",
    "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)",
    "Acoustic Guitar (steel)", "Electric Guitar (jazz)",
    "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar",
    "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass",
    "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass",
    "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin",
    "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings",
    "Orchestral Harp", "Timpani", "String Ensemble 1", "String Ensemble 2",
    "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs",
    "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba",
    "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1",
    "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
    "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute",
    "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle",
    "Ocarina", "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
    "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)",
    "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)",
    "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)",
    "Pad 7 (halo)", "Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)",
    "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)",
    "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo",
    "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum",
    "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise",
    "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter",
    "Applause", "Gunshot",
]


def read_vlq(data, i):
    value = 0
    while True:
        byte = data[i]
        i += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, i


def read_chunks(data):
    i = 0
    while i + 8 <= len(data):
        tag = data[i:i + 4]
        (length,) = struct.unpack(">I", data[i + 4:i + 8])
        yield tag, data[i + 8:i + 8 + length]
        i += 8 + length


def parse_track(track, index):
    """Yields (tick, order, track, kind, args) for one MTrk chunk."""
    i = tick = order = 0
    status = 0
    while i < len(track):
        delta, i = read_vlq(track, i)
        tick += delta
        byte = track[i]
        if byte == 0xFF:  # meta event
            kind = track[i + 1]
            length, i = read_vlq(track, i + 2)
            payload = track[i:i + length]
            i += length
            if kind == 0x51:
                yield tick, order, index, "tempo", (int.from_bytes(payload, "big"),)
            elif kind == 0x03 and payload:
                yield tick, order, index, "name", (payload.decode("latin-1"),)
            elif kind == 0x2F:
                yield tick, order, index, "end", ()
            order += 1
            continue
        if byte in (0xF0, 0xF7):  # sysex
            length, i = read_vlq(track, i + 1)
            i += length
            yield tick, order, index, "sysex", ()
            order += 1
            continue
        if byte & 0x80:
            status = byte
            i += 1
        elif not status:
            raise ValueError(f"track {index}: data byte without a status byte")
        kind, channel = status & 0xF0, status & 0x0F
        size = 1 if kind in (0xC0, 0xD0) else 2
        a = track[i:i + size]
        i += size
        if kind == 0x90 and a[1] > 0:
            yield tick, order, index, "on", (channel, a[0], a[1])
        elif kind == 0x80 or kind == 0x90:
            yield tick, order, index, "off", (channel, a[0])
        elif kind == 0xB0:
            yield tick, order, index, "cc", (channel, a[0], a[1])
        elif kind == 0xC0:
            yield tick, order, index, "program", (channel, a[0])
        elif kind == 0xE0:
            yield tick, order, index, "bend", (channel, a[0] | a[1] << 7)
        elif kind == 0xD0 or kind == 0xA0:
            yield tick, order, index, "pressure", (channel,)
        order += 1


def convert(path):
    data = open(path, "rb").read()
    chunks = list(read_chunks(data))
    if not chunks or chunks[0][0] != b"MThd":
        raise ValueError("not a Standard MIDI File (no MThd header)")
    fmt, _, division = struct.unpack(">HHH", chunks[0][1][:6])
    if fmt == 2:
        raise ValueError("format 2 (independent sequences) is not supported")
    if division & 0x8000:
        raise ValueError("SMPTE time division is not supported")

    raw = []
    for index, (tag, body) in enumerate(c for c in chunks[1:] if c[0] == b"MTrk"):
        raw.extend(parse_track(body, index))
    # Same tick: note-offs first, so a note that ends where another starts
    # does not cut it; then file order.
    raw.sort(key=lambda e: (e[0], e[3] != "off", e[2], e[1]))

    title = next((e[4][0] for e in raw if e[3] == "name" and e[2] == 0), None)
    title = title or os.path.splitext(os.path.basename(path))[0]

    # Tempo map: ticks -> milliseconds.
    tempo = 500000  # 120 BPM until the first tempo event
    last_tick = 0
    last_us = 0.0
    tempos = set()

    def to_ms(tick):
        return (last_us + (tick - last_tick) * tempo / division) / 1000.0

    volume = [127] * 16
    expression = [127] * 16
    held = collections.Counter()  # (channel, note) -> note-ons not yet off
    programs = {}
    notes_per_channel = collections.Counter()
    warnings = collections.Counter()
    dropped_drums = collections.Counter()
    events = []  # (ms, channel, note, velocity)

    for tick, _, _, kind, args in raw:
        ms = to_ms(tick)
        if kind == "tempo":
            last_us, last_tick, tempo = ms * 1000.0, tick, args[0]
            tempos.add(round(60e6 / tempo, 2))
        elif kind == "program":
            channel, program = args
            programs.setdefault(channel, program)
            events.append((ms, channel | SONG_PROGRAM, program, 0))
        elif kind == "cc":
            channel, number, value = args
            if number == 7:
                volume[channel] = value
            elif number == 11:
                expression[channel] = value
            elif number == 64 and value >= 64:
                warnings["sustain pedal (CC64) ignored"] += 1
            elif number in (1, 2, 4, 5, 65, 66, 67, 71, 74, 91, 93):
                warnings[f"controller CC{number} ignored"] += 1
        elif kind == "on":
            channel, note, velocity = args
            if channel == DRUM_CHANNEL and note not in SUPPORTED_DRUMS:
                dropped_drums[note] += 1
                continue
            scaled = velocity * volume[channel] * expression[channel]
            velocity = max(1, round(scaled / (127 * 127)))
            key = (channel, note)
            if held[key]:
                events.append((ms, channel, note, 0))  # retrigger
            held[key] += 1
            notes_per_channel[channel] += 1
            events.append((ms, channel, note, velocity))
        elif kind == "off":
            channel, note = args
            key = (channel, note)
            if held[key] == 0:
                continue  # dropped drum, or a stray note-off
            held[key] -= 1
            if held[key] == 0:
                events.append((ms, channel, note, 0))
        elif kind == "bend":
            if args[1] != 8192:
                warnings["pitch bend ignored"] += 1
        elif kind == "pressure":
            warnings["aftertouch ignored"] += 1
        elif kind == "sysex":
            warnings["sysex ignored"] += 1

    end_ms = to_ms(max((e[0] for e in raw), default=0))
    for (channel, note), count in held.items():
        if count:
            events.append((end_ms, channel, note, 0))
            warnings["note without note-off (released at the end)"] += 1

    # Round absolute times, then take differences, so rounding never
    # accumulates into drift.  Gaps over 65.5 s get no-op spacer events.
    out = []
    previous = 0
    for ms, channel, note, velocity in events:
        now = round(ms)
        delta = now - previous
        while delta > 0xFFFF:
            out.append((0xFFFF, SONG_NOP, 0, 0))
            delta -= 0xFFFF
        out.append((delta, channel, note, velocity))
        previous = now

    return {
        "title": title,
        "events": out,
        "length_ms": round(end_ms),
        "programs": programs,
        "notes": notes_per_channel,
        "tempos": sorted(tempos) or [120.0],
        "warnings": warnings,
        "dropped_drums": dropped_drums,
        "polyphony": peak_polyphony(out),
    }


def peak_polyphony(events):
    held = set()
    peak = 0
    for _, channel, note, velocity in events:
        if channel & SONG_PROGRAM:  # program change or no-op
            continue
        if velocity:
            held.add((channel, note))
            peak = max(peak, len(held))
        else:
            held.discard((channel, note))
    return peak


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_header(song, source, out):
    events = song["events"]
    lines = [
        f"// Generated by tools/midi2h.py from {os.path.basename(source)}; do not edit.",
        "// Regenerate with:  python3 tools/midi2h.py <file.mid> -o audio/midi/song.h",
        "//",
        f"// {song['title']}",
        f"// {song['length_ms'] / 1000:.1f} s, {len(events)} events, "
        f"tempo {', '.join(f'{t:g}' for t in song['tempos'])} BPM, "
        f"at most {song['polyphony']} notes at once.",
    ]
    for channel in sorted(set(song["notes"]) | set(song["programs"])):
        if channel == DRUM_CHANNEL:
            name = "drums"
        else:
            name = GM_PROGRAMS[song["programs"].get(channel, 0)]
        lines.append(f"//   channel {channel + 1:2d}: {name}, {song['notes'][channel]} notes")
    lines += [
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "struct SongEvent {",
        "  uint16_t deltaMs;  // time since the previous event",
        "  uint8_t channel;   // 0-15; SONG_PROGRAM set: a program change;",
        "                     // SONG_NOP: only a time step",
        "  uint8_t note;      // MIDI note, or the program number",
        "  uint8_t velocity;  // 1-127 note-on, 0 note-off",
        "};",
        "",
        f"constexpr uint8_t SONG_PROGRAM = 0x{SONG_PROGRAM:02X};",
        f"constexpr uint8_t SONG_NOP = 0x{SONG_NOP:02X};",
        f"constexpr char SONG_TITLE[] = {c_string(song['title'])};",
        f"constexpr uint32_t SONG_LENGTH_MS = {song['length_ms']};",
        "",
        "const SongEvent SONG_EVENTS[] = {",
    ]
    row = []
    for delta, channel, note, velocity in events:
        row.append(f"{{{delta}, {channel}, {note}, {velocity}}}")
        if len(row) == 6:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines += [
        "};",
        "constexpr uint32_t SONG_EVENT_COUNT = sizeof(SONG_EVENTS) / sizeof(SONG_EVENTS[0]);",
        "",
    ]
    with open(out, "w", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("midi", help="Standard MIDI File (.mid)")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("-o", "--output",
                        default=os.path.join(here, "..", "audio", "midi", "song.h"),
                        help="header to write (default: audio/midi/song.h)")
    args = parser.parse_args()

    try:
        song = convert(args.midi)
    except (OSError, ValueError, IndexError) as e:
        sys.exit(f"{args.midi}: {e}")
    write_header(song, args.midi, args.output)

    print(f"{song['title']}: {song['length_ms'] / 1000:.1f} s, "
          f"{len(song['events'])} events ({len(song['events']) * 6} bytes of flash), "
          f"at most {song['polyphony']} notes at once")
    for channel in sorted(song["notes"]):
        name = "drums" if channel == DRUM_CHANNEL else GM_PROGRAMS[song["programs"].get(channel, 0)]
        print(f"  channel {channel + 1:2d}: {name}, {song['notes'][channel]} notes")
    for text, count in song["warnings"].items():
        print(f"  warning: {text} ({count}x)")
    if song["dropped_drums"]:
        notes = ", ".join(f"{n} ({c}x)" for n, c in sorted(song["dropped_drums"].items()))
        print(f"  warning: unsupported drum notes dropped: {notes}")
    print(f"wrote {os.path.normpath(args.output)}")


if __name__ == "__main__":
    main()
