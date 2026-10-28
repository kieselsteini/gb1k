/*
========================================================================================================================

	GB1k - a small GameBoy emulator written in ~1K loc plain C11.
	Written by Sebastian Steinhauer <s.steinhauer@yahoo.de> ' 2026

========================================================================================================================
*/
// standard C headers
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

// SDL3 headers
#include <SDL3/SDL.h>
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#define KiB(n) (1024 * (n)) // definition for a kilobyte
#define BIT(n) (1 << (n)) // select a single bit

enum {
	TICK_RATE = 60, // GameBoy run with ~60Hz
	TICK_TIME = 1000000000 / TICK_RATE, // time for a single tick
	VIDEO_WIDTH = 160, // video width in pixels
	VIDEO_HEIGHT = 144, // video height in pixels
	CYCLE_COUNT = 70224 / 4, // M-cycles per tick
	CYCLE_RATE = CYCLE_COUNT * TICK_RATE, // M-cycles per second
	AUDIO_RATE = 44100, // audio mixing rate
};

#define WINDOW_TITLE "GB1k" // our window title
#define WINDOW_SCALE 0.8f // scale window to ~80% of desktop size

enum { IME_DISABLED, IME_PENDING, IME_ENABLED }; // CPU interrupt states
enum { INT_VBLANK, INT_LCD, INT_TIMER, INT_SERIAL, INT_JOYPAD }; // CPU interrupts
enum { ZF = BIT(7), NF = BIT(6), HF = BIT(5), CF = BIT(4) }; // CPU flags
enum { PPU_HBLANK, PPU_VBLANK, PPU_OAM, PPU_PIXELS }; // PPU modes
enum { BTN_A = 1, BTN_B = 2, BTN_SELECT = 4, BTN_START = 8, BTN_RIGHT = 16, BTN_LEFT = 32, BTN_UP = 64, BTN_DOWN = 128 }; // button bit-masks

typedef uint8_t byte; // define 8-bit byte
typedef uint16_t word; // define 16-bit word

typedef struct chan_t {
	byte enabled, volume, envelope;
	word period, length, step;
} chan_t;

// all GameBoy state lives in this nested structure
static struct gb_t {
	// memory system
	struct {
		byte *rom, rom_size; // ROM data and size in banks
		byte wram[KiB(8)], xram[KiB(32)], battery; // work- and external-ram and flag if we have battery
		uint32_t rom_base, ram_base, rom_hash; // base pointers for ROM/RAM banks, and 32-bit hash value for the ROM
		union { byte oram[256]; struct obj_t { byte y, x, tile, attr; } obj[40]; }; // OAM with attributes
		union { byte vram[KiB(8)]; struct { byte pixels[384][8][2], maps[2][32][32]; };}; // 384 tiles and two 32x32 tile-maps
	} mem;
	// I/O system (also hold high-ram)
	union { // define high-ram and all the I/O ports we need
		byte mem[256]; // 256 byte high-ram + I/O
		struct {
			byte joyp, sb, sc, X0, div, tima, tma, tac, X1[7], ifr;
			struct ioch_t { byte nrx0, nrx1, nrx2, nrx3, nrx4; } ch[4];
			byte nr50, nr51, nr52, X4[9], wave[16];
			byte lcdc, stat, scy, scx, ly, lyc, dma, bgp, obp[2], wy, wx;
			byte X5[179], ier;
		};
	} io;
	// timer system
	struct {
		uint32_t cycles; // counter for M-cycles
		word div; // timer divider register
	} timer;
	// CPU system
	struct {
		byte halted, ime; // flag if CPU is halted and current interrupt state
		byte a, f, b, c, d, e, h, l; // 8-bit registers
		word sp, pc; // 16-bit registers
	} cpu;
	// PPU system
	struct {
		byte ready, mode; // flag to indicate the the buffer is ready and keep the mode
		word lx, ly; // current X/Y position during rendering
		uint32_t buffer[VIDEO_WIDTH * VIDEO_HEIGHT]; // 32-bit RGBA screen buffer
	} ppu;
	// APU system
	struct {
		word div, lfsr; // DIV-APU counter, linear feedback shift register for noise
		word sweep_period, sweep_timer; bool sweep_enabled; // channel 1 sweep parameters
		chan_t chan[4]; // internal audio channel data
		int16_t buffer[512]; // internal PCM audio buffer (interleaved stereo L/R)
		unsigned int accu, pos; // internal M-cycle accumulator and buffer position
		int sample[2], count; // accumulated left/right samples and how many of them
		float hp_in[2], hp_out[2]; // simple left/right high-pass filter to emulate the analog audio mixing
	} apu;
} gb;

// all SDL application state lives in this structure
static struct app_t {
	jmp_buf error; // error handling routine
	uint64_t last, accu; // last measured time and accumulated delta time between frames
	byte buttons; // button state
	SDL_Window *window; // SDL window object
	SDL_Renderer *renderer; // SDL renderer object
	SDL_Texture *texture; // SDL texture object
	SDL_AudioDeviceID device; // SDL audio device object
	SDL_AudioStream *stream; // SDL audio stream object
	byte palette, volume, channels; // currently selected color palette, volume level and sound channels
	byte turbo_enabled, turbo_count; // flag for turbo mode and the turbo counter
	char *prefpath; // path where we store our save-files
} app;

// tick one M-cycle for the emulation
static void tick_cycle(void);

// show error message and quit
_Noreturn void fail(const char *fmt, ...) {
	char message[1024]; va_list va;
	va_start(va, fmt); SDL_vsnprintf(message, sizeof(message), fmt, va); va_end(va);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error!", message, app.window);
	longjmp(app.error, 1);
}

// format string printf-style
static const char *strf(const char *fmt, ...) {
	static char buffer[1024]; va_list va;
	va_start(va, fmt); SDL_vsnprintf(buffer, sizeof(buffer), fmt, va); va_end(va);
	return buffer;
}

// return minimum integer
static int mini(const int a, const int b) {
	return a < b ? a : b;
}

// return maximum integer
static int maxi(const int a, const int b) {
	return a > b ? a : b;
}

// detect falling edge bit 1 -> 0
static bool falling_edge(const word ov, const word nv, const byte bit) {
	return (ov & BIT(bit)) && !(nv & BIT(bit));
}

// request interrupt n
static void request_interrupt(const byte n) {
	gb.io.ifr |= (1 << n) & 0x1f;
}

