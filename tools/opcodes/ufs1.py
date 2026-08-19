"""Minimal UFS1 reader for NetBSD/vax boot disk (little-endian)."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

FS_MAGIC = 0x011954
ROOTINO = 2


@dataclass
class SuperBlock:
    bsize: int
    fsize: int
    iblkno: int
    dblkno: int
    fsbtodb: int
    ipg: int
    fpg: int
    inopb: int
    ncg: int
    magic: int


@dataclass
class Inode:
    mode: int
    size: int
    db: list[int]
    ib: list[int]


class Ufs1:
    def __init__(self, path: Path):
        self.path = path
        self.f = path.open("rb")
        self.sb = self._read_sb()
        self.bsize = self.sb.bsize

    def close(self) -> None:
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def read_at(self, off: int, n: int) -> bytes:
        self.f.seek(off)
        return self.f.read(n)

    def _read_sb(self) -> SuperBlock:
        raw = self.read_at(8192, 2048)
        magic = struct.unpack_from("<I", raw, 1372)[0]
        if magic != FS_MAGIC:
            raise RuntimeError(f"bad UFS magic {magic:#x}")
        # Offsets from NetBSD/FreeBSD struct fs (1376 bytes, magic at 1372)
        return SuperBlock(
            iblkno=struct.unpack_from("<i", raw, 16)[0],
            dblkno=struct.unpack_from("<i", raw, 20)[0],
            ncg=struct.unpack_from("<i", raw, 44)[0],
            bsize=struct.unpack_from("<i", raw, 48)[0],
            fsize=struct.unpack_from("<i", raw, 52)[0],
            fsbtodb=struct.unpack_from("<i", raw, 100)[0],
            inopb=struct.unpack_from("<i", raw, 120)[0],
            ipg=struct.unpack_from("<i", raw, 184)[0],
            fpg=struct.unpack_from("<i", raw, 188)[0],
            magic=magic,
        )

    def fsbtodb(self, fsblk: int) -> int:
        return fsblk << self.sb.fsbtodb

    def blk_off(self, fsblk: int) -> int:
        return self.fsbtodb(fsblk) * 512

    def cgstart(self, cg: int) -> int:
        # Simplified: contiguous CGs of fpg fragments (common for modern FFS)
        return cg * self.sb.fpg

    def inode_offset(self, ino: int) -> int:
        cg = ino // self.sb.ipg
        off_in_cg = ino % self.sb.ipg
        # inode blocks start at cgstart+iblkno (frag address)
        frag = self.cgstart(cg) + self.sb.iblkno + (off_in_cg // self.sb.inopb) * (
            self.sb.bsize // self.sb.fsize
        )
        return self.blk_off(frag) + (off_in_cg % self.sb.inopb) * 128

    def read_inode(self, ino: int) -> Inode:
        raw = self.read_at(self.inode_offset(ino), 128)
        mode = struct.unpack_from("<H", raw, 0)[0]
        # ufs1_dinode: di_size is ufs1_daddr-era 64-bit at offset 8 on NetBSD
        size = struct.unpack_from("<Q", raw, 8)[0]
        if size > (1 << 32):
            size = struct.unpack_from("<I", raw, 8)[0]
        db = list(struct.unpack_from("<12I", raw, 40))
        ib = list(struct.unpack_from("<3I", raw, 40 + 48))
        return Inode(mode=mode, size=int(size), db=db, ib=ib)

    def _read_fsblk(self, fsblk: int) -> bytes:
        if fsblk == 0:
            return b"\x00" * self.bsize
        return self.read_at(self.blk_off(fsblk), self.bsize)

    def read_file(self, ino: Inode) -> bytes:
        out = bytearray()
        remaining = ino.size

        def take(fsblk: int) -> None:
            nonlocal remaining
            if remaining <= 0:
                return
            n = min(self.bsize, remaining)
            if fsblk == 0:
                out.extend(b"\x00" * n)
            else:
                out.extend(self._read_fsblk(fsblk)[:n])
            remaining -= n

        for b in ino.db:
            take(b)
        if remaining > 0 and ino.ib[0]:
            ptrs = struct.unpack("<" + "I" * (self.bsize // 4), self._read_fsblk(ino.ib[0]))
            for p in ptrs:
                if remaining <= 0:
                    break
                take(p)
        if remaining > 0 and ino.ib[1]:
            ptrs2 = struct.unpack("<" + "I" * (self.bsize // 4), self._read_fsblk(ino.ib[1]))
            for p2 in ptrs2:
                if remaining <= 0:
                    break
                if p2 == 0:
                    continue
                ptrs = struct.unpack("<" + "I" * (self.bsize // 4), self._read_fsblk(p2))
                for p in ptrs:
                    if remaining <= 0:
                        break
                    take(p)
        return bytes(out[: ino.size])

    def lookup(self, path: str) -> Inode | None:
        parts = [p for p in path.strip("/").split("/") if p]
        ino_num = ROOTINO
        for name in parts:
            din = self.read_inode(ino_num)
            if (din.mode & 0xF000) != 0x4000:
                return None
            data = self.read_file(din)
            found = None
            off = 0
            while off + 8 <= len(data):
                ino, reclen, type_, namlen = struct.unpack_from("<IHBB", data, off)
                if reclen == 0:
                    break
                n = data[off + 8 : off + 8 + namlen].split(b"\x00")[0].decode("ascii", "replace")
                if ino and n == name:
                    found = ino
                    break
                off += reclen
            if found is None:
                return None
            ino_num = found
        return self.read_inode(ino_num)

    def list_root(self) -> list[str]:
        din = self.read_inode(ROOTINO)
        data = self.read_file(din)
        names = []
        off = 0
        while off + 8 <= len(data):
            ino, reclen, type_, namlen = struct.unpack_from("<IHBB", data, off)
            if reclen == 0:
                break
            if ino and namlen:
                n = data[off + 8 : off + 8 + namlen].split(b"\x00")[0].decode("ascii", "replace")
                names.append(n)
            off += reclen
        return names
