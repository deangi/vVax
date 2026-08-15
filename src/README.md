# Optional C++ layout notes
#
# Arduino IDE builds sketch-root `.ino` / `.cpp` only. Emulator stubs live at
# the sketch root for now (`vax_cpu.*`, `vax_mmu.*`, `vax_console.*`,
# `vax_clock.*`, `vax_mscp.*`, `eth_nat.*`).
#
# When the ISA grows, prefer moving implementation TUs under `src/` and
# adding sketch-root `#include "src/....cpp"` shims (same pattern as host_lib).
