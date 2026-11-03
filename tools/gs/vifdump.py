"""Summarizes a MKSM_DMA_CHAINDUMP capture: every DMA transfer as the VIF or GIF received it.

    python tools/gs/vifdump.py logs/chaindump.bin [--records N] [--regs] [--path P]

The capture is a sequence of [u32 path][u32 source address][u32 bytes][data] records (ps2_memory.cpp, scene
diagnostics patch): path 0 = VIF0, 1 = VIF1, 2 = GIF. A VIF record is decoded as a VIFcode stream (the DIRECT
payloads as GIF packets); a GIF record as GIF packets. The GIF primitive types are: 0 point, 1 line, 2 line strip,
3 triangle, 4 triangle strip, 5 triangle fan, 6 sprite."""
import struct
import sys
from collections import Counter

VIF_NAMES = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD', 0x06: 'MSKPATH3',
             0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT',
             0x20: 'STMASK', 0x30: 'STROW', 0x31: 'STCOL', 0x4A: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}
GS_REG_NAMES = {0x00: 'PRIM', 0x01: 'RGBAQ', 0x02: 'ST', 0x03: 'UV', 0x04: 'XYZF2', 0x05: 'XYZ2', 0x06: 'TEX0_1',
                0x07: 'TEX0_2', 0x08: 'CLAMP_1', 0x09: 'CLAMP_2', 0x0A: 'FOG', 0x0C: 'XYZF3', 0x0D: 'XYZ3',
                0x14: 'TEX1_1', 0x15: 'TEX1_2', 0x16: 'TEX2_1', 0x17: 'TEX2_2', 0x18: 'XYOFFSET_1', 0x19: 'XYOFFSET_2',
                0x1A: 'PRMODECONT', 0x1B: 'PRMODE', 0x1C: 'TEXCLUT', 0x22: 'SCANMSK', 0x34: 'MIPTBP1_1',
                0x35: 'MIPTBP1_2', 0x36: 'MIPTBP2_1', 0x37: 'MIPTBP2_2', 0x3B: 'TEXA', 0x3D: 'FOGCOL', 0x3F: 'TEXFLUSH',
                0x40: 'SCISSOR_1', 0x41: 'SCISSOR_2', 0x42: 'ALPHA_1', 0x43: 'ALPHA_2', 0x44: 'DIMX', 0x45: 'DTHE',
                0x46: 'COLCLAMP', 0x47: 'TEST_1', 0x48: 'TEST_2', 0x49: 'PABE', 0x4A: 'FBA_1', 0x4B: 'FBA_2',
                0x4C: 'FRAME_1', 0x4D: 'FRAME_2', 0x4E: 'ZBUF_1', 0x4F: 'ZBUF_2', 0x50: 'BITBLTBUF', 0x51: 'TRXPOS',
                0x52: 'TRXREG', 0x53: 'TRXDIR', 0x54: 'HWREG', 0x60: 'SIGNAL', 0x61: 'FINISH', 0x62: 'LABEL'}
PRIM_NAMES = ['point', 'line', 'linestrip', 'tri', 'tristrip', 'trifan', 'sprite', 'prim7']
VERTEX_REGS = {0x04, 0x05, 0x0C, 0x0D}
UNPACK_BITS = {0: 32, 1: 16, 2: 8, 3: 5}
PATH_NAMES = {0: 'VIF0', 1: 'VIF1', 2: 'GIF'}
STATE_REGS = (0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x52, 0x53, 0x40, 0x18, 0x47)


def read_records(path):
    data = open(path, 'rb').read()
    pos = 0
    while pos + 12 <= len(data):
        dma_path, source, size = struct.unpack_from('<III', data, pos)
        pos += 12
        yield dma_path, source, data[pos:pos + size]
        pos += size


