"""Parse Open SIMH VAX opcode[] + drom[] from vax_sys.c / vax_defs.h macros."""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

IG_NAMES = {
    0: "RSVD",
    1: "BASE",
    2: "BSGFL",
    3: "BSDFL",
    4: "PACKD",
    5: "EXTAC",
    6: "EMONL",
    7: "VECTR",
}

# From vax_defs.h — values used when evaluating drom rows as Python exprs.
_DROM_ENV = {
    "IG_RSVD": 0 << 12,
    "IG_BASE": 1 << 12,
    "IG_BSGFL": 2 << 12,
    "IG_BSDFL": 3 << 12,
    "IG_PACKD": 4 << 12,
    "IG_EXTAC": 5 << 12,
    "IG_EMONL": 6 << 12,
    "IG_VECTR": 7 << 12,
    "DR_F": 0x80,
    "RB_0": 0 << 8,
    "RB_B": 1 << 8,
    "RB_W": 2 << 8,
    "RB_L": 3 << 8,
    "RB_Q": 4 << 8,
    "RB_O": 5 << 8,
    "RB_OB": 6 << 8,
    "RB_OW": 7 << 8,
    "RB_OL": 8 << 8,
    "RB_OQ": 9 << 8,
    "RB_R0": 10 << 8,
    "RB_R1": 11 << 8,
    "RB_R3": 12 << 8,
    "RB_R5": 13 << 8,
    "RB_SP": 14 << 8,
    "DR_BYTE": 0,
    "DR_WORD": 1,
    "DR_LONG": 2,
    "DR_QUAD": 3,
    "DR_OCTA": 4,
    "DR_SPFLAG": 0x008,
    "DR_ACMASK": 0x300,
    "RB": 0x000,  # placeholders; real RB/RW/… redefined below
}

# Operand access/length combos used in drom (vax_defs.h)
def _acc(ln: int, ac: int, sp: int = 0) -> int:
    return ac | sp | ln

# DR_R=0x000 DR_M=0x100 DR_A=0x200 DR_W=0x300; length in low 3 bits
_DROM_ENV.update(
    {
        "RB": _acc(0, 0x000),
        "RW": _acc(1, 0x000),
        "RL": _acc(2, 0x000),
        "RQ": _acc(3, 0x000),
        "RO": _acc(4, 0x000),
        "MB": _acc(0, 0x100),
        "MW": _acc(1, 0x100),
        "ML": _acc(2, 0x100),
        "MQ": _acc(3, 0x100),
        "MO": _acc(4, 0x100),
        "AB": _acc(0, 0x200),
        "AW": _acc(1, 0x200),
        "AL": _acc(2, 0x200),
        "AQ": _acc(3, 0x200),
        "AO": _acc(4, 0x200),
        "WB": _acc(0, 0x300),
        "WW": _acc(1, 0x300),
        "WL": _acc(2, 0x300),
        "WQ": _acc(3, 0x300),
        "WO": _acc(4, 0x300),
        "VB": 0x008 | _acc(0, 0x300),
        "RF": 0x008 | _acc(2, 0x000),
        "RD": 0x008 | _acc(3, 0x000),
        "RG": 0x008 | _acc(3, 0x100),
        "RH": 0x008 | _acc(4, 0x000),
        "BB": 0x008 | _acc(0, 0x300) | 6,
        "BW": 0x008 | _acc(0, 0x300) | 7,
    }
)

def _odc(x: int) -> int:
    # Match non-FULL_VAX: specifier count lives in USP field.
    return (x & 7) << 4


_DROM_ENV["ODC"] = _odc

BB = _DROM_ENV["BB"]
BW = _DROM_ENV["BW"]
VB = _DROM_ENV["VB"]
DR_SPFLAG = 0x008
DR_LNMASK = 0x007
NPC = 15


@dataclass
class SimhOp:
    code: int
    name: str | None
    group: int
    nspec: int
    specs: list[int]  # drom[1..]


def parse_opcode_names(vax_sys: Path) -> list[str | None]:
    text = vax_sys.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"char const \* const opcode\[\] = \{([\s\S]*?)\};", text)
    if not m:
        raise RuntimeError("opcode[] not found in vax_sys.c")
    body = m.group(1)
    names: list[str | None] = []
    for tok in re.findall(r"NULL|\"([^\"]*)\"", body):
        if tok == "NULL" or tok == "":
            # NULL matches as the whole token when pattern is NULL|"..."
            pass
    # Retokenize properly
    names = []
    for m2 in re.finditer(r"NULL|\"([^\"]*)\"", body):
        if m2.group(0) == "NULL":
            names.append(None)
        else:
            names.append(m2.group(1))
    if len(names) < 512:
        names.extend([None] * (512 - len(names)))
    return names[:512]


def _eval_cell(expr: str) -> int:
    expr = expr.strip()
    expr = re.sub(r"/\*.*?\*/", "", expr).strip()
    if not expr or expr == "0":
        return 0
    return int(eval(expr, {"__builtins__": {}}, _DROM_ENV))


