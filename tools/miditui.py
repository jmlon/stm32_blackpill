#!/usr/bin/env python3
"""Browse and play a MIDI file in a text-mode interface.

    uv run tools/miditui.py [song.mid]

Shows the file's length, tracks and events, and lists its parts: one row
per track and channel with notes, with the track's name, the General MIDI
instrument and the number of notes.  Unchecked parts are left out when
playing, to hear an instrument alone or without it.  The parts are those
tools/midi2h.py converts, so what plays here is what the board gets.

The song plays as tools/midiplay.py plays it: with AMY, or with
Options > Board as the Black Pill plays it (see midiplay.py for what each
needs).  It is rendered offline, which takes a few seconds the first time
for each set of checked parts, and played with the system's player; pause,
resume and seek restart the player at the new position.

Keys: Up/Down select a part, Space checks it, a/n check all/none,
p play/pause, s stop, Left/Right seek 5 s, Home back to the start,
g go to a time, o open, h save song.h, w save WAV, e switch engine,
q quit; F10 or Esc opens the menu, Alt+F/P/O one of its menus.

File > Save song.h writes the checked parts as the header for audio/midi,
as tools/midi2h.py does for a whole file, and Save WAV the checked parts'
audio, so the parts that go to the board can be picked by ear.

Needs curses, so it runs on Linux or macOS.
"""

import contextlib
import curses
import io
import locale
import os
import subprocess
import sys
import tempfile
import threading
import time

import midi2h  # same directory
import midiplay

SEEK_S = 5.0
ENGINES = {"amy": "AMY", "board": "Board (Black Pill)"}
SONG_HEADER = os.path.join(midiplay.REPO, "audio", "midi", "song.h")

# (label, action, shortcut); None draws a separator.
MENUS = [
    ("File", [("Open...", "open", "o"), ("Save song.h...", "header", "h"),
              ("Save WAV...", "save", "w"), None, ("Quit", "quit", "q")]),
    ("Play", [("Play / Pause", "play_pause", "p"), ("Stop", "stop", "s"),
              ("Go to time...", "goto", "g"), ("Back to start", "rewind", "Home")]),
    ("Options", [("AMY", "engine_amy", ""), ("Board (Black Pill)", "engine_board", "")]),
]


def fmt_time(seconds):
    minutes, seconds = divmod(max(0.0, seconds), 60)
    return f"{int(minutes)}:{seconds:04.1f}"


def parse_time(text):
    """'1:23.5' or '83.5' -> seconds; raises ValueError otherwise."""
    fields = text.strip().split(":")
    if len(fields) > 2:
        raise ValueError(text)
    seconds = float(fields[-1])
    if len(fields) == 2:
        seconds += 60 * int(fields[0])
    return seconds


def render(engine, path, keep):
    """Renders the song with only the parts in keep: (PCM, rate, channels).

    It runs in a worker thread, so midiplay's printing and AMY's C
    diagnostics are kept off the screen, and its sys.exit calls become
    errors."""
    song = midi2h.convert(path, keep)
    try:
        with contextlib.redirect_stdout(io.StringIO()), midiplay.quiet_c_stderr():
            if engine == "board":
                pcm, rate, channels, _ = midiplay.render_board(song, path, 0, None)
            else:
                midiplay.load_amy()
                pcm, rate, channels, _ = midiplay.render_amy(song, 0, None)
    except SystemExit as e:
        raise RuntimeError(str(e.code).strip().splitlines()[0]) from None
    # The board renderer stops at the last note: with the last parts
    # unchecked, pad it to the song's length, so the timeline does not shrink.
    size = int(song["length_ms"] / 1000 * rate) * 2 * channels
    return pcm + bytes(max(0, size - len(pcm))), rate, channels