// access memory bus
static byte bus8(const word addr, const byte data, const bool write) {
	switch (addr / KiB(8)) {
		case 0: // ROM bank 00
			if (write) return 0;
		case 1: // ROM bank 00
			if (write) { const byte bank = data % gb.mem.rom_size; gb.mem.rom_base = (bank ? bank : 1) * KiB(16); }
			return gb.mem.rom[addr & 0x3fff];
		case 2: // ROM bank 01 - nn
			if (write) gb.mem.ram_base = (data % 4) * KiB(8);
		case 3: // ROM bank 01 - nn
			return gb.mem.rom[gb.mem.rom_base + (addr & 0x3fff)];
		case 4: // video ram
			return write ? gb.mem.vram[addr & 0x1fff] = data : gb.mem.vram[addr & 0x1fff];
		case 5: // external ram
			return write ? gb.mem.xram[gb.mem.ram_base + (addr & 0x1fff)] = data : gb.mem.xram[gb.mem.ram_base + (addr & 0x1fff)];
		case 6: // work ram
			return write ? gb.mem.wram[addr & 0x1fff] = data : gb.mem.wram[addr & 0x1fff];
		case 7: // various things
			if (addr <= 0xfdff) {
				return write ? gb.mem.wram[addr & 0x1fff] = data : gb.mem.wram[addr & 0x1fff]; // echo RAM
			} else if (addr <= 0xfeff) {
				return write ? gb.mem.oram[addr & 0xff] = data : gb.mem.oram[addr & 0xff]; // OAM
			} else if (write) {
				switch (addr) {
					case 0xff04: gb.timer.div = 0; break; // reset DIV
					case 0xff46: for (int i = 0; i < 40 * 4; ++i) gb.mem.oram[i] = bus8((data << 8) + i, 0, false); break; // do DMA
					default: gb.io.mem[addr & 0xff] = data; break;
				}
			} else {
				return gb.io.mem[addr & 0xff];
			}
	}
	return 0xff; // fail-safe for compiler
}

// access memory byte through CPU
static byte mem8(const word addr, const byte data, const bool write) {
	const byte tmp = bus8(addr, data, write);
	tick_cycle();
	return tmp;
}

// access memory word through CPU
static word mem16(const word addr, const word data, const bool write) {
	if (write) {
		mem8(addr + 0, (byte)(data & 0xff), true);
		mem8(addr + 1, (byte)(data >> 8), true);
		return data;
	}
	const byte lo = mem8(addr, 0, false);
	return lo | (mem8(addr + 1, 0, false) << 8);
}

// access stack
static word stk16(const word data, const bool write) {
	if (write) {
		mem8(--gb.cpu.sp, (byte)(data >> 8), true);
		mem8(--gb.cpu.sp, (byte)(data & 0xff), true);
		tick_cycle(); // tick additional M-cycle
		return data;
	}
	const byte lo = mem8(gb.cpu.sp++, 0, false);
	return lo | (mem8(gb.cpu.sp++, 0, false) << 8);
}

// fetch next byte
static byte fetch8(void) {
	return mem8(gb.cpu.pc++, 0, false);
}

// fetch next word
static word fetch16(void) {
	const byte lo = fetch8();
	return lo | (fetch8() << 8);
}

// access r8 register group - B, C, D, E, H, L, (HL), A
static byte r8(const byte which, const byte data, const bool write) {
	static byte *r[] = { &gb.cpu.b, &gb.cpu.c, &gb.cpu.d, &gb.cpu.e, &gb.cpu.h, &gb.cpu.l, NULL, &gb.cpu.a };
	if (which == 6) return mem8((gb.cpu.h << 8) | gb.cpu.l, data, write);
	return write ? *r[which] = data : *r[which];
}

// access r16 register group - BC, DE, HL, SP
static word r16(const byte which, const word data, const bool write) {
	static byte *r[] = { &gb.cpu.b, &gb.cpu.c, &gb.cpu.d, &gb.cpu.e, &gb.cpu.h, &gb.cpu.l };
	if (which == 3) return write ? gb.cpu.sp = data : gb.cpu.sp;
	if (write) { *r[which * 2] = (byte)(data >> 8); *r[which * 2 + 1] = (byte)(data & 0xff); return data; }
	return (*r[which * 2] << 8) | *r[which * 2 + 1];
}

// access r16stk register group - BC, DE, HL, AF
static word r16stk(const byte which, const word data, const bool write) {
	static byte *r[] = { &gb.cpu.b, &gb.cpu.c, &gb.cpu.d, &gb.cpu.e, &gb.cpu.h, &gb.cpu.l, &gb.cpu.a, &gb.cpu.f };
	static const byte m[] = { 0xff, 0xff, 0xff, 0xf0 };
	if (write) { *r[which * 2] = (byte)(data >> 8); *r[which * 2 + 1] = (byte)(data & m[which]); return data; }
	return (*r[which * 2] << 8) | (*r[which * 2 + 1] & m[which]);
}

// access r16mem register group - (BC), (DE), (HL+), (HL-)
static byte r16mem(const byte which, const byte data, const bool write) {
	static const byte m[] = { 0, 1, 2, 2 };
	static const int8_t d[] = { 0, 0, 1, -1 };
	const word addr = r16(m[which], 0, false);
	const byte tmp = mem8(addr, data, write);
	r16(m[which], addr + d[which], true);
	return tmp;
}

// check opcode condition - NZ, Z, NC, C
static bool opcond(const byte which) {
	static const byte flags[] = { ZF, ZF, CF, CF }; // mapping of the CPU flags to check
	static const byte masks[] = { ZF, 0, CF, 0 }; // xor-masks for inverting the result
	return (gb.cpu.f & flags[which]) ^ masks[which]; // grab the CPU flag and invert it if needed
}

// return the value of the carry flag
static byte carry_value(void) {
	return (gb.cpu.f & CF) ? 1 : 0;
}

// set CPU flags
static void set_flags(const byte keep, const bool z, const bool n, const bool h, const bool c) {
	gb.cpu.f = (gb.cpu.f & keep) | (z * ZF) | (n * NF) | (h * HF) | (c * CF);
}

// 8-bit arithmetic operations - ADD, ADC, SUB, SBC, AND, XOR, OR, CP, INC, DEC
static byte alu8(const byte which, const byte data) {
	byte a = gb.cpu.a, c = 0; word tmp = 0; // fetch register A, set carry to 0 and reserve a temporary var
	switch (which) {
		case 1: c = carry_value(); // ADC - fallthrough
		case 0: tmp = a + data + c; set_flags(0, !(tmp & 0xff), false, ((a & 0x0f) + (data & 0x0f) + c) & 0xf0, tmp & 0xff00); break; // ADD
		case 3: c = carry_value(); // SBC - fallthrough
		case 2: tmp = a - data - c; set_flags(0, !(tmp & 0xff), true, ((a & 0x0f) - (data & 0x0f) - c) & 0xf0, tmp & 0xff00); break; // SUB
		case 4: tmp = a & data; set_flags(0, !tmp, false, true, false); break; // AND
		case 5: tmp = a ^ data; set_flags(0, !tmp, false, false, false); break; // XOR
		case 6: tmp = a | data; set_flags(0, !tmp, false, false, false); break; // OR
		case 7: tmp = a - data; set_flags(0, !(tmp & 0xff), true, ((a & 0x0f) - (data & 0x0f)) & 0xf0, tmp & 0xff00); return a; // CP
		case 8: tmp = data + 1; set_flags(CF, !(tmp & 0xff), false, ((data & 0x0f) + 1) & 0xf0, 0); return (byte)(tmp & 0xff); // INC
		case 9: tmp = data - 1; set_flags(CF, !(tmp & 0xff), true, ((data & 0x0f) - 1) & 0xf0, 0); return (byte)(tmp & 0xff); // DEC
	}
	return gb.cpu.a = (byte)(tmp & 0xff); // save the result back to register A
}

