"""Tier A linear disassembly + Tier B allowlist + coverage compare."""
from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path

from extract_vvax import extract_vvax_opcodes
from simh_tables import IG_NAMES, insn_length, load_simh
from ufs1 import Ufs1

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SIMH = Path(
    r"C:\Users\deang\OneDrive\emulators\open-simh-simh-open-simh-baseline-448-ga1f57fa"
    r"\open-simh-simh-a1f57fa\VAX"
)
DEFAULT_DISK = ROOT / "vVaxSdCard" / "disks" / "netbsd101-boot.dsk"
OUT = Path(__file__).resolve().parent / "out"

# Product won't-implement (Tier D) — excluded from Tier B even if IG_BASE.
TIER_D = {
    0x0B,  # CRC (also EMONL)
    0x38,  # EDITPC
    0xFC,  # XFC
}

# CIS / string ops that stay Tier C unless Tier A proves need (still IG_BASE).
# Keep MOVC3/MOVC5 in B (already implemented / essential). Others optional B.
TIER_C_CIS = {
    0x29,  # CMPC3
    0x2A,  # SCANC
    0x2B,  # SPANC
    0x2D,  # CMPC5
    0x2E,  # MOVTC
    0x2F,  # MOVTUC
    0x39,  # MATCHC
    0x3A,  # LOCC
    0x3B,  # SKPC
}


def linear_histogram(ops, blob: bytes, start: int = 0) -> Counter:
    hist: Counter = Counter()
    pc = start
    bad = 0
    while pc < len(blob):
        r = insn_length(ops, blob, pc)
        if r is None:
            pc += 1
            bad += 1
            continue
        inst, ln = r
        hist[inst] += 1
        pc += ln
    hist["_bad_resync"] = bad  # type: ignore
    return hist


def reachable_histogram(ops, blob: bytes, entries: list[int]) -> Counter:
    """Follow BB/BW/BR/BSB/JSB/JMP/CALL* targets when PC-relative resolvable."""
    hist: Counter = Counter()
    seen_pc: set[int] = set()
    queue = list(entries)
    while queue:
        pc = queue.pop()
        if pc < 0 or pc >= len(blob) or pc in seen_pc:
            continue
        seen_pc.add(pc)
        while pc < len(blob):
            if pc in seen_pc and hist:
                # allow fall-through revisit stop only if already decoded here as start
                pass
            r = insn_length(ops, blob, pc)
            if r is None:
                break
            inst, ln = r
            hist[inst] += 1
            nxt = pc + ln
            # branch helpers
            opb = blob[pc]
            if opb == 0xFD:
                pass
            elif opb in (
                0x10,
                0x11,
                0x12,
                0x13,
                0x14,
                0x15,
                0x18,
                0x19,
                0x1A,
                0x1B,
                0x1C,
                0x1D,
                0x1E,
                0x1F,
            ):
                disp = int.from_bytes(blob[pc + 1 : pc + 2], "little", signed=True)
                tgt = nxt + disp
                queue.append(tgt)
                if opb in (0x11,):  # BRB — no fallthrough
                    break
            elif opb in (0x30, 0x31):
                disp = int.from_bytes(blob[pc + 1 : pc + 3], "little", signed=True)
                tgt = nxt + disp
                queue.append(tgt)
                if opb == 0x31:
                    break
            seen_pc.add(pc)
            pc = nxt
            # stop at unconditional returns
            if opb in (0x04, 0x05, 0x00, 0x02):  # RET RSB HALT REI
                break
    return hist


def build_tier_b(ops) -> dict[int, str]:
    """MicroVAX-class integer/base allowlist: IG_BASE, single-byte, minus D/C float/CIS/packed."""
    floatish = re.compile(
        r"^(ADD|SUB|MUL|DIV|CVT|MOV|CMP|MNEG|TST|EMOD|POLY|ACB)[FDGH]|"
        r"^CVT[BWL][FDGH]|^CVT[FDGH][BWL]|^CVTR[FDGH]L|^CVTFD$"
    )
    allow: dict[int, str] = {}
    for op in ops:
        if op.code >= 0x100 or op.name is None:
            continue
        if op.group != 1:  # IG_BASE
            continue
        if op.code in TIER_D or op.code in TIER_C_CIS:
            continue
        if floatish.match(op.name):
            continue
        # Packed-decimal mnemonics that are not IG_BASE after parse fix, but belt/suspenders
        if op.name.endswith("P") and op.name not in ("PUSHR", "POPR", "BISPSW", "BICPSW"):
            if op.name in ("ASHP", "MOVP", "CMPP3", "CMPP4", "ADDP4", "ADDP6", "SUBP4", "SUBP6", "MULP", "DIVP", "CVTPS", "CVTSP", "CVTPT", "CVTTP", "CVTPL", "CVTLP"):
                continue
        allow[op.code] = op.name
    for c in (0x28, 0x2C):
        if ops[c].name:
            allow[c] = ops[c].name
    return allow


