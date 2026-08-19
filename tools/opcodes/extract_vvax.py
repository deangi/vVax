"""Extract implemented opcodes from vax_cpu.cpp main switch."""
from __future__ import annotations

import re
from pathlib import Path


def extract_vvax_opcodes(cpu_cpp: Path) -> dict[int, str]:
    text = cpu_cpp.read_text(encoding="utf-8", errors="replace")
    # Main execute switch: after "switch (op)" — take largest block of case 0xNN
    # Avoid addressing-mode switches by requiring hex opcode comments or 2-digit hex.
    m = re.search(r"switch\s*\(\s*op\s*\)\s*\{([\s\S]*)\n\s*default\s*:", text)
    if not m:
        raise RuntimeError("main switch(op) not found in vax_cpu.cpp")
    body = m.group(1)
    out: dict[int, str] = {}
    for cm in re.finditer(
        r"case\s+0x([0-9A-Fa-f]+)\s*:[^\n]*?(?://\s*([^\n]+))?",
        body,
    ):
        code = int(cm.group(1), 16)
        if code > 0xFF and code < 0x100:
            continue
        comment = (cm.group(2) or "").strip()
        mnemonic = comment.split("—")[0].split("-")[0].split(",")[0].strip()
        mnemonic = re.split(r"\s+", mnemonic)[0] if mnemonic else f"OP_{code:02X}"
        # Skip tiny addressing nibbles accidentally captured (shouldn't be in switch(op))
        if code <= 0x0F and mnemonic.startswith(("B^", "@", "W^", "L^")):
            continue
        out[code] = mnemonic
    return out
