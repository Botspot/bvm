# Build:  make          (builds hang-repro and the shaders used by --bg and the !t shapes)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter

all: hang-repro shaders

hang-repro: hang-repro.c
	$(CC) $(CFLAGS) -o $@ $< -lvulkan

shaders: tri.vert.spv tri.frag.spv tex.vert.spv tex.frag.spv

%.spv: %
	glslangValidator -V $< -o $@

clean:
	rm -f hang-repro *.spv

.PHONY: all shaders clean
