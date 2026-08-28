#pragma once
#include <stdint.h>
#include <stddef.h>

// VAX integer subset interpreter (Phase 2+).
// VAX_MODEL_KA630: MicroVAX II (KA630 / UV2). VAX_MODEL_KA750: 11/750 (experimental).
// Study reference: Open SIMH VAX/ (MIT) — local path in docs/research/isa.md.
// Not a full ISA yet.
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
  uint32_t scbb;    // System Control Block base (IPR 17)
  uint32_t pcbb;
  bool     halt;
  uint32_t fault;   // sticky: 1=bad opcode, 2=bad addr, 3=bad specifier
  uint32_t irq_count;  // delivered hardware interrupts (diag)
};

bool     init(size_t ram_bytes);
void     reset();
void     cold_boot();
void     run();              // clear halt and execute from current PC
void     resume();           // continue after host halt (do not reset counters)
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
uint32_t instr_count();  // monotonic; status-bar KIPS uses deltas

}  // namespace vax_cpu