// 8-bit bitwise operations - RLC, RRC, RL, RR, SLA, SRA, SWAP, SRL
static byte bit8(const byte which, const byte data) {
	byte tmp = 0, c = carry_value(); // create temporary variable and get carry value
	switch (which) {
		case 0: tmp = (data << 1) | (data >> 7); set_flags(0, !tmp, false, false, data & 0x80); break; // RLC
		case 1: tmp = (data >> 1) | (data << 7); set_flags(0, !tmp, false, false, data & 0x01); break; // RRC
		case 2: tmp = (data << 1) | (c << 0); set_flags(0, !tmp, false, false, data & 0x80); break; // RL
		case 3: tmp = (data >> 1) | (c << 7); set_flags(0, !tmp, false, false, data & 0x01); break; // RR
		case 4: tmp = data << 1; set_flags(0, !tmp, false, false, data & 0x80); break; // SLA
		case 5: tmp = (data >> 1) | (data & 0x80); set_flags(0, !tmp, false, false, data & 0x01); break; // SRA
		case 6: tmp = (data >> 4) | (data << 4); set_flags(0, !tmp, false, false, false); break; // SWAP
		case 7: tmp = data >> 1; set_flags(0, !tmp, false, false, data & 0x01); break; // SRL
	}
	return tmp; // return the result
}

// add value to HL
static word add_hl(const word data) {
	const word hl = r16(2, 0, false);
	const uint32_t tmp = hl + data;
	set_flags(ZF, 0, false, ((hl & 0x0fff) + (data & 0x0fff)) & 0xf000, tmp & 0xffff0000);
	r16(2, (word)(tmp & 0xffff), true);
	tick_cycle();
	return (word)(tmp & 0xffff);
}

// add e8 to SP
static word add_sp_e8(void) {
	const byte e8 = fetch8(); // get e8 operand
	const word tmp = gb.cpu.sp + (int8_t)e8; // calculate the result
	set_flags(0, false, false, ((gb.cpu.sp & 0x0f) + (e8 & 0x0f)) & 0xf0, ((gb.cpu.sp & 0xff) + e8) & 0xff00);
	tick_cycle();
	return tmp;
}

// decimal adjust accumulator
static void daa(void) {
	byte adjust = 0;
	if (gb.cpu.f & NF) {
		if (gb.cpu.f & HF) adjust += 0x06;
		if (gb.cpu.f & CF) adjust += 0x60;
		gb.cpu.a -= adjust;
	} else {
		if ((gb.cpu.f & HF) || ((gb.cpu.a & 0x0f) > 0x09)) adjust += 0x06;
		if ((gb.cpu.f & CF) || (gb.cpu.a > 0x99)) { adjust += 0x60; gb.cpu.f |= CF; }
		gb.cpu.a += adjust;
	}
	set_flags(NF | CF, !gb.cpu.a, 0, false, 0);
}

// subroutine instructions
static void gosub(const bool cond, const bool call, const bool relative) {
	word addr; if (relative) { int8_t e8 = fetch8(); addr = gb.cpu.pc + e8; } else addr = fetch16(); // determine target address
	if (!cond) return; // if condition is not met, return
	if (call) stk16(gb.cpu.pc, true); else tick_cycle(); // save PC on call and tick otherwise
	gb.cpu.pc = addr; // continue execution at target address
}

// return instructions
static void goback(const bool cond, const bool basic, const bool intr) {
	tick_cycle(); // always use at least one M-cycle
	if (!cond) return; // return if condition is not met
	if (!basic) tick_cycle(); // use another M-cycle if condition has to be checked
	if (intr) gb.cpu.ime = IME_ENABLED; // enable interrupts if requested
	gb.cpu.pc = stk16(0, false); // continue exeuction at saved PC
}

