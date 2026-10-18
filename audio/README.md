# Audio

Sketches and notes on audio synthesis for the Black Pill:

- `FM_Synthesis.md`: FM synthesis background, with the `fmsynth_*` demos.
- `MIDI libraries.md`: the synthesis libraries compared, for the MIDI player.
- `midi/`: the MIDI player sketch, and `song.h`, the song it plays.

## Python tools

Three helper scripts in `../tools` prepare and preview songs for `midi/`:

| Script | What it does |
|---|---|
| `tools/midi2h.py` | Converts a Standard MIDI File into a C header (`song.h`) for the sketch. |
| `tools/midiplay.py` | Plays a MIDI file on the PC, before converting it. |
| `tools/miditui.py` | Text-mode browser and player: lists a file's tracks and plays them, with parts muted. |

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

### Browse a song and mute parts

```sh
uv run tools/miditui.py [song.mid]
```

A text-mode (curses) interface that shows the file's length, tracks, events
and tempo, and lists its parts: one row per track and channel with notes, with
the track name, the General MIDI instrument and the number of notes.  It plays
the song like `midiplay.py`, with AMY or, from *Options*, as the board plays
it.  Unchecked parts are left out, so an instrument can be heard alone or
without it.

To choose what goes to the board, uncheck the parts to leave out, listen,
and use *File > Save song.h* (`h`; it defaults to `audio/midi/song.h`).  The
header holds only the checked parts and lists the left-out ones in its
comment.  *File > Save WAV* also saves only the checked parts.  `midi2h.py`
still converts whole files, for batch jobs.

| Key | Action |
|---|---|
| Up / Down, PgUp / PgDn | Select a part |
| Space | Check / uncheck the part |
| `a` / `n` | Check all / none |
| `p` | Play / pause |
| `s` | Stop |
| Left / Right | Seek 5 s back / forward |
| Home | Back to the start |
| `g` | Go to a time (`1:23.5` or `83.5`) |
| `o` | Open a file (a browser; Backspace goes up a directory) |
| `h` | Save the checked parts as `song.h` for the sketch |
| `w` | Save the checked parts as a WAV file |
| `e` | Switch engine (AMY / board) |
| F10 or Esc, Alt+F/P/O | Menus |
| `q` | Quit |

The song is rendered offline, once for each set of checked parts and each
engine, and then played with the system's player.  The first play, or the
first after a change, waits for that render: under a second for AMY, a few
seconds for the board.  Changing the parts while playing re-renders the song
and resumes at the same point.

### Notes

- Add a dependency with `uv add <package>`; it updates `pyproject.toml` and
  `uv.lock`.  Commit both.
- AMY's Python build does not support Windows, and Python's `curses` module,
  which `miditui.py` needs, is not available there.  `midi2h.py` runs anywhere
  (`py tools\midi2h.py song.mid`).
