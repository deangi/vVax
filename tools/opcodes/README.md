# Opcode coverage tools

Host-side Python inventory for vVax vs Open SIMH `opcode[]`/`drom` and static
disassembly of NetBSD boot media.

## Run

```text
python tools/opcodes/run_coverage.py
```

Requires local Open SIMH VAX tree (path in `run_coverage.py`) and
`vVaxSdCard/disks/netbsd101-boot.dsk`.

## Outputs

- `tools/opcodes/out/*.json` — implemented set, Tier B allowlist, histograms
- `docs/research/opcode_coverage.md` — human summary

## Tiers

| Tier | Meaning |
|------|---------|
| A | Opcodes seen in xxboot / `/boot` / `/netbsd` static decode |
| B | Curated MicroVAX integer/`IG_BASE` allowlist (presence gate) |
| C | F/D float, remaining CIS (only if binaries need) |
| D | Won’t-implement (G/H, vector, most packed, …) |