// execute next opcode
static void execute_opcode(void) {
	#define CODE(o, code, div) { opcrel = (opcode - o) / div; code; break; }
	#define CASE4x(o, step) case o: case o+step: case o+(step*2): case o+(step*3):
	#define CASE8(o) CASE4x(o, 1) CASE4x(o+4, 1)
	#define CASE16(o) CASE8(o) CASE8(o+8)
	#define OP1(o, code) case o: { code; break; }
	#define OP64(o, code) CASE16(o) CASE16(o+16) CASE16(o+32) CASE16(o+48) CODE(o, code, 1)
	#define OP4x(o, step, code) CASE4x(o, step) CODE(o, code, step)
	#define OP8x(o, step, code) CASE4x(o, step) CASE4x(o+(step*4), step) CODE(o, code, step)
	byte opcode, opcrel; // define the opcode and relative opcode
	switch (opcode = fetch8()) {
		// load instructions -------------------------------------------------------------------------------------------
		OP8x(0x06, 8, r8(opcrel, fetch8(), true)) // LD r8, n8
		OP4x(0x01, 16, r16(opcrel, fetch16(), true)) // LD r16, n16
		OP4x(0x02, 16, r16mem(opcrel, gb.cpu.a, true)) // LD (r16), A
		OP4x(0x0a, 16, gb.cpu.a = r16mem(opcrel, 0, false)) // LD A, (r16)
		OP1(0xe0, mem8(0xff00 + fetch8(), gb.cpu.a, true)) // LDH (a8), A
		OP1(0xf0, gb.cpu.a = mem8(0xff00 + fetch8(), 0, false)) // LDH A, (a8)
		OP1(0xe2, mem8(0xff00 + gb.cpu.c, gb.cpu.a, true)) // LDH (C), A
		OP1(0xf2, gb.cpu.a = mem8(0xff00 + gb.cpu.c, 0, false)) // LDH A, (C)
		OP1(0xea, mem8(fetch16(), gb.cpu.a, true)) // LD (a16), A
		OP1(0xfa, gb.cpu.a = mem8(fetch16(), 0, false)) // LD A, (a16)
		OP1(0x08, mem16(fetch16(), gb.cpu.sp, true)) // LD (a16), SP
		OP1(0xf9, gb.cpu.sp = r16(2, 0, false); tick_cycle()) // LD SP, HL
		OP64(0x40,
			if (opcode == 0x76) { gb.cpu.halted = true; break;} // HALT
			r8(opcrel / 8, r8(opcrel % 8, 0, false), true); // LD r8, r8
		)
		// arithmetic instructions -------------------------------------------------------------------------------------
		OP8x(0x04, 8, r8(opcrel, alu8(8, r8(opcrel, 0, false)), true)) // INC r8
		OP8x(0x05, 8, r8(opcrel, alu8(9, r8(opcrel, 0, false)), true)) // DEC r8
		OP4x(0x03, 16, r16(opcrel, r16(opcrel, 0, false) + 1, true); tick_cycle()) // INC r16
		OP4x(0x0b, 16, r16(opcrel, r16(opcrel, 0, false) - 1, true); tick_cycle()) // DEC r16
		OP64(0x80, alu8(opcrel / 8, r8(opcrel % 8, 0, false))) // xxx r8
		OP8x(0xc6, 8, alu8(opcrel, fetch8())) // xxx n8
		OP4x(0x07, 8, gb.cpu.a = bit8(opcrel, gb.cpu.a); gb.cpu.f &= CF) // RLCA, RRCA, RLA, RRA
		OP4x(0x09, 16, add_hl(r16(opcrel, 0, false))) // ADD HL, r16
		OP1(0xe8, gb.cpu.sp = add_sp_e8(); tick_cycle()) // ADD SP, e8
		OP1(0xf8, r16(2, add_sp_e8(), true)) // LD HL, SP+e8
		// subroutine instructions -------------------------------------------------------------------------------------
		OP1(0x18, gosub(true, false, true)) // JR e8
		OP4x(0x20, 8, gosub(opcond(opcrel), false, true)) // JR cond, e8
		OP1(0xe9, gb.cpu.pc = r16(2, 0, false)) // JP HL
		OP1(0xc3, gosub(true, false, false)) // JP a16
		OP4x(0xc2, 8, gosub(opcond(opcrel), false, false)) // JP cond, a16
		OP1(0xcd, gosub(true, true, false)) // CALL a16
		OP4x(0xc4, 8, gosub(opcond(opcrel), true, false)) // CALL cond, a16
		OP4x(0xc0, 8, goback(opcond(opcrel), false, false)) // RET cond
		OP1(0xc9, goback(true, true, false)) // RET
		OP1(0xd9, goback(true, true, true)) // RETI
		OP4x(0xc1, 16, r16stk(opcrel, stk16(0, false), true)) // POP r16
		OP4x(0xc5, 16, stk16(r16stk(opcrel, 0, false), true)) // PUSH r16
		OP8x(0xc7, 8, stk16(gb.cpu.pc, true); gb.cpu.pc = opcrel * 8) // RST $xx
		// $CB prefix --------------------------------------------------------------------------------------------------
		OP1(0xcb, switch (opcode = fetch8()) {
			OP64(0x00, r8(opcrel % 8, bit8(opcrel / 8, r8(opcrel % 8, 0, false)), true)) // xxx r8
			OP64(0x40, set_flags(CF, !(r8(opcrel % 8, 0, false) & BIT(opcrel / 8)), false, true, 0)) // BIT n, r8
			OP64(0x80, r8(opcrel % 8, r8(opcrel % 8, 0, false) & ~BIT(opcrel / 8), true)) // RES n, r8
			OP64(0xc0, r8(opcrel % 8, r8(opcrel % 8, 0, false) | BIT(opcrel / 8), true)) // SET n, r8
		})
		// various opcodes ---------------------------------------------------------------------------------------------
		OP1(0x00, ) // NOP
		OP1(0x10, gb.cpu.pc++) // STOP
		OP1(0xf3, gb.cpu.ime = IME_DISABLED) // DI
		OP1(0xfb, if (gb.cpu.ime == IME_DISABLED) gb.cpu.ime = IME_PENDING) // EI
		OP1(0x27, daa()) // DAA
		OP1(0x37, set_flags(ZF, 0, false, false, true)) // SCF
		OP1(0x3f, set_flags(ZF, 0, false, false, !carry_value())) // CCF
		OP1(0x2f, gb.cpu.a = ~gb.cpu.a; gb.cpu.f |= NF | HF) // CPL
		// error case --------------------------------------------------------------------------------------------------
		default: fail("illegal opcode $%02x at $%04x", opcode, (word)(gb.cpu.pc - 1));
	}
	#undef OP1
}

// run CPU cycle
static void run_cpu(void) {
	// check for pending interrupts and wake up CPU
	const byte intr = (gb.io.ifr & gb.io.ier) & 0x1f;
	if (intr != 0) {
		gb.cpu.halted = false; // wake up CPU
		if (gb.cpu.ime == IME_ENABLED) {
			for (byte i = 0; i < 5; ++i) {
				if (intr & BIT(i)) { // take interrupt (consumes 5 M-cycles in total)
					stk16(gb.cpu.pc, true);
					gb.io.ifr &= ~BIT(i);
					gb.cpu.ime = IME_DISABLED;
					gb.cpu.pc = 0x40 + (i * 8);
					tick_cycle(); tick_cycle();
					return;
				}
			}
		}
	}
	// check if CPU is halted
	if (gb.cpu.halted) {
		tick_cycle();
		return;
	}
	// check if we have IME pending and enable interrupts *AFTER* the next instruction
	const bool pending = gb.cpu.ime == IME_PENDING;
	execute_opcode();
	if (pending && (gb.cpu.ime == IME_PENDING)) gb.cpu.ime = IME_ENABLED;
}

// update the PPU mode and fire interrupt if requested by STAT bit
static void change_ppu_mode(const byte mode) {
	gb.ppu.mode = mode;
	if ((mode < 3) && (gb.io.stat & BIT(3 + mode))) request_interrupt(INT_LCD);
}

// return the pixel value for the given tile - we need to combine 2-bit planes for a pixel value
static byte tile_pixel(const byte tile, const byte x, const byte y, const bool alt_addr) {
	const word idx = (tile < 128) ? (alt_addr ? tile : tile + 256) : tile;
	return (((gb.mem.pixels[idx][y][0] << x) & 0x80) >> 7) | (((gb.mem.pixels[idx][y][1] << x) & 0x80) >> 6);
}

// return mapped pixel color value
static byte map_pixel_color(const byte pixel, const byte palette) {
	return (palette >> (pixel * 2)) & 3;
}

