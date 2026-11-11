"""Lists what a GS dump of the game draws (MKSM_GS_DUMP, src/trace_gs_dump.cpp).

    python tools/gs_dump.py <dump> [--frame N] [--batches] [--uploads] [--where x,y]

The dump is the GIF packets the GS received during a few vertical blanks, in order. This tool decodes the GIF tags
(PACKED, REGLIST, IMAGE), follows the GS registers, and prints, per frame (a frame starts at each PATH3 packet that
sets FRAME_1, which is libgraph's drawing environment):

  default      a summary: the draw batches grouped by target buffer, texture, blending and test settings
  --batches    every draw batch in order (a batch is a run of primitives between two state changes)
  --uploads    the texture uploads (BITBLTBUF / TRXPOS / TRXREG / TRXDIR and their image data)
  --where x,y  only the batches whose bounding box covers that pixel of the target

It needs nothing but Python. The dump itself holds game data: keep it outside the repository.
"""
import struct
import sys
from collections import OrderedDict

PRIM_NAMES = ["point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "?"]
PSM_NAMES = {0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S", 0x13: "T8", 0x14: "T4", 0x1B: "T8H", 0x24: "T4HL",
             0x2C: "T4HH", 0x30: "Z32", 0x31: "Z24", 0x32: "Z16", 0x3A: "Z16S"}
VERTS_PER_PRIM = [1, 2, 2, 3, 3, 3, 2, 1]


def bits(value, low, count):
    return (value >> low) & ((1 << count) - 1)


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
                r.get(0x40 + ctx, 0), r.get(0x08 + ctx, 0) if tme else 0, r.get(0x3B, 0) if tme else 0)

    def start_batch(self):
        key = self.state_key()
        if self.batch is not None and self.batch["key"] == key:
            return
        self.end_batch()
        ctx = self.context()
        self.batch = {"key": key, "path": self.path, "packet": self.packet_index, "kicks": 0, "skipped": 0, "verts": 0,
                      "x0": 1e9, "y0": 1e9, "x1": -1e9, "y1": -1e9, "z0": 1 << 40, "z1": -1, "a0": 256, "a1": -1,
                      "r0": 256, "r1": -1, "q0": 1e30, "q1": -1e30, "ctx": ctx,
                      "tex_upload": None}
        tme = bits(self.prim, 4, 1)
        if tme:
            tbp = bits(self.reg.get(0x06 + ctx, 0), 0, 14)
            for index in range(len(self.frame["uploads"]) - 1, -1, -1):
                if self.frame["uploads"][index]["dbp"] == tbp:
                    self.batch["tex_upload"] = index
                    break

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
        self.upload = {"dir": direction, "sbp": bits(blt, 0, 14), "dbp": bits(blt, 32, 14), "dbw": bits(blt, 48, 6),
                       "dpsm": bits(blt, 56, 6), "spsm": bits(blt, 24, 6), "w": bits(size, 0, 12), "h": bits(size, 32, 12),
                       "dx": bits(pos, 32, 11), "dy": bits(pos, 48, 11), "qwords": 0, "path": self.path,
                       "batches_before": len(self.frame["batches"]) + (1 if self.batch and self.batch["kicks"] else 0)}
        self.frame["uploads"].append(self.upload)

    def image(self, qwords):
        if self.upload is not None:
            self.upload["qwords"] += qwords


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
                gs.image(take)
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
    ctx, prim, frame, zbuf, tex0, tex1, alpha, test, _ofs, _scissor, clamp, _texa = key
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


def load(path):
    data = open(path, "rb").read()
    if data[:4] != b"MKGD":
        raise SystemExit("not a GS dump of MKSM_GS_DUMP")
    pos = 8
    gs = Gs()
    parsers = {1: PathParser(gs, 1), 2: PathParser(gs, 2), 3: PathParser(gs, 3)}
    index = 0
    while pos + 12 <= len(data):
        tick, gif_path, size = struct.unpack_from("<III", data, pos)
        pos += 12
        packet = data[pos:pos + size]
        pos += size
        gs.packet_index = index
        index += 1
        gs.ensure_frame(tick)
        gs.path = gif_path
        parsers[gif_path].feed(packet, tick)
        if gs.frame is not None:
            gs.frame["packets"] += 1
            gs.frame["bytes"][gif_path] += size
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
    where = None
    if "--frame" in args:
        want_frame = int(args[args.index("--frame") + 1])
    if "--where" in args:
        where = tuple(float(v) for v in args[args.index("--where") + 1].split(","))
    gs = load(path)
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
        selected = batches
        if where is not None:
            selected = [b for b in batches if b["x0"] <= where[0] <= b["x1"] and b["y0"] <= where[1] <= b["y1"]]
        if show_batches or where is not None:
            for b in selected:
                index = batches.index(b)
                print("   batch %4d  path %d  %5d prims (%4d skipped)  x %7.1f..%-7.1f y %7.1f..%-7.1f  z %08X..%08X  a %3d..%-3d rgb %3d..%-3d  q %.3g..%.3g  upl %s | %s" % (
                    index, b["path"], b["kicks"], b["skipped"], b["x0"], b["x1"], b["y0"], b["y1"], b["z0"], b["z1"], b["a0"], b["a1"],
                    b["r0"], b["r1"], b["q0"], b["q1"], b["tex_upload"] if b["tex_upload"] is not None else "-", describe_key(b["key"])))
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
