#!/bin/sh
# Verify a firmware image's initial stack pointer matches the target's SRAM top.
# A mismatch means the sketch was built for a different MCU and would
# hard-fault at reset (an STM32F3 target, for example, sets 0x2000a000).
set -eu

bin=$1
expected=$2

sp=$(od -An -tx4 -N4 "$bin" | tr -d ' ')
if [ "$sp" != "$expected" ]; then
	echo "$bin: stack pointer is 0x$sp, expected 0x$expected." >&2
	echo "Built for the wrong board -- check FQBN in the Makefile." >&2
	exit 1
fi
echo "$bin: stack pointer 0x$sp OK"