// render the whole scanline
static void render_scanline(void) {
	static const uint32_t palette[4][4] = { // 32-bit RGBA color palettes
		{ 0xe9efecff, 0xa0a08bff, 0x555568ff, 0x211e20ff },
		{ 0xe0f8d0ff, 0x88c070ff, 0x346856ff, 0x081820ff },
		{ 0xe6d69cff, 0xb4a56aff, 0x7b7162ff, 0x393829ff },
		{ 0x9ff4e5ff, 0x00b9beff, 0x005f8cff, 0x002b59ff },
	};
	const int y = gb.ppu.ly;
	const bool wnd = (gb.io.lcdc & BIT(5)) && (y >= gb.io.wy);
	const int objh = (gb.io.lcdc & BIT(2)) ? 16 : 8;

	// We iterate just once over OAM and render sprites in a temporary scanline we later mix with background.
	// All sprites draw in this 16-bit row with bits 8-15 representing the X value of the sprite for DMG sprite
	// rendering priority. Bit 7 is set when the sprite needs to be pushed behind the background and bit 0-2 represent the
	// mapped color value, which must be used for the rendering.
	word row[VIDEO_WIDTH] = {0};
	if (gb.io.lcdc & BIT(1)) { // check if objects are enabled
		for (int i = 0, count = 0; (i < 40) && (count < 10); ++i) { // iterate over whole OAM but stop at 10th sprite
			struct obj_t *obj = &gb.mem.obj[i];
			const int ox = obj->x - 8, oy = obj->y - 16;
			if ((y < oy) || (y >= oy + objh)) continue;
			count++;
			byte py = (obj->attr & BIT(6)) ? (objh - (y - oy + 1)) : (y - oy);
			byte tile = (objh == 16) ? (obj->tile & 0xfe) : obj->tile;
			if (py >= 8) { tile |= 1; py -= 8; }
			for (int x = 0; x < 8; ++x) { // draw the whole sprite-line
				const int sx = ox + x;
				if ((sx < 0) || (sx >= VIDEO_WIDTH)) continue;
				const byte px = (obj->attr & BIT(5)) ? (7 - (sx - ox)) : (sx - ox);
				const byte pixel = tile_pixel(tile, px, py, true);
				if (!pixel) continue;
				if ((row[sx] >> 8) && ((row[sx] >> 8) <= obj->x)) continue;
				row[sx] = (obj->x << 8);
				row[sx] |= map_pixel_color(pixel, gb.io.obp[(obj->attr >> 4) & 1]);
				if (obj->attr & BIT(7)) row[sx] |= BIT(7);
			}
		}
	}

	// render the whole scanline in one go
	for (int x = 0; x < VIDEO_WIDTH; ++x) {
		byte bgpixel = 0, color = 0;
		// check if background/window is enabled
		if (gb.io.lcdc & BIT(0)) {
			if (wnd && (x >= (gb.io.wx - 7))) {
				const byte sx = (x - (gb.io.wx - 7)) % 256, sy = (y - gb.io.wy) % 256;
				const byte tile = gb.mem.maps[(gb.io.lcdc >> 6) & 1][sy / 8][sx / 8];
				bgpixel = tile_pixel(tile, sx % 8, sy % 8, gb.io.lcdc & BIT(4));
			} else {
				const byte sx = (x + gb.io.scx) % 256, sy = (y + gb.io.scy) % 256;
				const byte tile = gb.mem.maps[(gb.io.lcdc >> 3) & 1][sy / 8][sx / 8];
				bgpixel = tile_pixel(tile, sx % 8, sy % 8, gb.io.lcdc & BIT(4));
			}
			color = map_pixel_color(bgpixel, gb.io.bgp);
		}
		// Check if we have a sprite color on this X position. Then we mix sprite and background color with
		// the GameBoy priority rules and finally draw the 32-bit RGBA pixel value to screen buffer.
		if (row[x] && (!((row[x] & 0x80) && bgpixel))) color = row[x] & 3;
		gb.ppu.buffer[y * VIDEO_WIDTH + x] = palette[app.palette % 4][color];
	}
}

// run PPU
static void run_ppu(void) {
	// check if PPU is active
	if (!(gb.io.lcdc & BIT(7))) {
		gb.io.stat &= 0x78; // reset status register
		gb.io.ly = 0; // reset LY register
		gb.ppu.lx = gb.ppu.ly = gb.ppu.mode = 0; // reset internal X/Y and mode
		return;
	}
	// run the PPU 4 times
	for (byte i = 0; i < 4; ++i) {
		// update the PPU state machine based on lx/ly
		if (gb.ppu.ly < 144) {
			if (gb.ppu.lx == 0) { change_ppu_mode(PPU_OAM); }
			else if (gb.ppu.lx == 80) { change_ppu_mode(PPU_PIXELS); }
			else if (gb.ppu.lx == 80 + 172) { change_ppu_mode(PPU_HBLANK); render_scanline(); }
		} else if ((gb.ppu.ly == 144) && (gb.ppu.lx == 0)) {
			change_ppu_mode(PPU_VBLANK);
			request_interrupt(INT_VBLANK);
			gb.ppu.ready = true;
		}
		// advance X+Y and fire interrupt when LY==LYC and it's requested, finally make sure I/O has current values
		if (++gb.ppu.lx >= 456) {
			gb.ppu.lx = 0;
			if (++gb.ppu.ly > 153) gb.ppu.ly = 0;
			if ((gb.ppu.ly == gb.io.lyc) && (gb.io.stat & BIT(6))) request_interrupt(INT_LCD);
		}
		if (gb.ppu.ly == gb.io.lyc) gb.io.stat |= BIT(2); else gb.io.stat &= ~BIT(2);
		gb.io.stat = (gb.io.stat & 0x7c) | gb.ppu.mode;
		gb.io.ly = (byte)gb.ppu.ly;
	}
}

// update channel length and envelope
static bool update_length_and_envelope(chan_t *ch, struct ioch_t *io, const word odiv) {
	if (falling_edge(odiv, gb.apu.div, 0) && (io->nrx4 & BIT(6)) && (++ch->length >= 64)) {
		return ch->enabled = false;
	}
	if ((io->nrx2 & 0xf8) == 0) {
		return ch->enabled = false;
	}
	const byte pace = io->nrx2 & 0x07;
	if ((pace > 0) && falling_edge(odiv, gb.apu.div, 2) && (--ch->envelope == 0)) {
		ch->volume = (io->nrx2 & BIT(3)) ? (byte)mini(ch->volume + 1, 15) : (byte)maxi(ch->volume - 1, 0);
		ch->envelope = pace;
	}
	return true;
}

// calculate the next channel 1 sweep period, disable the channel on overflow
static word calc_sweep_period(void) {
	const byte nr10 = gb.io.ch[0].nrx0;
	const word delta = gb.apu.sweep_period >> (nr10 & 0x07);
	const word period = (nr10 & BIT(3)) ? gb.apu.sweep_period - delta : gb.apu.sweep_period + delta;
	if (period > 0x7ff) gb.apu.chan[0].enabled = false;
	return period;
}