class Player:
    """Plays rendered audio with the system's player, from any position."""

    def __init__(self, directory):
        self.wav = os.path.join(directory, "play.wav")
        self.proc = None
        self.started = 0.0  # when position 0 played, on the monotonic clock

    def playing(self):
        return self.proc is not None and self.proc.poll() is None

    def position(self):
        return time.monotonic() - self.started

    def start(self, audio, offset):
        self.stop()
        command = midiplay.player_command()
        if not command:
            raise RuntimeError("no audio player found (paplay, aplay, ffplay or afplay)")
        pcm, rate, channels = audio
        frame = 2 * channels
        first = min(int(offset * rate), len(pcm) // frame)
        midiplay.write_wav(self.wav, pcm[first * frame:], rate, channels)
        self.proc = subprocess.Popen(command + [self.wav], stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.started = time.monotonic() - first / rate

    def stop(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
            self.proc = None


class App:
    def __init__(self, screen, directory):
        self.screen = screen
        self.directory = directory
        self.player = Player(directory)
        self.engine = "amy"
        self.path = None
        self.song = None
        self.checked = set()
        self.cursor = 0
        self.scroll = 0
        self.state = "stopped"  # or "playing", "paused"
        self.position = 0.0  # where play starts or resumes, when not playing
        self.cache = {}  # (path, engine, kept parts) -> rendered audio
        self.job = None  # [key, thread, result, error] of the render in progress
        self.want = None  # ("play", offset) or ("save", path), once rendered
        self.menu_choice = {}  # the item last chosen in each menu
        self.message = "Press o to open a MIDI file, F10 for the menu."
        self.running = True

    # --- song and rendering ------------------------------------------------

    def load(self, path):
        try:
            song = midi2h.convert(path)
        except (OSError, ValueError, IndexError) as e:
            self.message = f"{os.path.basename(path)}: {e}"
            return
        self.stop()
        self.want = None
        self.cache.clear()
        self.path = os.path.abspath(path)
        self.song = song
        self.checked = {(p["track"], p["channel"]) for p in song["parts"]}
        self.cursor = self.scroll = 0
        self.message = f"Opened {self.path}"

    def length(self):
        return self.song["length_ms"] / 1000 if self.song else 0.0

    def key(self):
        return (self.path, self.engine, frozenset(self.checked))

    def request(self, action):
        """Does action when the current parts are rendered, rendering them first."""
        if not self.song:
            self.message = "No file open."
            return
        self.want = action
        self.tick()

    def tick(self):
        """Advances rendering and playback; called on every loop."""
        if self.job and not self.job[1].is_alive():
            key, _, result, error = self.job
            self.job = None
            if error:
                self.message = f"Render failed: {error}"
                if self.want and self.want[0] == "play":
                    self.state = "stopped"
                self.want = None
            else:
                self.cache[key] = result
        if self.want and not self.job:
            key = self.key()
            if key in self.cache:
                action, self.want = self.want, None
                self.perform(action, self.cache[key])
            else:
                self.start_render(key)
        if self.state == "playing" and not self.player.playing():
            self.state, self.position = "stopped", 0.0  # reached the end

    def start_render(self, key):
        path, engine, keep = key
        job = [key, None, None, None]

        def work():
            try:
                job[2] = render(engine, path, set(keep))
            except Exception as e:  # shown to the user, not raised
                job[3] = str(e) or type(e).__name__

        job[1] = threading.Thread(target=work, daemon=True)
        self.job = job
        job[1].start()

    def perform(self, action, audio):
        kind, arg = action
        try:
            if kind == "play":
                self.player.start(audio, min(arg, self.length()))
                self.state = "playing"
            elif kind == "save":
                midiplay.write_wav(arg, *audio)
                self.message = f"Wrote {arg}"
        except (OSError, RuntimeError) as e:
            self.state = "stopped" if kind == "play" else self.state
            self.message = str(e)

    def current_position(self):
        if self.state == "playing":
            return min(self.player.position(), self.length())
        if self.want and self.want[0] == "play":
            return self.want[1]
        return self.position

    # --- playback commands -------------------------------------------------

    def play_pause(self):
        if self.state == "playing":
            self.position = self.current_position()
            self.player.stop()
            self.state = "paused"
        elif self.want and self.want[0] == "play":
            self.want = None  # cancel a play still waiting for its render
        else:
            self.request(("play", self.position))

    def stop(self):
        self.player.stop()
        if self.want and self.want[0] == "play":
            self.want = None
        self.state, self.position = "stopped", 0.0

    def seek(self, seconds):
        seconds = max(0.0, min(seconds, self.length()))
        if self.state == "playing" or (self.want and self.want[0] == "play"):
            self.request(("play", seconds))
        else:
            self.position = seconds

    def parts_changed(self):
        """Re-renders and resumes in place if playing; else renders on play."""
        if self.state == "playing":
            position = self.current_position()
            self.player.stop()
            self.state = "stopped"
            self.request(("play", position))

    def toggle(self):
        if not self.song or not self.song["parts"]:
            return
        p = self.song["parts"][self.cursor]
        self.checked ^= {(p["track"], p["channel"])}
        self.parts_changed()

    def check_all(self, on):
        if not self.song:
            return
        self.checked = {(p["track"], p["channel"]) for p in self.song["parts"]} if on else set()
        self.parts_changed()

    def set_engine(self, engine):
        if engine != self.engine:
            self.engine = engine
            self.message = f"Engine: {ENGINES[engine]}"
            self.parts_changed()

    def save_header(self, out):
        """Writes the checked parts as a C header for audio/midi, like midi2h.py."""
        try:
            song = midi2h.convert(self.path, self.checked)
            midi2h.write_header(song, self.path, out)
        except (OSError, ValueError, IndexError) as e:
            self.message = f"{out}: {e}"
            return
        over = "" if song["polyphony"] <= 8 else ", more than the board's 8"
        self.message = (f"Wrote {out}: {len(song['events'])} events "
                        f"({len(song['events']) * 6} bytes of flash), "
                        f"at most {song['polyphony']} notes at once{over}")

    def run_action(self, action):
        if action == "open":
            path = self.browse()
            if path:
                self.load(path)
        elif action == "save":
            if not self.song:
                self.message = "No file open."
                return
            default = os.path.splitext(self.path)[0] + ".wav"
            path = self.prompt("Save WAV (checked parts) as", default)
            if path:
                self.request(("save", os.path.expanduser(path)))
        elif action == "header":
            if not self.song:
                self.message = "No file open."
                return
            path = self.prompt("Save song.h (checked parts) as", SONG_HEADER)
            if path:
                self.save_header(os.path.expanduser(path))
        elif action == "quit":
            self.running = False
        elif action == "play_pause":
            self.play_pause()
        elif action == "stop":
            self.stop()
        elif action == "rewind":
            self.seek(0.0)
        elif action == "goto":
            text = self.prompt("Go to time (m:ss or seconds)", fmt_time(self.current_position()))
            if text:
                try:
                    self.seek(parse_time(text))
                except ValueError:
                    self.message = f"Not a time: {text}"
        elif action.startswith("engine_"):
            self.set_engine(action[len("engine_"):])

    # --- drawing -----------------------------------------------------------

    def put(self, y, x, text, attr=0):
        """Writes text clipped to the screen."""
        h, w = self.screen.getmaxyx()
        if 0 <= y < h and 0 <= x < w:
            try:
                self.screen.addstr(y, x, text[:max(0, w - x)], attr)
            except curses.error:
                pass  # writing the bottom-right cell moves the cursor off-screen

    def box(self, y, x, h, w, title=""):
        for row in range(y, y + h):
            self.put(row, x, " " * w)
        try:
            win = self.screen.derwin(h, w, y, x)
            win.box()
        except curses.error:
            return
        if title:
            self.put(y, x + 2, f" {title} "[:w - 4], curses.A_BOLD)

    def draw(self):
        s = self.screen
        s.erase()
        h, w = s.getmaxyx()
        self.draw_menu_bar()
        song = self.song
        if song:
            name = os.path.basename(self.path)
            fmt = f"format {song['format']}, {song['division']} ticks/beat"
            self.put(1, 1, f"{name}  -  {song['title']}"[:w - 2], curses.A_BOLD)
            tempos = ", ".join(f"{t:g}" for t in song["tempos"][:4])
            tempos += "..." if len(song["tempos"]) > 4 else ""
            notes = sum(p["notes"] for p in song["parts"])
            self.put(2, 1, f"Length {fmt_time(self.length())}   Tracks {song['track_count']}   "
                           f"Events {song['event_count']:,}   Notes {notes:,}   "
                           f"Tempo {tempos} BPM   ({fmt})")
            warnings = [f"{text} ({count}x)" for text, count in song["warnings"].items()]
            self.put(3, 1, f"Peak polyphony {song['polyphony']} (the board plays 8)   "
                           f"Engine {ENGINES[self.engine]}"
                           + (f"   Warnings: {'; '.join(warnings)}" if warnings else ""))
            self.draw_parts(5, h - 3)
        self.draw_progress(h - 2)
        self.put(h - 1, 0, (" " + self.message).ljust(w), curses.A_REVERSE)

    def draw_menu_bar(self, active=None):
        w = self.screen.getmaxyx()[1]
        self.put(0, 0, " " * w, curses.A_REVERSE)
        x = 1
        for i, (title, _) in enumerate(MENUS):
            attr = curses.A_NORMAL if i == active else curses.A_REVERSE
            self.put(0, x, f" {title} ", attr)
            x += len(title) + 3
        self.put(0, max(x, w - 22), "F10 menu  q quit"[:w - x - 1], curses.A_REVERSE)

    def draw_parts(self, top, bottom):
        """The parts table, from row top to row bottom (exclusive)."""
        w = self.screen.getmaxyx()[1]
        parts = self.song["parts"]
        room = max(0, w - 31)
        inst_w = min(30, room // 2)
        name_w = min(30, room - inst_w)
        header = f"     Trk  Ch  {'Track name':<{name_w}}  {'Instrument':<{inst_w}}  Notes"
        self.put(top, 0, header.ljust(w), curses.A_BOLD | curses.A_UNDERLINE)
        rows = bottom - top - 1
        if not parts:
            self.put(top + 1, 2, "No notes in this file.")
            return
        self.cursor = max(0, min(self.cursor, len(parts) - 1))
        self.scroll = max(min(self.scroll, self.cursor), self.cursor - rows + 1, 0)
        for i, p in enumerate(parts[self.scroll:self.scroll + rows]):
            index = self.scroll + i
            on = (p["track"], p["channel"]) in self.checked
            if p["channel"] == midi2h.DRUM_CHANNEL:
                instrument = "Drums"
                if p["dropped"]:
                    instrument += f" ({p['dropped']} unsupported)"
            else:
                instrument = midi2h.GM_PROGRAMS[p["program"]]
            line = (f" [{'x' if on else ' '}] {p['track'] + 1:>3}  {p['channel'] + 1:>2}  "
                    f"{(p['name'] or '-')[:name_w]:<{name_w}}  "
                    f"{instrument[:inst_w]:<{inst_w}}  {p['notes']:>5}")
            attr = curses.A_NORMAL if on else curses.A_DIM
            if index == self.cursor:
                attr |= curses.A_REVERSE
            self.put(top + 1 + i, 0, line.ljust(w), attr)
        if len(parts) > rows:
            self.put(bottom - 1, w - 12,
                     f" {self.scroll + 1}-{min(len(parts), self.scroll + rows)}/{len(parts)} ")

    def draw_progress(self, y):
        w = self.screen.getmaxyx()[1]
        if self.job or self.want:
            status = "Rendering..."
        else:
            status = self.state.capitalize()
        position, length = self.current_position(), self.length()
        left = f" {status:<12} {fmt_time(position)} / {fmt_time(length)} "
        bar_w = max(0, w - len(left) - 3)
        filled = int(bar_w * position / length) if length else 0
        self.put(y, 0, left + "[" + "#" * filled + "-" * (bar_w - filled) + "]")

    # --- modal dialogs -----------------------------------------------------

    def wait_key(self):
        """The next key; keeps playback and the screen going while waiting."""
        while True:
            key = self.screen.getch()
            if key != -1:
                return key
            self.tick()
            return None

    def menu(self, index):
        """The menu bar, with menu index open; runs the chosen action."""
        while True:
            items = MENUS[index][1]
            selectable = [i for i, item in enumerate(items) if item]
            choice = self.menu_choice.get(index, selectable[0])
            while True:
                self.tick()
                self.draw()
                self.draw_menu_bar(active=index)
                x = 1 + sum(len(t) + 3 for t, _ in MENUS[:index])
                width = max(len(item[0]) + len(item[2]) for item in items if item) + 8
                self.box(1, x, len(items) + 2, width)
                for row, item in enumerate(items):
                    y = 2 + row
                    if item is None:
                        self.put(y, x + 1, "-" * (width - 2))
                        continue
                    label, action, shortcut = item
                    if action.startswith("engine_"):
                        mark = "(*)" if action == f"engine_{self.engine}" else "( )"
                        label = f"{mark} {label}"
                    text = f" {label:<{width - len(shortcut) - 4}}{shortcut} "
                    self.put(y, x + 1, text, curses.A_REVERSE if row == choice else 0)
                self.screen.refresh()
                key = self.wait_key()
                if key is None:
                    continue
                if key in (27, curses.KEY_F10):
                    return
                if key == curses.KEY_UP:
                    choice = selectable[(selectable.index(choice) - 1) % len(selectable)]
                elif key == curses.KEY_DOWN:
                    choice = selectable[(selectable.index(choice) + 1) % len(selectable)]
                elif key in (curses.KEY_LEFT, curses.KEY_RIGHT):
                    index = (index + (1 if key == curses.KEY_RIGHT else -1)) % len(MENUS)
                    break
                elif key in (10, 13, curses.KEY_ENTER):
                    self.menu_choice[index] = choice
                    self.run_action(items[choice][1])
                    return

    def prompt(self, title, text=""):
        """A one-line text input; the text, or None if cancelled."""
        curses.curs_set(1)
        try:
            while True:
                self.tick()
                self.draw()
                h, w = self.screen.getmaxyx()
                width = min(w - 4, 72)
                x, y = (w - width) // 2, h // 2 - 2
                self.box(y, x, 3, width, title)
                field = width - 4
                shown = text[-field + 1:] if len(text) >= field else text
                self.put(y + 1, x + 2, shown)
                self.screen.move(y + 1, x + 2 + len(shown))
                self.screen.refresh()
                try:
                    key = self.screen.get_wch()
                except curses.error:
                    continue  # timeout
                if key in ("\n", "\r", curses.KEY_ENTER):
                    return text.strip() or None
                if key == "\x1b":
                    return None
                if key in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                    text = text[:-1]
                elif key == "\x15":  # Ctrl+U
                    text = ""
                elif isinstance(key, str) and key.isprintable():
                    text += key
        finally:
            curses.curs_set(0)

    def browse(self):
        """A file browser listing directories and MIDI files; a path or None."""
        directory = os.path.dirname(self.path) if self.path else os.getcwd()
        choice = top = 0
        while True:
            try:
                names = sorted(os.listdir(directory), key=str.lower)
            except OSError as e:
                self.message = str(e)
                names = []
            dirs = [n + "/" for n in names
                    if not n.startswith(".") and os.path.isdir(os.path.join(directory, n))]
            files = [n for n in names if n.lower().endswith((".mid", ".midi"))]
            entries = ["../"] + dirs + files
            while True:
                self.tick()
                self.draw()
                h, w = self.screen.getmaxyx()
                width, height = min(w - 4, 72), max(5, h - 4)
                x, y = (w - width) // 2, 2
                self.box(y, x, height, width, directory)
                rows = height - 2
                choice = max(0, min(choice, len(entries) - 1))
                top = max(min(top, choice), choice - rows + 1, 0)
                for row, entry in enumerate(entries[top:top + rows]):
                    attr = curses.A_REVERSE if top + row == choice else 0
                    if entry.endswith("/"):
                        attr |= curses.A_BOLD
                    self.put(y + 1 + row, x + 1, f" {entry}"[:width - 2].ljust(width - 2), attr)
                self.screen.refresh()
                key = self.wait_key()
                if key is None:
                    continue
                if key in (27, ord("q")):
                    return None
                if key == curses.KEY_UP:
                    choice -= 1
                elif key == curses.KEY_DOWN:
                    choice += 1
                elif key == curses.KEY_PPAGE:
                    choice -= rows
                elif key == curses.KEY_NPAGE:
                    choice += rows
                elif key == curses.KEY_HOME:
                    choice = 0
                elif key == curses.KEY_END:
                    choice = len(entries) - 1
                elif key in (curses.KEY_BACKSPACE, 127, 8, curses.KEY_LEFT):
                    entry = "../"
                    choice = 0
                    break
                elif key in (10, 13, curses.KEY_ENTER, curses.KEY_RIGHT):
                    entry = entries[choice]
                    if not entry.endswith("/"):
                        return os.path.join(directory, entry)
                    break
            previous = os.path.basename(directory)
            directory = os.path.normpath(os.path.join(directory, entry))
            choice = top = 0
            if entry == "../":  # back up: select the directory just left
                with contextlib.suppress(OSError):
                    visible = sorted((n for n in os.listdir(directory)
                                      if not n.startswith(".")
                                      and os.path.isdir(os.path.join(directory, n))),
                                     key=str.lower)
                    choice = visible.index(previous) + 1

    # --- main loop ---------------------------------------------------------

    def run(self):
        while self.running:
            self.tick()
            self.draw()
            self.screen.refresh()
            key = self.wait_key()
            if key is None:
                continue
            parts = self.song["parts"] if self.song else []
            rows = max(1, self.screen.getmaxyx()[0] - 9)
            if key == 27:  # Esc alone opens the menu; Esc + letter is Alt+letter
                self.screen.nodelay(True)
                follow = self.screen.getch()
                self.screen.timeout(100)
                letters = {ord(t[0].lower()): i for i, (t, _) in enumerate(MENUS)}
                self.menu(letters.get(follow, 0))
            elif key == curses.KEY_F10:
                self.menu(0)
            elif key == curses.KEY_UP:
                self.cursor = max(0, self.cursor - 1)
            elif key == curses.KEY_DOWN:
                self.cursor = min(len(parts) - 1, self.cursor + 1) if parts else 0
            elif key == curses.KEY_PPAGE:
                self.cursor = max(0, self.cursor - rows)
            elif key == curses.KEY_NPAGE:
                self.cursor = min(len(parts) - 1, self.cursor + rows) if parts else 0
            elif key == ord(" "):
                self.toggle()
            elif key == ord("a"):
                self.check_all(True)
            elif key == ord("n"):
                self.check_all(False)
            elif key == curses.KEY_LEFT:
                self.seek(self.current_position() - SEEK_S)
            elif key == curses.KEY_RIGHT:
                self.seek(self.current_position() + SEEK_S)
            elif key == curses.KEY_HOME:
                self.run_action("rewind")
            elif key == ord("e"):
                self.set_engine("board" if self.engine == "amy" else "amy")
            elif key == curses.KEY_RESIZE:
                pass  # redrawn on the next loop
            else:
                shortcuts = {ord(item[2]): item[1] for _, items in MENUS
                             for item in items if item and len(item[2]) == 1}
                if key in shortcuts:
                    self.run_action(shortcuts[key])


def main(argv):
    if len(argv) > 2 or (len(argv) == 2 and argv[1] in ("-h", "--help")):
        print(__doc__)
        return 0 if len(argv) == 2 else 2
    locale.setlocale(locale.LC_ALL, "")
    os.environ.setdefault("ESCDELAY", "25")  # Esc reacts at once

    def start(screen):
        curses.curs_set(0)
        with contextlib.suppress(curses.error):
            curses.use_default_colors()
        screen.keypad(True)
        screen.timeout(100)  # redraw the progress 10 times a second
        with tempfile.TemporaryDirectory(prefix="miditui-") as directory:
            app = App(screen, directory)
            if len(argv) == 2:
                app.load(argv[1])
            try:
                app.run()
            finally:
                app.player.stop()

    curses.wrapper(start)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
