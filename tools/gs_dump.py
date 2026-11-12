"""Lists what a GS dump of the game draws (MKSM_GS_DUMP, src/trace_gs_dump.cpp).

    python tools/gs_dump.py <dump> [--frame N] [--batches] [--uploads] [--mip] [--where x,y] [--textures <folder>]
                             [--verts B] [--targets <folder>]

The dump is the GIF packets the GS received during a few vertical blanks, in order. This tool decodes the GIF tags
(PACKED, REGLIST, IMAGE), follows the GS registers, and prints, per frame (a frame starts at each PATH3 packet that
sets FRAME_1, which is libgraph's drawing environment):

  default      a summary: the draw batches grouped by target buffer, texture, blending and test settings
  --batches    every draw batch in order (a batch is a run of primitives between two state changes)
  --uploads    the texture uploads (BITBLTBUF / TRXPOS / TRXREG / TRXDIR and their image data)
  --mip        with --batches: the mipmap state of each textured batch (TEX1's LCM, MXL, L and K, the level of
               detail its Q range selects, the MIPTBP levels)
  --where x,y  only the batches whose bounding box covers that pixel of the target
  --verts B    the vertices of batch B of the chosen frame: position, Z, colour, S, T, Q and the texel S/Q, T/Q
  --textures <folder>
               follows the uploads into a model of the GS memory and writes, for every textured batch of the chosen
               frame, the texture as the GS reads it when the batch starts: f<frame>_b<batch>_<tbp>_L<level>.png
               (every mipmap level the batch can use). The memory starts from the dump's first memory record; without
               one it starts empty, and a texture uploaded before the dump began comes out black.
  --targets <folder>
               needs a dump made with MKSM_GS_DUMP_VRAM=<n>: at every memory record, writes the buffers that were
               drawn into since the record before it, as the backend holds them: p<packet>_fb<fbp>_<psm>.png. The
               list printed with it says which batches each record follows.

It needs nothing but Python. The dump itself holds game data: keep it outside the repository.
"""
import os
import struct
import sys
import zlib
from collections import OrderedDict

PRIM_NAMES = ["point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"]
PSM_NAMES = {0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S", 0x13: "T8", 0x14: "T4", 0x1B: "T8H", 0x24: "T4HL",
             0x2C: "T4HH", 0x30: "Z32", 0x31: "Z24", 0x32: "Z16", 0x3A: "Z16S"}
VERTS_PER_PRIM = [1, 2, 2, 3, 3, 3, 2, 1]


def bits(value, low, count):
    return (value >> low) & ((1 << count) - 1)


# ---- GS local memory: where pixel (x, y) of a buffer is, per pixel storage format ---------------------------------
# A page is 8192 bytes, a block 256, a column 64. The tables give the offset of a pixel inside its page: in bytes for
# the 32, 24 and 16 bit formats and for T8, in nibbles for T4.
def _page_table(kind):
    if kind == "32":
        pw, ph, bw_, bh, ch = 64, 32, 8, 8, 2
    elif kind in ("16", "16S"):
        pw, ph, bw_, bh, ch = 64, 64, 16, 8, 2
    elif kind == "8":
        pw, ph, bw_, bh, ch = 128, 64, 16, 16, 4
    else:
        pw, ph, bw_, bh, ch = 128, 128, 32, 16, 4
    table = []
    for y in range(ph):
        row = []
        for x in range(pw):
            bx, by = x // bw_, y // bh
            column = (y % bh) // ch
            px, py = x % bw_, y % ch
            if kind in ("32", "8"):
                block = (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2)
            elif kind == "16S":
                block = (by & 1) | ((bx & 1) << 1) | (by & 4) | ((by & 2) << 2) | ((bx & 2) << 3)
            else:
                block = (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2)
            if kind == "32":
                pixel = (px & 1) | ((py & 1) << 1) | ((px & 2) << 1) | ((px & 4) << 1)
                row.append((block, column * 64 + pixel * 4))
            elif kind in ("16", "16S"):
                pixel = ((px & 8) >> 3) | ((px & 1) << 1) | ((py & 1) << 2) | ((px & 2) << 2) | ((px & 4) << 2)
                row.append((block, column * 64 + pixel * 2))
            elif kind == "8":
                pixel = (((py & 2) >> 1) | ((px & 8) >> 2) | ((px & 1) << 2) | ((py & 1) << 3) | ((px & 2) << 3)
                         | ((((px & 4) << 3) ^ ((py & 2) << 4)) ^ ((column & 1) << 5)))
                row.append((block, column * 64 + pixel))
            else:
                pixel = (((py & 2) >> 1) | ((px & 8) >> 2) | ((px & 0x10) >> 2) | ((px & 1) << 3) | ((py & 1) << 4)
                         | ((px & 2) << 4) | ((((px & 4) << 4) ^ ((py & 2) << 5)) ^ ((column & 1) << 6)))
                row.append((block, column * 64 * 2 + pixel))
        table.append(row)
    return pw, ph, table