// update channel 1 sweep
static bool update_sweep(chan_t *ch, struct ioch_t *io, const word odiv) {
	if (!falling_edge(odiv, gb.apu.div, 1) || (--gb.apu.sweep_timer > 0)) return ch->enabled; // sweep runs at 128Hz
	const byte pace = (io->nrx0 >> 4) & 0x07;
	gb.apu.sweep_timer = pace ? pace : 8; // reset sweep timer (pace 0 is treated as 8)
	if (gb.apu.sweep_enabled && pace) {
		const word period = calc_sweep_period();
		if ((period <= 0x7ff) && (io->nrx0 & 0x07)) {
			gb.apu.sweep_period = period;
			io->nrx3 = (byte)(period & 0xff); io->nrx4 = (io->nrx4 & ~7) | (period >> 8); // write back to NR13/NR14
			calc_sweep_period();
		}
	}
	return ch->enabled;
}

// trigger a channel and load its initial state
static void trigger_channel(const int i, chan_t *ch, struct ioch_t *io) {
	io->nrx4 &= ~BIT(7);
	ch->period = io->nrx3 | ((io->nrx4 & 7) << 8);
	ch->volume = io->nrx2 >> 4;
	ch->length = (i == 2) ? io->nrx1 : (io->nrx1 & 0x3f);
	ch->envelope = io->nrx2 & 0x07;
	ch->enabled = true;
	if (i == 3) { gb.apu.lfsr = 0x7fff; ch->step = 0;} // reset LFSR + step on noise channel
	if (i == 2) { ch->step = 0;} // reset step for WAVE channel
	if (i == 0) { // initialize sweep on pulse channel 1
		const byte pace = (io->nrx0 >> 4) & 0x07, step = io->nrx0 & 0x07;
		gb.apu.sweep_period = ch->period;
		gb.apu.sweep_timer = pace ? pace : 8;
		gb.apu.sweep_enabled = pace || step;
		if (step) calc_sweep_period();
	}
}

// advance a channel by one M-cycle and return its output sample
static int channel_sample(const int i, chan_t *ch, struct ioch_t *io, const word odiv) {
	switch (i) {
		case 0:   // Pulse Channel 1
		case 1: { // Pulse Channel 2
			if (!update_length_and_envelope(ch, io, odiv)) return 0;
			if ((i == 0) && !update_sweep(ch, io, odiv)) return 0;
			if (++ch->period >= 0x800) {
				ch->period = io->nrx3 | ((io->nrx4 & 7) << 8);
				ch->step = (ch->step + 1) % 8;
			}
			static const byte patterns[] = { 0x80, 0x81, 0xe1, 0x7e }; // bit-patterns for duty cycles
			const int16_t s = (patterns[io->nrx1 >> 6] & BIT(ch->step)) ? 16 : -16;
			return s * ch->volume;
		}
		case 2: { // WAVE Channel
			if (!(io->nrx0 & BIT(7))) return ch->enabled = false; // DAC off
			if (falling_edge(odiv, gb.apu.div, 0) && (io->nrx4 & BIT(6) && (++ch->length >= 256))) {
				return ch->enabled = false;
			}
			for (int n = 0; n < 2; ++n) { // period divider runs at 2MHz -> twice per M-cycle
				if (++ch->period >= 0x800) {
					ch->period = io->nrx3 | ((io->nrx4 & 7) << 8);
					ch->step = (ch->step + 1) % 32;
				}
			}
			static const byte volume[] = { 0, 4, 2, 1 }; // 0 = mute, 1 = 100%, 2 = 50%, 3 = 25% (in quarters)
			const byte level = (io->nrx2 >> 5) & 0x03;
			const int s = ((gb.io.wave[ch->step / 2] >> ((ch->step & 1) ? 0: 4)) & 0x0f) - 8; // upper nibble first
			return s * 4 * volume[level];
		}
		case 3: { // Noise Channel
			if (!update_length_and_envelope(ch, io, odiv)) return 0;
			if (ch->step == 0) {
				static const byte noise_divisor[] = { 2, 4, 8, 12, 16, 20, 24, 28 };
				ch->step = noise_divisor[io->nrx3 & 0x07] << (io->nrx3 >> 4);
				if ((io->nrx3 >> 4) < 14) {
					const word bit = (gb.apu.lfsr ^ (gb.apu.lfsr >> 1)) & 1;
					gb.apu.lfsr = (gb.apu.lfsr >> 1) | (bit << 14);
					if (io->nrx3 & BIT(3)) gb.apu.lfsr = (gb.apu.lfsr & ~0x40) | (bit << 6);
				}
			}
			ch->step--;
			return ((gb.apu.lfsr & 1) ? -16 : 16) * ch->volume;
		}
	}
	return 0; // fail-safe for compiler
}

// push average left/right samples through high-pass filter to audio buffer and update audio stream if it is full
static void output_sample(void) {
	static const int levels[] = { 32, 16, 8, 0 }; // global volume levels - controlled by F10
	for (int i = 0; i < 2; ++i) {
		const float in = (float)gb.apu.sample[i] / (float)(gb.apu.count * 8);
		gb.apu.hp_out[i] = in - gb.apu.hp_in[i] + 0.996f * gb.apu.hp_out[i]; gb.apu.hp_in[i] = in;
		gb.apu.buffer[gb.apu.pos++] = (int16_t)SDL_clamp(gb.apu.hp_out[i] * levels[app.volume % 4], -32768, 32767);
		gb.apu.sample[i] = 0;
	}
	gb.apu.count = 0;
	if (gb.apu.pos >= SDL_arraysize(gb.apu.buffer)) {
		gb.apu.pos = 0;
		if (SDL_GetAudioStreamAvailable(app.stream) > ((int)sizeof(gb.apu.buffer) * 16)) return; // prevent overflow of audio queue
		if (!SDL_PutAudioStreamData(app.stream, gb.apu.buffer, sizeof(gb.apu.buffer))) fail("SDL_PutAudioStreamData() error: %s", SDL_GetError());
	}
}

// run APU
static void run_apu(const bool edge) {
	// check if audio is enabled
	if (!(gb.io.nr52 & BIT(7))) for (int i = 0; i < 4; ++i) { gb.apu.chan[i] = (chan_t){0}; gb.io.ch[i] = (struct ioch_t){0}; }
	const word odiv = gb.apu.div;
	if (edge) gb.apu.div++;
	// iterate over all 4 channels
	int sample[2] = {0}; // left/right sample
	for (int i = 0; i < 4; ++i) {
		chan_t *ch = &gb.apu.chan[i]; struct ioch_t *io = &gb.io.ch[i];
		if (io->nrx4 & BIT(7)) trigger_channel(i, ch, io);
		if (ch->enabled) gb.io.nr52 |= BIT(i); else gb.io.nr52 &= ~BIT(i);
		if (!ch->enabled) continue;
		const int out = channel_sample(i, ch, io, odiv);
		if (app.channels & BIT(i)) continue; // ignore this sample when channel is disabled using F1-F4
		if (gb.io.nr51 & BIT(i + 4)) sample[0] += out;
		if (gb.io.nr51 & BIT(i)) sample[1] += out;
	}
	// apply left/right master volume (1-8) and add the computed samples to APU mixer
	gb.apu.sample[0] += sample[0] * (((gb.io.nr50 >> 4) & 0x07) + 1);
	gb.apu.sample[1] += sample[1] * ((gb.io.nr50 & 0x07) + 1);
	gb.apu.count++;
	// downsample to AUDIO_RATE from M-cycles
	gb.apu.accu += AUDIO_RATE;
	if (gb.apu.accu < CYCLE_RATE) return;
	gb.apu.accu -= CYCLE_RATE;
	output_sample();
}