def parse_drom(vax_sys: Path) -> list[list[int]]:
    text = vax_sys.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"const uint16 drom\[NUM_INST\]\[MAX_SPEC \+ 1\] = \{([\s\S]*?)\n\};", text)
    if not m:
        raise RuntimeError("drom[] not found in vax_sys.c")
    body = m.group(1)
    # Collapse #if/#else/#endif: keep #else branch when present, else keep #if body.
    def _pp(text_in: str) -> str:
        out = []
        i = 0
        while True:
            m_if = re.search(r"#if[^\n]*\n", text_in[i:])
            if not m_if:
                out.append(text_in[i:])
                break
            out.append(text_in[i : i + m_if.start()])
            start = i + m_if.end()
            rest = text_in[start:]
            m_else = re.search(r"#else[^\n]*\n", rest)
            m_endif = re.search(r"#endif[^\n]*\n", rest)
            if not m_endif:
                out.append(rest)
                break
            if m_else and m_else.start() < m_endif.start():
                # keep else..endif
                out.append(rest[m_else.end() : m_endif.start()])
            else:
                out.append(rest[: m_endif.start()])
            i = start + m_endif.end()
        return "".join(out)

    body = _pp(body)
    rows: list[list[int]] = []
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        rm = re.match(r"\{([^}]*)\}", line)
        if not rm:
            continue
        cells = [c.strip() for c in rm.group(1).split(",")]
        if len(cells) < 7:
            continue
        vals = [_eval_cell(c) for c in cells[:7]]
        rows.append(vals)
        if len(rows) >= 512:
            break
    if len(rows) < 512:
        raise RuntimeError(f"expected 512 drom rows, got {len(rows)}")
    return rows


def load_simh(simh_vax_dir: Path) -> list[SimhOp]:
    vax_sys = simh_vax_dir / "vax_sys.c"
    names = parse_opcode_names(vax_sys)
    drom = parse_drom(vax_sys)
    out: list[SimhOp] = []
    for i in range(512):
        flag = drom[i][0]
        nspec = flag & 0x07
        if nspec == 0:
            nspec = (flag >> 4) & 0x07
        group = (flag >> 12) & 0x07
        out.append(
            SimhOp(
                code=i,
                name=names[i],
                group=group,
                nspec=nspec if names[i] else 0,
                specs=drom[i][1:],
            )
        )
    return out


def spec_nbytes(data: bytes, off: int, disp: int) -> int | None:
    """Return number of bytes consumed by one operand starting at off, or None on failure."""
    if off >= len(data):
        return None
    if disp == BB:
        return 1 if off + 1 <= len(data) else None
    if disp == BW:
        return 2 if off + 2 <= len(data) else None

    start = off
    if off >= len(data):
        return None
    spec = data[off]
    off += 1
    if (spec & 0xF0) == 0x40:  # indexed
        if off >= len(data):
            return None
        spec = data[off]
        off += 1
    rn = spec & 0x0F
    mode = spec & 0xF0
    if mode <= 0x30:  # short literal
        return off - start
    if mode in (0x50, 0x60, 0x70):
        return off - start
    if mode == 0x80:  # autoinc / imm
        if rn == NPC:
            ln = 1 << (disp & DR_LNMASK)
            if (disp & DR_SPFLAG) and (disp & 7) not in (6, 7):
                # VB uses byte length
                ln = 1
            if disp == VB:
                ln = 1
            else:
                ln = 1 << (disp & DR_LNMASK)
            # VB is SPFLAG|WB — length byte
            if disp == VB:
                ln = 1
            elif (disp & DR_SPFLAG) and ((disp & 7) in (6, 7)):
                return None  # branch shouldn't use AIN
            else:
                ln = 1 << (disp & DR_LNMASK)
            if disp == VB:
                ln = 1
            off += ln
        return off - start if off <= len(data) else None
    if mode == 0x90:  # autoinc deferred / absolute
        if rn == NPC:
            off += 4
        return off - start if off <= len(data) else None
    if mode in (0xA0, 0xB0):
        off += 1
        return off - start if off <= len(data) else None
    if mode in (0xC0, 0xD0):
        off += 2
        return off - start if off <= len(data) else None
    if mode in (0xE0, 0xF0):
        off += 4
        return off - start if off <= len(data) else None
    return None


def insn_length(ops: list[SimhOp], data: bytes, pc: int) -> tuple[int, int] | None:
    """Return (opcode_index, total_length) or None if undecodable."""
    if pc >= len(data):
        return None
    inst = data[pc]
    consumed = 1
    if inst == 0xFD:
        if pc + 1 >= len(data):
            return None
        inst = 0x100 | data[pc + 1]
        consumed = 2
    op = ops[inst]
    if op.name is None:
        return None
    off = pc + consumed
    for i in range(op.nspec):
        disp = op.specs[i]
        if disp == 0 and i >= op.nspec:
            break
        # special-case VB length for AIN+PC
        n = _spec_nbytes_fixed(data, off, disp)
        if n is None:
            return None
        off += n
    return inst, off - pc


def _spec_nbytes_fixed(data: bytes, off: int, disp: int) -> int | None:
    if off >= len(data):
        return None
    if disp == BB:
        return 1 if off + 1 <= len(data) else None
    if disp == BW:
        return 2 if off + 2 <= len(data) else None

    start = off
    spec = data[off]
    off += 1
    if (spec & 0xF0) == 0x40:
        if off >= len(data):
            return None
        spec = data[off]
        off += 1
    rn = spec & 0x0F
    mode = spec & 0xF0
    if mode <= 0x30 or mode in (0x50, 0x60, 0x70):
        return off - start
    if mode == 0x80:
        if rn == NPC:
            if disp == VB:
                ln = 1
            else:
                ln = 1 << (disp & DR_LNMASK)
            off += ln
        return off - start if off <= len(data) else None
    if mode == 0x90:
        if rn == NPC:
            off += 4
        return off - start if off <= len(data) else None
    if mode in (0xA0, 0xB0):
        off += 1
    elif mode in (0xC0, 0xD0):
        off += 2
    elif mode in (0xE0, 0xF0):
        off += 4
    else:
        return None
    return off - start if off <= len(data) else None