def extract_tier_a_blobs(disk: Path) -> dict[str, bytes]:
    blobs: dict[str, bytes] = {}
    with disk.open("rb") as f:
        blobs["xxboot_lba0_15"] = f.read(16 * 512)
    with Ufs1(disk) as fs:
        try:
            print("root:", fs.list_root()[:40])
            print("sb", fs.sb)
        except Exception as e:
            print("ufs list_root failed:", e)
        for path in ("/boot", "/boot.vax", "/netbsd", "/netbsd.VAB", "/netbsd.gz"):
            try:
                ino = fs.lookup(path)
            except Exception as e:
                print(f"lookup {path}: {e}")
                ino = None
            if ino is None:
                continue
            try:
                data = fs.read_file(ino)
            except Exception as e:
                print(f"warn: read {path}: {e}")
                continue
            if data:
                blobs[path] = data
                print(f"extracted {path}: {len(data)} bytes mode={ino.mode:#x}")
    return blobs


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    ops = load_simh(DEFAULT_SIMH)
    vvax = extract_vvax_opcodes(ROOT / "vax_cpu.cpp")
    tier_b = build_tier_b(ops)

    (OUT / "vvax_implemented.json").write_text(
        json.dumps({f"0x{k:02X}": v for k, v in sorted(vvax.items())}, indent=2) + "\n",
        encoding="utf-8",
    )
    (OUT / "tier_b_allowlist.json").write_text(
        json.dumps({f"0x{k:02X}": v for k, v in sorted(tier_b.items())}, indent=2) + "\n",
        encoding="utf-8",
    )

    simh_base = {
        op.code: op.name
        for op in ops
        if op.name and op.group == 1 and op.code < 0x100
    }
    (OUT / "simh_ig_base.json").write_text(
        json.dumps({f"0x{k:02X}": v for k, v in sorted(simh_base.items())}, indent=2) + "\n",
        encoding="utf-8",
    )

    blobs = extract_tier_a_blobs(DEFAULT_DISK)
    tier_a: Counter = Counter()
    per_blob = {}
    for name, blob in blobs.items():
        # Prefer reachable from likely entries; also union linear for xxboot
        h = Counter()
        if name.startswith("xxboot"):
            # NetBSD xxboot: primary at 0; after load PC often 0x0C
            h = reachable_histogram(ops, blob, [0, 0x0C, 0x200])
            h2 = linear_histogram(ops, blob)
            for k, v in h2.items():
                if isinstance(k, int):
                    h[k] = max(h[k], v)
        else:
            # ELF? NetBSD/vax boot may be a.out or raw. Try ELF
            entries = [0]
            if blob[:4] == b"\x7fELF":
                # e_entry at 0x18 for ELF32 LE
                entry = int.from_bytes(blob[0x18:0x1C], "little")
                # For ET_EXEC load addr — use file offset heuristic: scan PT_LOAD
                entries = [0]
                phoff = int.from_bytes(blob[0x1C:0x20], "little")
                phentsize = int.from_bytes(blob[0x2A:0x2C], "little")
                phnum = int.from_bytes(blob[0x2C:0x2E], "little")
                for i in range(phnum):
                    off = phoff + i * phentsize
                    p_type = int.from_bytes(blob[off : off + 4], "little")
                    if p_type != 1:
                        continue
                    p_offset = int.from_bytes(blob[off + 4 : off + 8], "little")
                    p_vaddr = int.from_bytes(blob[off + 8 : off + 12], "little")
                    if p_vaddr <= entry < p_vaddr + int.from_bytes(blob[off + 16 : off + 20], "little"):
                        entries = [entry - p_vaddr + p_offset]
                        break
                h = reachable_histogram(ops, blob, entries)
                # Also linear-sweep executable PT_LOAD filesz
                for i in range(phnum):
                    off = phoff + i * phentsize
                    p_type = int.from_bytes(blob[off : off + 4], "little")
                    if p_type != 1:
                        continue
                    flags = int.from_bytes(blob[off + 24 : off + 28], "little")
                    if flags & 1 == 0:
                        continue
                    p_offset = int.from_bytes(blob[off + 4 : off + 8], "little")
                    p_filesz = int.from_bytes(blob[off + 16 : off + 20], "little")
                    h2 = linear_histogram(ops, blob[p_offset : p_offset + p_filesz])
                    for k, v in h2.items():
                        if isinstance(k, int):
                            h[k] += v
            else:
                # a.out midmag / raw
                h = linear_histogram(ops, blob)
                h2 = reachable_histogram(ops, blob, [0, 0x1000, 0x2000])
                for k, v in h2.items():
                    if isinstance(k, int):
                        h[k] = max(h[k], v)
        per_blob[name] = {f"0x{k:02X}": v for k, v in sorted(h.items()) if isinstance(k, int)}
        for k, v in h.items():
            if isinstance(k, int):
                tier_a[k] += v

    (OUT / "tier_a_histogram.json").write_text(
        json.dumps(
            {
                "per_blob": per_blob,
                "union": {
                    f"0x{k:02X}": {"name": ops[k].name, "count": c}
                    for k, c in sorted(tier_a.items(), key=lambda kv: -kv[1])
                    if ops[k].name
                },
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    def missing(need: dict[int, str]) -> list[dict]:
        rows = []
        for code, name in sorted(need.items()):
            if code not in vvax:
                rows.append(
                    {
                        "op": f"0x{code:02X}",
                        "name": name,
                        "group": IG_NAMES[ops[code].group],
                        "tier_a_count": tier_a.get(code, 0),
                    }
                )
        rows.sort(key=lambda r: (-r["tier_a_count"], r["op"]))
        return rows

    tier_a_need = {c: ops[c].name for c in tier_a if ops[c].name}
    miss_a = missing(tier_a_need)
    miss_b = missing(tier_b)
    miss_a_or_b = missing({**tier_b, **tier_a_need})

    miss_ab = [r for r in miss_a if int(r["op"], 16) in tier_b]
    miss_c = [r for r in miss_a if int(r["op"], 16) not in tier_b]
    report = {
        "vvax_count": len(vvax),
        "tier_a_unique": len(tier_a_need),
        "tier_b_allowlist": len(tier_b),
        "tier_a_missing": miss_a,
        "tier_b_missing": miss_b,
        "tier_a_or_b_missing": miss_a_or_b,
        "tier_a_and_b_missing": miss_ab,
        "tier_a_deferred_c": miss_c,
        "blobs": {k: len(v) for k, v in blobs.items()},
    }
    (OUT / "coverage_report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# Opcode coverage (generated)",
        "",
        f"- vVax implemented: **{len(vvax)}**",
        f"- Tier A unique (static): **{len(tier_a_need)}**",
        f"- Tier B allowlist (MicroVAX integer/base): **{len(tier_b)}**",
        f"- Tier B missing: **{len(miss_b)}**",
        f"- Tier A∩B missing (presence gate): **{len(miss_ab)}**",
        f"- Tier A deferred (float/CIS/packed/…): **{len(miss_c)}**",
        "",
        "Presence gate: **Tier B empty**. Remaining Tier A hits are Tier C/D "
        "(F/D float, packed decimal, heavy CIS, G/H) unless a boot path proves otherwise.",
        "",
        "## Blobs",
        "",
    ]
    for k, n in report["blobs"].items():
        lines.append(f"- `{k}`: {n} bytes")
    lines += ["", "## Tier B missing", ""]
    if not miss_b:
        lines.append("_none_")
    for r in miss_b:
        lines.append(f"- `{r['op']}` {r['name']} (tier_a_count={r['tier_a_count']})")
    lines += ["", "## Tier A deferred (Tier C/D candidates, by frequency)", ""]
    for r in miss_c:
        lines.append(
            f"- `{r['op']}` {r['name']} [{r['group']}] (count={r['tier_a_count']})"
        )
    md_path = ROOT / "docs" / "research" / "opcode_coverage.md"
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT}")
    print(f"wrote {md_path}")
    print(f"A_deferred={len(miss_c)} B_missing={len(miss_b)} A_and_B={len(miss_ab)}")

if __name__ == "__main__":
    main()