// tick one M-cycle for the emulation
static void tick_cycle(void) {
	// update joypad inputs - this must be done every cycle (even in speed mode)
	byte btn = 0;
	if (!(gb.io.joyp & BIT(4))) btn |= app.buttons >> 4; // dpad buttons requested
	if (!(gb.io.joyp & BIT(5))) btn |= app.buttons & 0x0f; // action buttons requested
	gb.io.joyp = (gb.io.joyp & 0x30) | (btn ^ 0x0f);

	// turbo mode acts a 4x times faster CPU
	if (app.turbo_enabled) {
		if (++app.turbo_count < 4) return;
		app.turbo_count = 0;
	}

	// advance the M-cycles and DIV timer
	gb.timer.cycles++;
	const word odiv = gb.timer.div;
	gb.timer.div += 4;
	gb.io.div = (byte)(gb.timer.div >> 8);

	// check if timer is enabled and advance it
	if (gb.io.tac & BIT(2)) {
		static const byte clocks[] = { 9, 3, 5, 7 };
		if (falling_edge(odiv, gb.timer.div, clocks[gb.io.tac & 0x03]) && !++gb.io.tima) {
			gb.io.tima = gb.io.tma;
			request_interrupt(INT_TIMER);
		}
	}

	// run PPU + APU in sync
	run_ppu(); run_apu(falling_edge(odiv, gb.timer.div, 12));
}

// save battery ram
static void save_battery_ram(void) {
	if (!gb.mem.battery) return;
	const char *filename = strf("%ssave.%08x.ram", app.prefpath, gb.mem.rom_hash);
	if (!SDL_SaveFile(filename, gb.mem.xram, sizeof(gb.mem.xram))) {
		// don't use fail here, as we are inside the App_Quit !!!
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error!", strf("Failed to save battery RAM: %s", SDL_GetError()), app.window);
	}
}

// initialize the GameBoy
static void init_gameboy(const char *filename) {
	// prepare the GameBoy state and load the DMG register values after the boot ROM
	gb = (struct gb_t){
		.cpu = { .a = 0x01, .f = 0xb0, .b = 0x00, .c = 0x13, .d = 0x00, .e = 0xd8, .h = 0x01, .l = 0x4d, .sp = 0xfffe, .pc = 0x0100 },
		.timer = { .div = 0xabcc },
		.io = {
			.joyp = 0xcf, .sc = 0x7e, .div = 0xab, .tac = 0xf8, .ifr = 0xe1,
			.ch = { // NRx4 trigger bit (bit 7) is cleared, otherwise the channels would trigger on the first APU cycle
				{ 0x80, 0xbf, 0xf3, 0xff, 0x3f }, // NR10 - NR14
				{ 0xff, 0x3f, 0x00, 0xff, 0x3f }, // NR20 - NR24
				{ 0x7f, 0xff, 0x9f, 0xff, 0x3f }, // NR30 - NR34
				{ 0xff, 0xff, 0x00, 0x00, 0x3f }, // NR40 - NR44
			},
			.nr50 = 0x77, .nr51 = 0xf3, .nr52 = 0xf1,
			.lcdc = 0x91, .stat = 0x85, .dma = 0xff, .bgp = 0xfc, .obp = { 0xff, 0xff },
		},
	};
	// load the ROM file and setup banking
	size_t bytes;
	if (!(gb.mem.rom = SDL_LoadFile(filename, &bytes))) fail("Failed to load ROM: %s", SDL_GetError());
	if (bytes < KiB(32)) fail("ROM is smaller than 32KiB");
	if (bytes % KiB(16)) fail("ROM is not a multiple of 16KiB");
	if (bytes >= KiB(1024 *4)) fail("ROM is larger than 4MiB");
	gb.mem.rom_size = bytes / KiB(16);
	gb.mem.rom_base = KiB(16) * 1;
	if (!gb.mem.rom[0x014d]) gb.cpu.f = ZF; // H and C flags are only set if the header checksum is not zero
	// check for battery in cartridge - list of mappers with battery included
	static const bool bat[] = {[0x03]=true,[0x06]=true,[0x09]=true,[0x0d]=true,[0x0f]=true,[0x10]=true,[0x13]=true,[0x1b]=true,[0x1e]=true,[0xff]=true};
	gb.mem.battery = bat[gb.mem.rom[0x0147]];
	// calculate 32-bit fnv1a hash of the whole rom so we can use it for the save-files
	gb.mem.rom_hash = 2166136261;
	for (size_t i = 0; i < bytes; ++i) gb.mem.rom_hash = (gb.mem.rom_hash ^ gb.mem.rom[i]) * 16777619;
	// update window title
	const char *title = strf("%s - %.16s", WINDOW_TITLE, (const char*)&gb.mem.rom[0x0134]);
	SDL_SetWindowTitle(app.window, title);
	// load battery-ram if present
	if (!gb.mem.battery) return;
	const char *savename = strf("%ssave.%08x.ram", app.prefpath, gb.mem.rom_hash);
	SDL_IOStream *strm = SDL_IOFromFile(savename, "rb");
	if (!strm) return;
	SDL_ReadIO(strm, gb.mem.xram, sizeof(gb.mem.xram));
	SDL_CloseIO(strm);
}

