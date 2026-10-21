.PHONY: default clean

default:
	cc -std=c11 -O2 -Wall -Wextra `pkg-config --cflags --libs sdl3` -o gb1k gb1k.c

clean:
	rm -f gb1k
