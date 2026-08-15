#pragma once
#include <stdint.h>
#include <stddef.h>

// MicroVAX II–class integer subset interpreter (Phase 2).
// Study reference: Open SIMH VAX/ (MIT). Not a full ISA yet.
namespace vax_cpu {

enum {
  R_AP = 12,
  R_FP = 13,
  R_SP = 14,
  R_PC = 15
};

// PSL condition codes (low nibble).
enum {
  PSL_C = 0x01,
  PSL_V = 0x02,
  PSL_Z = 0x04,
  PSL_N = 0x08
};

// Host console MMIO: MOVB to this PA emits via vax_console::put_guest.
static constexpr uint32_t CONSOLE_TX_PA = 0x20000000u;

struct State {
  uint32_t r[16];   // R0–R11, AP, FP, SP, PC
  uint32_t psl;
  bool     halt;
  uint32_t fault;   // sticky: 1=bad opcode, 2=bad addr, 3=bad specifier
};

bool     init(size_t ram_bytes);
void     reset();
void     cold_boot();
void     step(unsigned n);
bool     running();
void     request_halt();

// Load bytes into guest RAM; returns false if out of range.
bool     load(uint32_t pa, const uint8_t* data, size_t len);

// Run built-in Phase 2 self-test (prints to VT100; returns true on PASS).
bool     selftest();

uint8_t* ram();
size_t   ram_bytes();
State&   state();

}  // namespace vax_cpu