// callback to initialize the application
SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
	(void)appstate;
	// init core system
	app = (struct app_t){0};
	if (setjmp(app.error)) return SDL_APP_FAILURE;
	if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) fail("SDL_Init() error: %s", SDL_GetError());
	if (!(app.prefpath = SDL_GetPrefPath("kieselsteini", "gb1k"))) fail("SDL_GetPrefPath() error: %s", SDL_GetError());
	// init video system
	const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
	int w = VIDEO_WIDTH, h = VIDEO_HEIGHT;
	if (dm) {
		const int factor = maxi(1, mini((dm->w * WINDOW_SCALE) / VIDEO_WIDTH, (dm->h * WINDOW_SCALE) / VIDEO_HEIGHT));
		w *= factor; h *= factor;
	}
	if (!(app.window = SDL_CreateWindow(WINDOW_TITLE, w, h, SDL_WINDOW_RESIZABLE))) fail("SDL_CreateWindow() error: %s", SDL_GetError());
	if (!(app.renderer = SDL_CreateRenderer(app.window, NULL))) fail("SDL_CreateRenderer() error: %s", SDL_GetError());
	if (!(app.texture = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, VIDEO_WIDTH, VIDEO_HEIGHT))) fail("SDL_CreateTexture() error: %s", SDL_GetError());
	if (!SDL_SetRenderVSync(app.renderer, 1)) fail("SDL_SetRenderVSync() error: %s", SDL_GetError());
	if (!SDL_SetRenderLogicalPresentation(app.renderer, VIDEO_WIDTH, VIDEO_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX)) fail("SDL_SetLogicalPresentation() error: %s", SDL_GetError());
	if (!SDL_SetTextureScaleMode(app.texture, SDL_SCALEMODE_PIXELART)) fail("SDL_SetTextureScaleMode() error: %s", SDL_GetError());
	if (!SDL_SetTextureBlendMode(app.texture, SDL_BLENDMODE_NONE)) fail("SDL_SetTextureBlendMode() error: %s", SDL_GetError());
	// init audio system
	const SDL_AudioSpec have = { .format = SDL_AUDIO_S16, .freq = AUDIO_RATE, .channels = 2 };
	if (!(app.device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL))) fail("SDL_OpenAudioDevice() error: %s", SDL_GetError());
	if (!(app.stream = SDL_CreateAudioStream(&have, NULL))) fail("SDL_CreateAudioStream() error: %s", SDL_GetError());
	if (!SDL_BindAudioStream(app.device, app.stream)) fail("SDL_BindAudioStream() error: %s", SDL_GetError());
	// init gameboy
	if (argc < 2) fail("Please specify a GameBoy ROM file.");
	init_gameboy(argv[1]);
	// init timer
	app.last = SDL_GetTicksNS();
	return SDL_APP_CONTINUE;
}

// callback to finalize the application
void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	(void)appstate; (void)result;
	save_battery_ram();
	if (gb.mem.rom) SDL_free(gb.mem.rom);
	if (app.stream) SDL_DestroyAudioStream(app.stream);
	if (app.device) SDL_CloseAudioDevice(app.device);
	if (app.texture) SDL_DestroyTexture(app.texture);
	if (app.renderer) SDL_DestroyRenderer(app.renderer);
	if (app.window) SDL_DestroyWindow(app.window);
	if (app.prefpath) SDL_free(app.prefpath);
	SDL_Quit();
}

// callback for every SDL event
SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
	(void)appstate; byte btn; bool down;
	switch (event->type) {
		default:
			return SDL_APP_CONTINUE;
		case SDL_EVENT_QUIT:
			return SDL_APP_SUCCESS;
		case SDL_EVENT_GAMEPAD_ADDED:
			SDL_OpenGamepad(event->gdevice.which);
			app.last = SDL_GetTicksNS(); // plug in gamepad take quite some time ...
			return SDL_APP_CONTINUE;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			down = event->type == SDL_EVENT_KEY_DOWN;
			switch (event->key.key) {
				default: return SDL_APP_CONTINUE;
				case SDLK_ESCAPE: return SDL_APP_SUCCESS;
				case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4: if (down) app.channels ^= BIT(event->key.key - SDLK_F1); return SDL_APP_CONTINUE;
				case SDLK_F5: if (down) app.turbo_enabled = !app.turbo_enabled; return SDL_APP_CONTINUE;
				case SDLK_F9: if (down) app.palette++; return SDL_APP_CONTINUE;
				case SDLK_F10: if (down) app.volume++; return SDL_APP_CONTINUE;
				case SDLK_W: case SDLK_8: case SDLK_KP_8: case SDLK_UP: btn = BTN_UP; break;
				case SDLK_S: case SDLK_2: case SDLK_KP_2: case SDLK_DOWN: btn = BTN_DOWN; break;
				case SDLK_A: case SDLK_4: case SDLK_KP_4: case SDLK_LEFT: btn = BTN_LEFT; break;
				case SDLK_D: case SDLK_6: case SDLK_KP_6: case SDLK_RIGHT: btn = BTN_RIGHT; break;
				case SDLK_Z: case SDLK_Y: case SDLK_I: case SDLK_RCTRL: case SDLK_LCTRL: btn = BTN_A; break;
				case SDLK_X: case SDLK_O: case SDLK_RALT: case SDLK_LALT: btn = BTN_B; break;
				case SDLK_K: case SDLK_RETURN: case SDLK_RETURN2: btn = BTN_START; break;
				case SDLK_L: case SDLK_SPACE: btn = BTN_SELECT; break;
			}
			break;
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			switch (event->gbutton.button) {
				default: return SDL_APP_CONTINUE;
				case SDL_GAMEPAD_BUTTON_DPAD_UP: btn = BTN_UP; break;
				case SDL_GAMEPAD_BUTTON_DPAD_DOWN: btn = BTN_DOWN; break;
				case SDL_GAMEPAD_BUTTON_DPAD_LEFT: btn = BTN_LEFT; break;
				case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: btn = BTN_RIGHT; break;
				case SDL_GAMEPAD_BUTTON_NORTH: btn = BTN_SELECT; break;
				case SDL_GAMEPAD_BUTTON_SOUTH: btn = BTN_A; break;
				case SDL_GAMEPAD_BUTTON_WEST: btn = BTN_START; break;
				case SDL_GAMEPAD_BUTTON_EAST: btn = BTN_B; break;
			}
			down = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
			break;
	}
	if (down) app.buttons |= btn; else app.buttons &= ~btn; // apply button mask
	return SDL_APP_CONTINUE;
}

// callback for every frame
SDL_AppResult SDL_AppIterate(void *appstate) {
	(void)appstate;
	if (setjmp(app.error)) return SDL_APP_FAILURE;
	// update the accumulated delta time
	const uint64_t now = SDL_GetTicksNS();
	app.accu += now - app.last;
	app.last = now;
	// do GameBoy emulation cycles
	for (; app.accu >= TICK_TIME; app.accu -= TICK_TIME) {
		const uint32_t start = gb.timer.cycles; // save current cycle count
		gb.ppu.ready = false; // reset PPU ready state
		while (((gb.timer.cycles - start) < CYCLE_COUNT) && (!gb.ppu.ready)) run_cpu(); // run until frame is ready or time is up
		gb.timer.cycles -= start; // subtract the previous saved cycle count
	}
	// update the screen
	if (gb.ppu.ready) {
		if (!SDL_UpdateTexture(app.texture, NULL, gb.ppu.buffer, VIDEO_WIDTH * 4)) fail("SDL_UpdateTexture() error: %s", SDL_GetError());
	}
	if (!SDL_RenderClear(app.renderer)) fail("SDL_RenderClear() error: %s", SDL_GetError());
	if (!SDL_RenderTexture(app.renderer, app.texture, NULL, NULL)) fail("SDL_RenderTexture() error: %s", SDL_GetError());
	if (!SDL_RenderPresent(app.renderer)) fail("SDL_RenderPresent() error: %s", SDL_GetError());
	return SDL_APP_CONTINUE;
}
