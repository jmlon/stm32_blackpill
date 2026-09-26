#!/bin/sh
# Install AMY's Python module into .venv at the repository root, for
# tools/midiplay.py.
#
# The module is compiled (a C extension, about 15 s) from the AMY sources
# the Arduino library already holds, so it is the same AMY version as the
# sketches use.  It includes the Gamma9001 General MIDI drum kits.
#
# Usage: tools/install-amy-python.sh   (AMY sources: $AMY_SRC, default
#                                       ~/Arduino/libraries/AMY_Synthesizer)
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
src=${AMY_SRC:-$HOME/Arduino/libraries/AMY_Synthesizer}
venv=$repo/.venv

if [ ! -f "$src/setup.py" ]; then
	echo "AMY sources not found in $src." >&2
	echo "Install the Arduino library first: arduino-cli lib install \"AMY Synthesizer\"" >&2
	exit 1
fi

# pip builds a local directory in place (build/, *.egg-info, and AMY's
# setup.py generates build/drums_bin.c), so build from a copy to leave the
# Arduino library untouched.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -r "$src" "$tmp/amy"

[ -d "$venv" ] || python3 -m venv "$venv"
"$venv/bin/pip" install --quiet --upgrade pip
"$venv/bin/pip" install --quiet "$tmp/amy"

echo "Installed AMY in $venv; run: $venv/bin/python tools/midiplay.py song.mid"
