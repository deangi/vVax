#pragma once
#include <stdint.h>

// Host-side NetBSD xxboot handoff (FROM750-style, no proprietary KA630 ROM).
namespace vax_boot {

// Magic JSB target used as R6 "ROM disk read" for 11/750 ABI.
static constexpr uint32_t ROM_READ_PA = 0x20010000u;

// Fake KA630 console-page ROM vectors (NetBSD ka630_rom_* JSB targets).
static constexpr uint32_t KA630_PUTC_POLL_PA = 0x20010010u;
static constexpr uint32_t KA630_PUTC_PA      = 0x20010014u;
static constexpr uint32_t KA630_GETC_PA      = 0x20010018u;

// Guest RAM console page + TOD NVR that points at it (ka630_consinit).
// Do NOT use 0xF2000: loadfile copies the kernel to PA 0 (KERNBASE) and a
// ~3.5 MB image overwrites that page; cnputc then JSBs through smashed
// vectors (pa-r at a wild PC, /boot SP still live). Planted at the last
// VAX page of guest RAM — above /boot, outside the kernel window.
uint32_t ka630_conspage_pa();
static constexpr uint32_t KA630_NVR_PA = 0x200B8024u;  // KA630_NVR_ADRS

static constexpr uint32_t BDEV_UDA   = 17u;
static constexpr uint32_t BOOT_BLOCKS = 16u;  // LBA 0..15
static constexpr uint32_t BOOT_BYTES  = BOOT_BLOCKS * 512u;

// After selftests: load xxboot from MSCP unit, set bootregs, PC=0x0C, run.
bool start_mscp(uint8_t unit);  // 0 = A (dua), 1 = B (dub)

uint8_t boot_unit();

// CPU JSB hook: 750 ROM disk read — R8=LBN, 4(SP)=dest PA; R0 bit0 = OK.
void rom_disk_read();

// Plant conspage + NVR pointer so UV2 /boot uses host console, not missing ROM.
void plant_ka630_console();

// machdep_start niclose is DEUNA teardown; stub to RET (no DEUNA on vVax).
void plant_boot_stubs(uint32_t boot_base);

// CALLS/JSB target is niclose (patched or not).
bool is_niclose(uint32_t pa);

// NVR nibble/byte for KA630_NVR_ADRS reads (conspage address, low bytes of shorts).
bool nvr_hit(uint32_t pa);
uint8_t nvr_read8(uint32_t pa);

// JSB hooks for fake KA630 console ROM. Returns true if handled (PC already restored).
bool ka630_console_jsb(uint32_t target);

}  // namespace vax_boot
