# Audio

Sketches and notes on audio synthesis for the Black Pill:

- `FM_Synthesis.md`: FM synthesis background, with the `fmsynth_*` demos.
- `MIDI libraries.md`: the synthesis libraries compared, for the MIDI player.
- `midi/`: the MIDI player sketch, and `song.h`, the song it plays.

## Python tools

Two helper scripts in `../tools` prepare and preview songs for `midi/`:

| Script | What it does |
|---|---|
| `tools/midi2h.py` | Converts a Standard MIDI File into a C header (`song.h`) for the sketch. |
| `tools/midiplay.py` | Plays a MIDI file on the PC, before converting it. |

They are managed with [uv](https://docs.astral.sh/uv/), which creates the
virtual environment (`.venv`) and installs the dependencies listed in
`pyproject.toml` at the repository root.  Run everything from the repository
root.

### Setup

1. Install uv: <https://docs.astral.sh/uv/getting-started/installation/>
2. Install the dependencies:

   ```sh
   uv sync
   ```

   uv also fetches the Python version in `.python-version` if it is missing.
   The first sync compiles AMY, a C extension, from its git repository, so it
   needs `git`, network access and a C compiler (about 15 s).  AMY is pinned
   to a tag in `pyproject.toml`; keep it in line with the version of the
   *AMY Synthesizer* Arduino library.

There is no need to activate the environment: `uv run` uses it.

### Convert a song for the sketch

```sh
uv run tools/midi2h.py song.mid -o audio/midi/song.h
```

It needs only the Python standard library.  It prints each channel's
instrument and warns about MIDI features that are dropped (pitch bend, pedal,
and so on).

### Preview a song on the PC

```sh
uv run tools/midiplay.py song.mid                 # AMY synthesizer
uv run tools/midiplay.py song.mid --board         # as the Black Pill plays it
uv run tools/midiplay.py song.mid --start 10 --seconds 30
uv run tools/midiplay.py song.mid --wav song.wav --no-play
```

- Default mode renders with AMY, using richer General MIDI sounds. It shows the
  arrangement and how busy the song is.
- `--board` compiles the sketch's own synthesis code with LEAF, to hear exactly
  what the board will play, including voice stealing and clipping.  It needs
  `gcc`/`g++` (or `$CC`/`$CXX`) and LEAF, installed once with
  `tools/install-leaf.sh`.  It does not need AMY.
- The audio is rendered to a WAV file and played with `paplay`, `aplay` or
  `ffplay` (Linux) or `afplay` (macOS).  `--wav` keeps the file, and
  `--no-play` only renders it.

Run `uv run tools/midiplay.py -h` for all options.

### Notes

- Add a dependency with `uv add <package>`; it updates `pyproject.toml` and
  `uv.lock`.  Commit both.
- AMY's Python build does not support Windows.  `midi2h.py` runs anywhere
  (`py tools\midi2h.py song.mid`).
