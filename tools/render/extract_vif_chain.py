#!/usr/bin/env python3
"""Prepara la entrada de tools/render/replay_vif_chain.cpp.

Extracts the VIF1 DMA chain of a frame (tags, TTE VIFcodes and data) together with the
VU1 memory and the GS VRAM. It works with two sources:

  --pcsx2 <savestate.p2s>   savestate de PCSX2 (zip con eeMemory.bin, GS.bin, vu1*.bin...)
  --volcado <carpeta>       volcado del port con GOW_RENDER_DIAG (gow_render_ram.bin, ...)

The output contains game data: it is saved in logs/ and never published.
"""
import argparse
import os
import struct
import sys
import zipfile

# God of War frame headers (DIRECT with the GS configuration and NEXT to the list):
# the game alternates between two buffers.
INICIOS_CUADRO = (0x450C00, 0x450A00)
RAM = 0x2000000


def recorrer(ee, inicio, fin=None, limite=500000):
    """Walks a DMA chain from `inicio`. Returns (address of END, number of tags)."""
    a, pila, n = inicio, [], 0
    while n < limite:
        if a < 0 or a + 16 > min(RAM, len(ee)) or a & 15:
            return None, n
        t = struct.unpack_from('<Q', ee, a)[0]
        qwc, tid, addr = t & 0xFFFF, (t >> 28) & 7, (t >> 32) & 0x7FFFFFFF
        n += 1
        if tid not in (1, 2, 3, 4, 5, 6, 7):
            return None, n
        datos = a + 16 if tid in (1, 2, 5, 6, 7) else addr & (RAM - 1)
        if datos + qwc * 16 > min(RAM, len(ee)):
            return None, n
        if tid == 1:
            a = a + 16 + qwc * 16
        elif tid == 2:
            a = addr
        elif tid in (3, 4):
            a += 16
        elif tid == 5:
            if len(pila) >= 2:
                return None, n
            pila.append(a + 16 + qwc * 16)
            a = addr
        elif tid == 6:
            if not pila:
                return None, n
            a = pila.pop()
        elif tid == 7:
            return (a, n) if fin is None or a == fin else (None, n)
        else:
            return None, n
    return None, n


def seleccionar_cadena(ee, inicio=None, fin=None):
    """Requires a single valid header; an explicit choice does not identify the current frame."""
    # GOW-Port: both buffers can keep valid chains; their order does not identify the frame.
    candidatos = [inicio] if inicio is not None else dict.fromkeys(INICIOS_CUADRO)
    validos = []
    for candidato in candidatos:
        final, etiquetas = recorrer(ee, candidato, fin)
        if final is not None:
            validos.append((candidato, final, etiquetas))
    destino = '' if fin is None else ' 0x%X' % fin
    if not validos:
        if inicio is not None:
            raise ValueError('The chain chosen with --inicio 0x%X does not end in END%s' % (inicio, destino))
        raise ValueError('No frame header ends in END%s' % destino)
    if len(validos) > 1:
        detalle = ', '.join('0x%X -> END 0x%X' % (candidato, final)
                            for candidato, final, _ in validos)
        raise ValueError('Ambiguous selection: %s. Give --inicio; choosing a header does not prove '
                         'that it is the current frame.' % detalle)
    return validos[0]


def aplanar(ee, inicio):
    """Stream that VIF1 receives with CHCR.TTE: the high 64 bits of each tag and its payload."""
    final, etiquetas = recorrer(ee, inicio)
    if final is None:
        raise ValueError('Invalid DMA chain or without END')
    out, a, pila = bytearray(), inicio, []
    for _ in range(etiquetas):
        t = struct.unpack_from('<Q', ee, a)[0]
        qwc, tid, addr = t & 0xFFFF, (t >> 28) & 7, (t >> 32) & 0x7FFFFFFF
        out += ee[a + 8:a + 16]
        if tid in (1, 2, 5, 6, 7):
            datos = a + 16
        else:
            datos = addr & (RAM - 1)
        if tid == 1:
            a = a + 16 + qwc * 16
        elif tid == 2:
            a = addr
        elif tid in (3, 4):
            a += 16
        elif tid == 5:
            pila.append(a + 16 + qwc * 16)
            a = addr
        elif tid == 6:
            a = pila.pop() if pila else None
        else:
            a = None
        out += ee[datos:datos + qwc * 16]
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument('--pcsx2', help='savestate .p2s de PCSX2')
    g.add_argument('--volcado', help='carpeta con gow_render_ram.bin, gow_render_vram.bin, gow_vu1_*.bin')
    p.add_argument('--inicio', type=lambda v: int(v, 0), default=None,
                   help='explicitly chosen header; by default it requires a single valid chain '
                        'entre 0x450C00 y 0x450A00')
    p.add_argument('salida', help='carpeta de salida (en logs/)')
    a = p.parse_args()

    os.makedirs(a.salida, exist_ok=True)
    if a.pcsx2:
        z = zipfile.ZipFile(a.pcsx2)
        ee = z.read('eeMemory.bin')
        gs = z.read('GS.bin')
        # GSState::Freeze: registers, 4 MB of VRAM and, at the end, 4 GIFPaths (16 B tag + 4 B reg) and Q.
        vram = gs[len(gs) - 0x54 - 0x400000:len(gs) - 0x54]
        archivos = {'vu1MicroMem.bin': z.read('vu1MicroMem.bin'), 'vu1Memory.bin': z.read('vu1Memory.bin'),
                    'eeHwRegs.bin': z.read('eeHwRegs.bin'), 'Screenshot.png': z.read('Screenshot.png')}
        hw = archivos['eeHwRegs.bin']
        fin = struct.unpack_from('<I', hw, 0x9030)[0]  # D1_TADR: END of the last chain
    else:
        leer = lambda n: open(os.path.join(a.volcado, n), 'rb').read()
        ee, vram = leer('gow_render_ram.bin'), leer('gow_render_vram.bin')
        archivos = {'vu1MicroMem.bin': leer('gow_vu1_code.bin'), 'vu1Memory.bin': leer('gow_vu1_data.bin')}
        fin = None
    if len(vram) != 0x400000 or len(ee) != RAM:
        sys.exit('Unexpected RAM or VRAM sizes')

    try:
        a.inicio, final, etiquetas = seleccionar_cadena(ee, a.inicio, fin)
    except ValueError as error:
        sys.exit(str(error))
    flujo = aplanar(ee, a.inicio)
    for nombre, datos in archivos.items():
        open(os.path.join(a.salida, nombre), 'wb').write(datos)
    open(os.path.join(a.salida, 'vram.bin'), 'wb').write(vram)
    open(os.path.join(a.salida, 'eeMemory.bin'), 'wb').write(ee)
    open(os.path.join(a.salida, 'vif1_stream.bin'), 'wb').write(flujo)
    print('cadena 0x%X -> END 0x%X: %d etiquetas, %d bytes para VIF1' % (a.inicio, final, etiquetas, len(flujo)))


if __name__ == '__main__':
    main()
