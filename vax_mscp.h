#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

// RQDX3 / UDA50-class MSCP port: dual units (dua/dub) on SD images.
// KA630: Q22 CSR 0172150/0172152. /boot RPB uses 0x20001C68; kernel uba maps
// the 8K I/O page as 0x20000000+(csr-0160000) → 0x20001468. Both must decode.
// KA750: Unibus 0172150 at 0xFFF468 / 0xFFF46A only. No Q22 dual-alias.
namespace vax_mscp {

enum Unit : uint8_t { UNIT_A = 0, UNIT_B = 1, UNIT_COUNT = 2 };

#if VAX_MODEL == VAX_MODEL_KA750
static constexpr uint32_t CSR_IP_PA = 0x00FFF468u;  // IOPAGEBASE+0x1468
static constexpr uint32_t CSR_SA_PA = 0x00FFF46Au;
#else
static constexpr uint32_t CSR_IP_PA = 0x20001C68u;
static constexpr uint32_t CSR_SA_PA = 0x20001C6Au;
#endif

// Diagnostic dump categories for [diag] mscp_dump_flags=
enum DumpFlags : uint32_t {
  DUMP_CSR  = 0x01,  // IP/SA register access
  DUMP_INIT = 0x02,  // UQSSP init step transitions
  DUMP_RING = 0x04,  // ring descriptor fetch/release
  DUMP_CMD  = 0x08,  // MSCP command opcodes
  DUMP_XFER = 0x10,  // read/write transfer chunks
  DUMP_IRQ  = 0x20,  // interrupt latch / vector
  DUMP_ALL  = 0x3F
};

// Guest physical memory callbacks for ring/packet DMA.
using PhysRead8Fn  = uint8_t (*)(uint32_t pa);
using PhysWrite8Fn = void (*)(uint32_t pa, uint8_t v);

void set_phys_ops(PhysRead8Fn rd, PhysWrite8Fn wr);

// Bounded USB-serial diagnostics. count=0 disables; each log line consumes one.
void set_dump(uint32_t flags, uint32_t count);
uint32_t dump_flags();
uint32_t dump_remaining();
// Parse "csr,init,ring,cmd,xfer,irq,all" or "0x3F" / decimal.
uint32_t parse_dump_flags(const char* s);

void begin();
void reset();                 // soft: keep mounts; hard init via IP write
bool mount(Unit u, const char* path);
void unmount(Unit u);
bool mounted(Unit u);
bool readonly(Unit u);
const char* path(Unit u);
uint64_t size_bytes(Unit u);

bool read_blocks(Unit u, uint32_t lba, void* buf, size_t nblocks);
bool write_blocks(Unit u, uint32_t lba, const void* buf, size_t nblocks);

// Controller register access (16-bit IP/SA).
uint16_t csr_read(uint32_t pa);
void     csr_write(uint32_t pa, uint16_t v);
bool     csr_hit(uint32_t pa);

void poll();                  // init service + ring transport
void instr_tick();            // expire deferred response IRQ (lost-wakeup)

bool irq_pending();
bool busy();                  // cmd, xfer, rsp, IRQ, or deferred IRQ
bool unit_busy(Unit u);       // this unit has an in-flight or recent media xfer
// ONLINE posted; host has not queued another command yet (ra_putonline tsleep).
bool host_online_wait();
uint16_t irq_vector();        // SCB byte offset; kernel uda probe uses 0x1FC
void irq_clear();             // host ack after delivery

bool selftest();              // init steps 1–4 + GO

}  // namespace vax_mscp