_TABLES = {}
PSM_KIND = {0x00: "32", 0x01: "32", 0x02: "16", 0x0A: "16S", 0x13: "8", 0x14: "4", 0x1B: "32", 0x24: "32", 0x2C: "32",
            0x30: "32", 0x31: "32", 0x32: "16", 0x3A: "16S"}
PSM_Z = (0x30, 0x31, 0x32, 0x3A)     # the depth formats: the block number is flipped (xor 0x18) after the base is added


def pixel_address(x, y, bp, bw, psm):
    """Offset of pixel (x, y) of the buffer at block bp, width bw (in 64 pixels): bytes, or nibbles for T4."""
    kind = PSM_KIND.get(psm, "32")
    if kind not in _TABLES:
        _TABLES[kind] = _page_table(kind)
    pw, ph, table = _TABLES[kind]
    stride = bw >> 1 if kind in ("8", "4") else bw
    page = (y // ph) * stride + (x // pw)
    block, inside = table[y % ph][x % pw]
    block += bp
    if psm in PSM_Z:
        block ^= 0x18
    base = page * 8192 + block * 256
    if kind == "4":
        return (base * 2 + inside) & 0x7FFFFF
    return (base + inside) & 0x3FFFFF


def write_png(path, width, height, rgba):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                  + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


class Gs:
    def __init__(self):
        self.reg = {}
        self.prim = 0
        self.rgbaq = (0, 0, 0, 0, 1.0)
        self.st = (0.0, 0.0)
        self.uv = (0, 0)
        self.q = 1.0
        self.fog = 0
        self.frames = []
        self.frame = None
        self.batch = None
        self.upload = None
        self.vertex_queue = 0
        self.packet_index = 0
        self.path = 0
        self.keep_vertices = False
        self.target_dir = None
        self.targets_drawn = OrderedDict()   # (fbp, fbw, psm, width, height) -> the batches since the last record
        self.vram_records = 0
        self.vram = None            # bytearray(4 MB) with --textures
        self.texture_dir = None
        self.texture_frame = None
        self.transfer = None        # the host to local transfer in progress: [pixel index, leftover bytes]

    # ---- frames and batches -------------------------------------------------------------------------------
    def new_frame(self, tick):
        self.end_batch()
        self.frame = {"tick": tick, "batches": [], "uploads": [], "packets": 0, "bytes": {1: 0, 2: 0, 3: 0}}
        self.frames.append(self.frame)

    def ensure_frame(self, tick):
        if self.frame is None:
            self.new_frame(tick)

    def end_batch(self):
        if self.batch is not None and self.batch["kicks"] + self.batch["skipped"] > 0:
            self.frame["batches"].append(self.batch)
        self.batch = None

    def context(self):
        return bits(self.prim, 9, 1)

    def state_key(self):
        ctx = self.context()
        r = self.reg
        tme = bits(self.prim, 4, 1)
        return (ctx, self.prim & 0x7FF, r.get(0x4C + ctx, 0), r.get(0x4E + ctx, 0), r.get(0x06 + ctx, 0) if tme else 0,
                r.get(0x14 + ctx, 0) if tme else 0, r.get(0x42 + ctx, 0), r.get(0x47 + ctx, 0), r.get(0x18 + ctx, 0),
                r.get(0x40 + ctx, 0), r.get(0x08 + ctx, 0) if tme else 0, r.get(0x3B, 0) if tme else 0,
                r.get(0x34 + ctx, 0) if tme else 0, r.get(0x36 + ctx, 0) if tme else 0)

    def start_batch(self):
        key = self.state_key()
        if self.batch is not None and self.batch["key"] == key:
            return
        self.end_batch()
        ctx = self.context()
        self.batch = {"key": key, "path": self.path, "packet": self.packet_index, "kicks": 0, "skipped": 0, "verts": 0,
                      "x0": 1e9, "y0": 1e9, "x1": -1e9, "y1": -1e9, "z0": 1 << 40, "z1": -1, "a0": 256, "a1": -1,
                      "r0": 256, "r1": -1, "q0": 1e30, "q1": -1e30, "ctx": ctx,
                      "s0": 1e30, "s1": -1e30, "t0": 1e30, "t1": -1e30, "vertices": [],
                      "tex_upload": None}
        tme = bits(self.prim, 4, 1)
        if tme:
            tbp = bits(self.reg.get(0x06 + ctx, 0), 0, 14)
            for index in range(len(self.frame["uploads"]) - 1, -1, -1):
                if self.frame["uploads"][index]["dbp"] == tbp:
                    self.batch["tex_upload"] = index
                    break
            if self.texture_dir is not None and self.texture_frame in (None, len(self.frames) - 1):
                self.save_texture(len(self.frames) - 1, len(self.frame["batches"]), ctx)
        if self.target_dir is not None:
            frame = self.reg.get(0x4C + ctx, 0)
            scissor = self.reg.get(0x40 + ctx, 0)
            target = (bits(frame, 0, 9), bits(frame, 16, 6), bits(frame, 24, 6),
                      min(bits(scissor, 16, 11) + 1, 1024), min(bits(scissor, 48, 11) + 1, 1024))
            self.targets_drawn.setdefault(target, []).append((len(self.frames) - 1, len(self.frame["batches"])))

    def vertex(self, x, y, z, adc, kick):
        if self.frame is None:
            return
        self.start_batch()
        b = self.batch
        ctx = b["ctx"]
        ofs = self.reg.get(0x18 + ctx, 0)
        px = (x - bits(ofs, 0, 16)) / 16.0
        py = (y - bits(ofs, 32, 16)) / 16.0
        b["verts"] += 1
        b["x0"] = min(b["x0"], px)
        b["x1"] = max(b["x1"], px)
        b["y0"] = min(b["y0"], py)
        b["y1"] = max(b["y1"], py)
        b["z0"] = min(b["z0"], z)
        b["z1"] = max(b["z1"], z)
        b["a0"] = min(b["a0"], self.rgbaq[3])
        b["a1"] = max(b["a1"], self.rgbaq[3])
        b["r0"] = min(b["r0"], self.rgbaq[0], self.rgbaq[1], self.rgbaq[2])
        b["r1"] = max(b["r1"], self.rgbaq[0], self.rgbaq[1], self.rgbaq[2])
        b["q0"] = min(b["q0"], self.q)
        b["q1"] = max(b["q1"], self.q)
        if self.q != 0.0:
            b["s0"] = min(b["s0"], self.st[0] / self.q)
            b["s1"] = max(b["s1"], self.st[0] / self.q)
            b["t0"] = min(b["t0"], self.st[1] / self.q)
            b["t1"] = max(b["t1"], self.st[1] / self.q)
        if self.keep_vertices:
            b["vertices"].append((px, py, z, adc, kick, self.rgbaq[:4], self.st[0], self.st[1], self.q, self.uv, self.fog))
        prim_type = self.prim & 7
        self.vertex_queue += 1
        need = VERTS_PER_PRIM[prim_type]
        if self.vertex_queue >= need:
            if kick and not adc:
                b["kicks"] += 1
            else:
                b["skipped"] += 1
            if prim_type in (2, 4, 5):   # strips and fans keep their history
                self.vertex_queue = need - 1
            else:
                self.vertex_queue = 0

    # ---- registers ----------------------------------------------------------------------------------------
    def write(self, address, value, tick):
        if address == 0x00:
            self.prim = value & 0x7FF
            self.vertex_queue = 0
        elif address == 0x01:
            q = struct.unpack("<f", struct.pack("<I", bits(value, 32, 32)))[0]
            self.rgbaq = (bits(value, 0, 8), bits(value, 8, 8), bits(value, 16, 8), bits(value, 24, 8), q)
            self.q = q
        elif address == 0x02:
            self.st = struct.unpack("<ff", struct.pack("<Q", value))
        elif address == 0x03:
            self.uv = (bits(value, 0, 14), bits(value, 16, 14))
        elif address in (0x04, 0x0C):
            self.fog = bits(value, 56, 8)
            self.vertex(bits(value, 0, 16), bits(value, 16, 16), bits(value, 32, 24), False, address == 0x04)
        elif address in (0x05, 0x0D):
            self.vertex(bits(value, 0, 16), bits(value, 16, 16), bits(value, 32, 32), False, address == 0x05)
        elif address == 0x0A:
            self.fog = bits(value, 56, 8)
        elif address == 0x53:
            self.reg[address] = value
            self.begin_transfer()
        else:
            if address == 0x4C and self.path == 3:
                self.new_frame(tick)
            self.reg[address] = value

    def begin_transfer(self):
        if self.frame is None:
            return
        r = self.reg
        blt = r.get(0x50, 0)
        pos = r.get(0x51, 0)
        size = r.get(0x52, 0)
        direction = bits(r.get(0x53, 0), 0, 2)
        self.transfer = [0, b""]
        self.upload = {"dir": direction, "sbp": bits(blt, 0, 14), "dbp": bits(blt, 32, 14), "dbw": bits(blt, 48, 6),
                       "dpsm": bits(blt, 56, 6), "spsm": bits(blt, 24, 6), "w": bits(size, 0, 12), "h": bits(size, 32, 12),
                       "dx": bits(pos, 32, 11), "dy": bits(pos, 48, 11), "qwords": 0, "path": self.path,
                       "batches_before": len(self.frame["batches"]) + (1 if self.batch and self.batch["kicks"] else 0)}
        self.frame["uploads"].append(self.upload)

    def image(self, qwords, data=b""):
        if self.upload is not None:
            self.upload["qwords"] += qwords
            if self.vram is not None and self.upload["dir"] == 0 and self.transfer is not None:
                self.write_image(data)

    # ---- the memory records of MKSM_GS_DUMP_VRAM ------------------------------------------------------------
    def wants_memory_record(self):
        """Whether the next memory record has to be read: the first one, and those of the chosen frame."""
        self.vram_records += 1
        current = len(self.frames) - 1
        chosen = self.texture_frame is None or current in (self.texture_frame - 1, self.texture_frame)
        if self.vram_records == 1 and self.vram is not None:
            return True
        if not chosen or (self.vram is None and self.target_dir is None):
            self.targets_drawn = OrderedDict()
            return False
        return True

    def memory_record(self, data):
        if self.vram is not None:
            self.vram[:] = data[:len(self.vram)]
        if self.target_dir is None or (self.texture_frame is not None and len(self.frames) - 1 != self.texture_frame):
            self.targets_drawn = OrderedDict()
            return
        self.end_batch()
        for (fbp, fbw, psm, width, height), batches in self.targets_drawn.items():
            name = "p%05d_fb%03X_%s.png" % (self.packet_index, fbp, PSM_NAMES.get(psm, "%02X" % psm))
            print("   record %3d before packet %5d: buffer %03X/%d %s %dx%d after batches %s -> %s" % (
                self.vram_records, self.packet_index, fbp, fbw, PSM_NAMES.get(psm, "%02X" % psm), width, height,
                ", ".join("%d.%d" % b for b in (batches if len(batches) <= 6 else batches[:3] + batches[-3:])), name))
            rgba = bytearray(width * height * 4)
            out = 0
            sixteen = psm in (0x02, 0x0A, 0x32, 0x3A)
            for y in range(height):
                for x in range(width):
                    address = pixel_address(x, y, fbp * 32, fbw, psm)
                    if sixteen:
                        value = self.expand16(data[address] | (data[address + 1] << 8))
                        rgba[out] = value & 255
                        rgba[out + 1] = (value >> 8) & 255
                        rgba[out + 2] = (value >> 16) & 255
                    else:
                        rgba[out:out + 3] = data[address:address + 3]
                    rgba[out + 3] = 255
                    out += 4
            write_png(os.path.join(self.target_dir, name), width, height, rgba)
        self.targets_drawn = OrderedDict()

    # ---- the memory model (--textures) --------------------------------------------------------------------
    def write_image(self, data):
        u = self.upload
        psm, width, height = u["dpsm"], u["w"], u["h"]
        if width == 0 or height == 0:
            return
        vram = self.vram
        data = self.transfer[1] + bytes(data)
        index = self.transfer[0]
        total = width * height
        kind = PSM_KIND.get(psm, "32")
        pos = 0
        if kind == "4":
            while index < total and pos < len(data):
                byte = data[pos]
                pos += 1
                for nibble in (byte & 15, byte >> 4):
                    if index >= total:
                        break
                    address = pixel_address(u["dx"] + index % width, u["dy"] + index // width, u["dbp"], u["dbw"], psm)
                    shift = (address & 1) * 4
                    vram[address >> 1] = (vram[address >> 1] & ~(15 << shift) & 255) | (nibble << shift)
                    index += 1
            self.transfer = [index, b""]
            return
        size = {"32": 3 if psm == 0x01 else 4, "16": 2, "16S": 2, "8": 1}[kind]
        if psm in (0x1B, 0x24, 0x2C):
            size = 1          # T8H arrives as bytes (into bits 24-31); T4HL and T4HH are not modelled
        while index < total and pos + size <= len(data):
            address = pixel_address(u["dx"] + index % width, u["dy"] + index // width, u["dbp"], u["dbw"], psm)
            if psm == 0x1B:
                vram[address + 3] = data[pos]
            elif psm not in (0x24, 0x2C):
                vram[address:address + size] = data[pos:pos + size]
            pos += size
            index += 1
        self.transfer = [index, data[pos:] if index < total else b""]

    def read_texel(self, x, y, bp, bw, psm):
        vram = self.vram
        address = pixel_address(x, y, bp, bw, psm)
        if psm == 0x14:
            return (vram[address >> 1] >> ((address & 1) * 4)) & 15
        if psm == 0x13:
            return vram[address]
        if psm in (0x02, 0x0A):
            return vram[address] | (vram[address + 1] << 8)
        return vram[address] | (vram[address + 1] << 8) | (vram[address + 2] << 16) | (vram[address + 3] << 24)

    def save_texture(self, frame_number, batch_number, ctx):
        tex0 = self.reg.get(0x06 + ctx, 0)
        tex1 = self.reg.get(0x14 + ctx, 0)
        psm = bits(tex0, 20, 6)
        width, height = 1 << bits(tex0, 26, 4), 1 << bits(tex0, 30, 4)
        cbp, cpsm, csm, csa = bits(tex0, 37, 14), bits(tex0, 51, 4), bits(tex0, 55, 1), bits(tex0, 56, 5)
        levels = [(bits(tex0, 0, 14), bits(tex0, 14, 6))]
        if bits(tex1, 6, 3) >= 2:
            for index in range(min(bits(tex1, 2, 3), 6)):
                reg = self.reg.get((0x34 if index < 3 else 0x36) + ctx, 0)
                levels.append((bits(reg, 20 * (index % 3), 14), bits(reg, 20 * (index % 3) + 14, 6)))
        if width > 1024 or height > 1024:
            return
        palette = None
        if psm in (0x13, 0x14, 0x1B, 0x24, 0x2C):
            count = 256 if psm in (0x13, 0x1B) else 16
            clut_psm = cpsm if cpsm in (0x02, 0x0A) else 0
            palette = []
            for entry in range(count):
                if csm:
                    cx, cy = entry + csa * 16, 0
                elif count == 256:
                    swapped = (entry & 0xE7) | ((entry & 8) << 1) | ((entry & 0x10) >> 1)
                    cx, cy = swapped % 16, swapped // 16
                else:
                    cx, cy = entry % 8 + (csa & 1) * 8, entry // 8 + (csa >> 1) * 2
                value = self.read_texel(cx, cy, cbp, 1, clut_psm)
                palette.append(self.expand16(value) if clut_psm else value)
        for level, (bp, bw) in enumerate(levels):
            w, h = max(1, width >> level), max(1, height >> level)
            rgba = bytearray(w * h * 4)
            out = 0
            for y in range(h):
                for x in range(w):
                    if palette is not None:
                        if psm in (0x13, 0x14):
                            value = palette[self.read_texel(x, y, bp, bw, psm)]
                        else:
                            word = self.read_texel(x, y, bp, bw, 0)
                            value = palette[word >> 24 if psm == 0x1B else (word >> 24) & 15 if psm == 0x24 else word >> 28]
                    elif psm in (0x02, 0x0A):
                        value = self.expand16(self.read_texel(x, y, bp, bw, psm))
                    else:
                        value = self.read_texel(x, y, bp, bw, psm)
                    rgba[out] = value & 255
                    rgba[out + 1] = (value >> 8) & 255
                    rgba[out + 2] = (value >> 16) & 255
                    rgba[out + 3] = 255          # the picture shows the colours; alpha is in the listing's state
                    out += 4
            name = "f%d_b%03d_%04X_L%d.png" % (frame_number, batch_number, levels[0][0], level)
            write_png(os.path.join(self.texture_dir, name), w, h, rgba)

    @staticmethod
    def expand16(value):
        r, g, b = (value & 31) << 3, ((value >> 5) & 31) << 3, ((value >> 10) & 31) << 3
        return r | (g << 8) | (b << 16) | (0x80000000 if value & 0x8000 else 0)


class PathParser:
    """The GIF tag state of one path: a tag may continue in the next packet of the path."""

    def __init__(self, gs, path):
        self.gs = gs
        self.path = path
        self.nloop = 0
        self.flg = 0
        self.regs = []
        self.reg_index = 0
        self.pending_q = 1.0

    def feed(self, data, tick):
        gs = self.gs
        gs.path = self.path
        pos = 0
        size = len(data)
        while pos + 16 <= size or (self.nloop and pos < size):
            if self.nloop == 0:
                lo, hi = struct.unpack_from("<QQ", data, pos)
                pos += 16
                self.nloop = bits(lo, 0, 15)
                self.flg = bits(lo, 58, 2)
                nreg = bits(lo, 60, 4) or 16
                self.regs = [bits(hi, 4 * i, 4) for i in range(nreg)]
                self.reg_index = 0
                if bits(lo, 46, 1) and self.flg != 2 and self.flg != 3:
                    gs.write(0x00, bits(lo, 47, 11), tick)
                continue
            if self.flg == 0:      # PACKED
                if pos + 16 > size:
                    break
                lo, hi = struct.unpack_from("<QQ", data, pos)
                pos += 16
                reg = self.regs[self.reg_index]
                if reg == 0x0:
                    gs.write(0x00, bits(lo, 0, 11), tick)
                elif reg == 0x1:
                    r, g, b, a = bits(lo, 0, 8), bits(lo, 32, 8), bits(hi, 0, 8), bits(hi, 32, 8)
                    gs.rgbaq = (r, g, b, a, self.pending_q)
                    gs.q = self.pending_q
                elif reg == 0x2:
                    s, t = struct.unpack("<ff", struct.pack("<Q", lo))
                    gs.st = (s, t)
                    self.pending_q = struct.unpack("<f", struct.pack("<I", bits(hi, 0, 32)))[0]
                elif reg == 0x3:
                    gs.uv = (bits(lo, 0, 14), bits(lo, 32, 14))
                elif reg in (0x4, 0xC):
                    gs.fog = bits(hi, 36, 8)
                    gs.vertex(bits(lo, 0, 16), bits(lo, 32, 16), bits(hi, 4, 24), bits(hi, 47, 1) == 1, reg == 0x4)
                elif reg in (0x5, 0xD):
                    gs.vertex(bits(lo, 0, 16), bits(lo, 32, 16), bits(hi, 0, 32), bits(hi, 47, 1) == 1, reg == 0x5)
                elif reg == 0xA:
                    gs.fog = bits(hi, 36, 8)
                elif reg == 0xE:
                    gs.write(bits(hi, 0, 8), lo, tick)
                elif reg in (0x6, 0x7, 0x8, 0x9):
                    gs.write(reg, lo, tick)
                self.advance()
            elif self.flg == 1:    # REGLIST
                if pos + 8 > size:
                    break
                (value,) = struct.unpack_from("<Q", data, pos)
                pos += 8
                reg = self.regs[self.reg_index]
                if reg not in (0xE, 0xF):
                    gs.write(reg, value, tick)
                last = self.advance()
                if last and (pos & 15):
                    pos += 8
            else:                  # IMAGE
                take = min(self.nloop, (size - pos) // 16)
                if take == 0:
                    break
                gs.image(take, data[pos:pos + take * 16])
                pos += take * 16
                self.nloop -= take

    def advance(self):
        self.reg_index += 1
        if self.reg_index >= len(self.regs):
            self.reg_index = 0
            self.nloop -= 1
            return self.nloop == 0
        return False


def describe_key(key):
    ctx, prim, frame, zbuf, tex0, tex1, alpha, test, _ofs, _scissor, clamp, _texa, _miptbp1, _miptbp2 = key
    parts = []
    parts.append("%-8s" % PRIM_NAMES[prim & 7])
    flags = "".join(name for name, bit in (("G", 3), ("T", 4), ("F", 5), ("A", 6), ("a", 7), ("U", 8)) if bits(prim, bit, 1))
    parts.append("%-5s" % flags)
    parts.append("fb %03X/%s msk %08X" % (bits(frame, 0, 9), PSM_NAMES.get(bits(frame, 24, 6), "?"), bits(frame, 32, 32)))
    parts.append("z %03X%s" % (bits(zbuf, 0, 9), " nowr" if bits(zbuf, 32, 1) else ""))
    if bits(prim, 4, 1):
        parts.append("tex %04X %dx%d %s tcc%d tfx%d cbp %04X  mag%d min%d" % (
            bits(tex0, 0, 14), 1 << bits(tex0, 26, 4), 1 << bits(tex0, 30, 4), PSM_NAMES.get(bits(tex0, 20, 6), "?"),
            bits(tex0, 34, 1), bits(tex0, 35, 2), bits(tex0, 37, 14), bits(tex1, 5, 1), bits(tex1, 6, 3)))
    else:
        parts.append("untextured")
    if bits(prim, 6, 1):
        names = (("Cs", "Cd", "0", "?"), ("Cs", "Cd", "0", "?"), ("As", "Ad", "FIX", "?"), ("Cs", "Cd", "0", "?"))
        a, b, c, d = bits(alpha, 0, 2), bits(alpha, 2, 2), bits(alpha, 4, 2), bits(alpha, 6, 2)
        parts.append("blend (%s-%s)*%s+%s fix %02X" % (names[0][a], names[1][b], names[2][c], names[3][d], bits(alpha, 32, 8)))
    else:
        parts.append("opaque")
    ate = bits(test, 0, 1)
    ztst = ("never", "always", "gequal", "greater")[bits(test, 17, 2)] if bits(test, 16, 1) else "off"
    parts.append("ztest %s" % ztst)
    if ate:
        parts.append("atest %s ref %02X fail %s" % (("never", "always", "less", "lequal", "equal", "gequal", "greater", "notequal")[bits(test, 1, 3)],
                                                    bits(test, 4, 8), ("keep", "fb", "zb", "rgb")[bits(test, 12, 2)]))
    if bits(test, 14, 1):
        parts.append("date%d" % bits(test, 15, 1))
    return "  ".join(parts)


def describe_mip(batch):
    """The mipmap state of a textured batch: which level of detail the GS computes for its Q values."""
    import math
    key = batch["key"]
    prim, tex0, tex1, miptbp1, miptbp2 = key[1], key[4], key[5], key[12], key[13]
    if not bits(prim, 4, 1):
        return None
    lcm, mxl, mmin, mtba, l = bits(tex1, 0, 1), bits(tex1, 2, 3), bits(tex1, 6, 3), bits(tex1, 9, 1), bits(tex1, 19, 2)
    k = bits(tex1, 32, 12)
    if k & 0x800:
        k -= 0x1000
    k /= 16.0
    text = "lcm%d mxl%d mtba%d L%d K%+.2f" % (lcm, mxl, mtba, l, k)
    if lcm:
        text += "  lod %.2f (fixed)" % k
    else:
        lods = []
        for q in (batch["q1"], batch["q0"]):
            q = abs(q)
            lods.append((math.log2(1.0 / q) * (1 << l) + k) if q > 0.0 and q < 1e30 else float("nan"))
        text += "  lod %.2f..%.2f" % (lods[0], lods[1])
    if batch["s1"] >= batch["s0"]:
        text += "  s %.3f..%.3f t %.3f..%.3f" % (batch["s0"], batch["s1"], batch["t0"], batch["t1"])
    if mmin >= 2 and mxl:
        levels = ["%04X/%d" % (bits(tex0, 0, 14), bits(tex0, 14, 6))]
        for index in range(min(mxl, 6)):
            reg = miptbp1 if index < 3 else miptbp2
            shift = 20 * (index % 3)
            levels.append("%04X/%d" % (bits(reg, shift, 14), bits(reg, shift + 14, 6)))
        text += "  levels " + " ".join(levels)
    return text


def load(path, texture_dir=None, texture_frame=None, keep_vertices=False, target_dir=None):
    source = open(path, "rb")
    if source.read(8)[:4] != b"MKGD":
        raise SystemExit("not a GS dump of MKSM_GS_DUMP")
    gs = Gs()
    gs.keep_vertices = keep_vertices
    if texture_dir is not None:
        os.makedirs(texture_dir, exist_ok=True)
        gs.vram = bytearray(4 * 1024 * 1024)
        gs.texture_dir = texture_dir
    gs.texture_frame = texture_frame
    if target_dir is not None:
        os.makedirs(target_dir, exist_ok=True)
        gs.target_dir = target_dir
    parsers = {1: PathParser(gs, 1), 2: PathParser(gs, 2), 3: PathParser(gs, 3)}
    index = 0
    while True:
        header = source.read(12)
        if len(header) < 12:
            break
        tick, gif_path, size = struct.unpack("<III", header)
        gs.packet_index = index
        if gif_path == 0:
            if gs.wants_memory_record():
                gs.memory_record(source.read(size))
            else:
                source.seek(size, 1)
            continue
        packet = source.read(size)
        index += 1
        gs.ensure_frame(tick)
        gs.path = gif_path
        parsers[gif_path].feed(packet, tick)
        if gs.frame is not None:
            gs.frame["packets"] += 1
            gs.frame["bytes"][gif_path] += size
    source.close()
    gs.end_batch()
    return gs


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    want_frame = None
    show_batches = "--batches" in args
    show_uploads = "--uploads" in args
    show_mip = "--mip" in args
    where = None
    if "--frame" in args:
        want_frame = int(args[args.index("--frame") + 1])
    if "--where" in args:
        where = tuple(float(v) for v in args[args.index("--where") + 1].split(","))
    texture_dir = args[args.index("--textures") + 1] if "--textures" in args else None
    verts_of = int(args[args.index("--verts") + 1]) if "--verts" in args else None
    target_dir = args[args.index("--targets") + 1] if "--targets" in args else None
    gs = load(path, texture_dir, want_frame, verts_of is not None, target_dir)
    for number, frame in enumerate(gs.frames):
        if want_frame is not None and number != want_frame:
            continue
        batches = frame["batches"]
        kicks = sum(b["kicks"] for b in batches)
        print("frame %d (tick %d): %d packets (PATH1 %d KB, PATH2 %d KB, PATH3 %d KB), %d uploads (%d KB), %d batches, %d primitives drawn" % (
            number, frame["tick"], frame["packets"], frame["bytes"][1] // 1024, frame["bytes"][2] // 1024, frame["bytes"][3] // 1024,
            len(frame["uploads"]), sum(u["qwords"] for u in frame["uploads"]) * 16 // 1024, len(batches), kicks))
        if show_uploads:
            for index, u in enumerate(frame["uploads"]):
                print("   upload %3d  path %d  dir %d  dbp %04X dbw %2d %-5s  %4dx%-4d at (%d,%d)  %6d qwords  before batch %d" % (
                    index, u["path"], u["dir"], u["dbp"], u["dbw"], PSM_NAMES.get(u["dpsm"], "%02X" % u["dpsm"]), u["w"], u["h"],
                    u["dx"], u["dy"], u["qwords"], u["batches_before"]))
        if verts_of is not None and verts_of < len(batches):
            b = batches[verts_of]
            print("   batch %d: %s" % (verts_of, describe_key(b["key"])))
            mip = describe_mip(b)
            if mip is not None:
                print("      mip: " + mip)
            tex0 = b["key"][4]
            tw, th = 1 << bits(tex0, 26, 4), 1 << bits(tex0, 30, 4)
            for n, (px, py, z, adc, kick, rgba, vs, vt, vq, uv, fog) in enumerate(b["vertices"]):
                texel = "u %9.2f v %9.2f" % (vs / vq * tw, vt / vq * th) if vq != 0.0 else "q = 0"
                print("      %4d  x %8.2f y %8.2f z %08X %s  rgba %3d %3d %3d %3d  fog %3d  S %-13.6g T %-13.6g Q %-13.6g  %s" % (
                    n, px, py, z, "skip" if (adc or not kick) else "draw", rgba[0], rgba[1], rgba[2], rgba[3], fog, vs, vt, vq, texel))
            continue
        selected = batches
        if where is not None:
            selected = [b for b in batches if b["x0"] <= where[0] <= b["x1"] and b["y0"] <= where[1] <= b["y1"]]
        if show_batches or where is not None:
            for b in selected:
                index = batches.index(b)
                print("   batch %4d  path %d  %5d prims (%4d skipped)  x %7.1f..%-7.1f y %7.1f..%-7.1f  z %08X..%08X  a %3d..%-3d rgb %3d..%-3d  q %.3g..%.3g  upl %s | %s" % (
                    index, b["path"], b["kicks"], b["skipped"], b["x0"], b["x1"], b["y0"], b["y1"], b["z0"], b["z1"], b["a0"], b["a1"],
                    b["r0"], b["r1"], b["q0"], b["q1"], b["tex_upload"] if b["tex_upload"] is not None else "-", describe_key(b["key"])))
                if show_mip:
                    mip = describe_mip(b)
                    if mip is not None:
                        print("               mip: " + mip)
        else:
            groups = OrderedDict()
            for b in batches:
                g = groups.setdefault(b["key"], {"batches": 0, "kicks": 0, "x0": 1e9, "y0": 1e9, "x1": -1e9, "y1": -1e9, "a0": 256, "a1": -1,
                                                 "first": batches.index(b)})
                g["batches"] += 1
                g["kicks"] += b["kicks"]
                for lo in ("x0", "y0", "a0"):
                    g[lo] = min(g[lo], b[lo])
                for hi in ("x1", "y1", "a1"):
                    g[hi] = max(g[hi], b[hi])
            for key, g in groups.items():
                print("   %4d batches from %4d  %6d prims  x %7.1f..%-7.1f y %7.1f..%-7.1f  a %3d..%-3d | %s" % (
                    g["batches"], g["first"], g["kicks"], g["x0"], g["x1"], g["y0"], g["y1"], g["a0"], g["a1"], describe_key(key)))


if __name__ == "__main__":
    main()