def parse_gif(payload, stats, regs):
    """Walks GIF packets; counts primitives and vertices, keeps the A+D register writes."""
    pos = 0
    end = len(payload) - len(payload) % 16
    while pos + 16 <= end:
        low, high = struct.unpack_from('<QQ', payload, pos)
        pos += 16
        nloop = low & 0x7FFF
        eop = (low >> 15) & 1
        pre = (low >> 46) & 1
        prim = (low >> 47) & 0x7FF
        flg = (low >> 58) & 3
        nreg = (low >> 60) & 0xF or 16
        stats['giftags'] += 1
        if nloop == 0:
            if eop:
                stats['eop'] += 1
            continue
        if flg == 2 or flg == 3:  # IMAGE
            stats['image_qw'] += nloop
            pos += nloop * 16
            continue
        regs_list = [(high >> (4 * i)) & 0xF for i in range(nreg)]
        if pre:
            stats['prim:' + PRIM_NAMES[prim & 7]] += 1
            stats['prims'] += 1
        if flg == 0:  # PACKED
            for _ in range(nloop):
                for reg in regs_list:
                    if pos + 16 > end:
                        stats['truncated'] += 1
                        return
                    if reg == 0xE:  # A+D
                        value, addr = struct.unpack_from('<QQ', payload, pos)
                        addr &= 0xFF
                        name = GS_REG_NAMES.get(addr, '0x%02X' % addr)
                        regs['A+D ' + name] += 1
                        if addr == 0x00:
                            stats['prim:' + PRIM_NAMES[value & 7] + '(A+D)'] += 1
                            stats['prims'] += 1
                        elif addr in VERTEX_REGS:
                            stats['vertices'] += 1
                        elif addr in STATE_REGS:
                            regs['%s=%016X' % (name, value)] += 1
                    elif reg in VERTEX_REGS:
                        stats['vertices'] += 1
                    pos += 16
        else:  # REGLIST
            count = nloop * nreg
            for i in range(count):
                if regs_list[i % nreg] in VERTEX_REGS:
                    stats['vertices'] += 1
            pos += ((count + 1) // 2) * 16
        if eop:
            stats['eop'] += 1
    if pos < end:
        stats['trailing_qw'] += (end - pos) // 16


class VifStream:
    """Decodes VIFcodes; STCYCL and a DIRECT in progress persist from one record to the next, as in the VIF."""

    def __init__(self):
        self.cl = self.wl = 1
        self.direct_remaining = 0
        self.direct_payload = bytearray()

    def decode(self, block, counts, launches, stats, regs, totals):
        words = struct.unpack('<%dI' % (len(block) // 4), block)
        i = 0
        n = len(words)
        if self.direct_remaining:
            take = min(self.direct_remaining, n * 4)
            self.direct_payload += block[:take]
            self.direct_remaining -= take
            i = (take + 3) // 4
            if self.direct_remaining == 0:
                parse_gif(bytes(self.direct_payload), stats, regs)
                self.direct_payload = bytearray()
        while i < n:
            code = words[i]
            cmd = (code >> 24) & 0x7F
            imm = code & 0xFFFF
            num = (code >> 16) & 0xFF
            i += 1
            if cmd >= 0x60:
                vn = ((cmd >> 2) & 3) + 1
                vl = cmd & 3
                if vl == 3 and vn == 4:
                    bits = 16  # V4-5: one 16-bit word per vector
                else:
                    bits = {0: 32, 1: 16, 2: 8, 3: 4}[vl] * vn
                count = num or 256
                if self.wl > self.cl:
                    count = self.cl * (count // self.wl) + min(count % self.wl, self.cl)
                size_bytes = (count * bits + 7) // 8
                name = 'UNPACK V%d-%d' % (vn, UNPACK_BITS[vl]) + ('m' if cmd & 0x10 else '')
                counts[name] += 1
                totals['unpack_bytes'] += size_bytes
                i += (size_bytes + 3) // 4
                continue
            name = VIF_NAMES.get(cmd)
            if name is None:
                counts['UNKNOWN 0x%02X' % cmd] += 1
                totals['unknown'] += 1
                continue
            counts[name] += 1
            if cmd == 0x01:
                self.cl = imm & 0xFF or 1
                self.wl = (imm >> 8) & 0xFF or 1
            elif cmd in (0x14, 0x15):
                launches.append('%s 0x%X' % (name, imm * 8))
            elif cmd == 0x17:
                launches.append('MSCNT')
            elif cmd == 0x20:
                i += 1
            elif cmd in (0x30, 0x31):
                i += 4
            elif cmd == 0x4A:
                i += (num or 256) * 2
            elif cmd in (0x50, 0x51):
                size = (imm or 65536) * 16
                totals['direct_bytes'] += size
                available = min(size, (n - i) * 4)
                payload = block[i * 4:i * 4 + available]
                i += (available + 3) // 4
                if available < size:
                    self.direct_payload = bytearray(payload)
                    self.direct_remaining = size - available
                else:
                    parse_gif(payload, stats, regs)


def main(argv):
    path = argv[1]
    limit = int(argv[argv.index('--records') + 1]) if '--records' in argv else 10 ** 9
    only = int(argv[argv.index('--path') + 1]) if '--path' in argv else None
    show_regs = '--regs' in argv
    streams = {0: VifStream(), 1: VifStream()}
    grand = Counter()
    for index, (dma_path, source, block) in enumerate(read_records(path)):
        grand['records'] += 1
        grand['bytes'] += len(block)
        grand['bytes path %d' % dma_path] += len(block)
        if only is not None and dma_path != only:
            continue
        if index >= limit:
            continue
        counts, launches, stats, regs, totals = Counter(), [], Counter(), Counter(), Counter()
        if dma_path in streams:
            streams[dma_path].decode(block, counts, launches, stats, regs, totals)
        else:
            parse_gif(block, stats, regs)
        print('#%d %s source=0x%08X bytes=%d' % (index, PATH_NAMES.get(dma_path, dma_path), source, len(block)))
        if counts:
            print('  vifcodes: ' + ', '.join('%s x%d' % (k, v) for k, v in sorted(counts.items(), key=lambda kv: -kv[1])))
        if launches:
            print('  launches: ' + ' '.join(launches))
        if totals:
            print('  ' + ', '.join('%s=%d' % (k, v) for k, v in sorted(totals.items())))
        if stats:
            print('  gif: ' + ', '.join('%s=%d' % (k, v) for k, v in sorted(stats.items())))
        if show_regs and regs:
            for k, v in sorted(regs.items()):
                print('    %s x%d' % (k, v))
    print('total: ' + ', '.join('%s=%d' % (k, v) for k, v in sorted(grand.items())))


if __name__ == '__main__':
    main(sys.argv)
