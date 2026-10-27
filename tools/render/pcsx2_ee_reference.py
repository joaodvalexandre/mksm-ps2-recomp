"""Local EE reference through PINE; read-only, PCSX2 paused, SCUS-97399."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct

RAM_SIZE = 0x2000000
MAX_REPLY = 450000
EXPECTED_ID = "SCUS-97399"
EXPECTED_CRC = "d6385328"


class Pine:
    # Formato de pcsx2/PINE.cpp (PCSX2, GPL-3.0+): longitud LE incluye cabecera.
    # No write commands nor state load/save are implemented.
    def __init__(self, connection):
        self.connection = connection

    def exact(self, size):
        result = bytearray()
        while len(result) < size:
            chunk = self.connection.recv(size - len(result))
            if not chunk:
                raise EOFError("PINE closed the connection")
            result.extend(chunk)
        return bytes(result)

    def query(self, request):
        self.connection.sendall(struct.pack("<I", len(request) + 4) + request)
        size = struct.unpack("<I", self.exact(4))[0]
        if not 5 <= size < MAX_REPLY:
            raise ValueError("Invalid PINE response length")
        response = self.exact(size - 4)
        if response[0] != 0:
            raise ValueError("PCSX2 rejected the PINE read")
        return response[1:]

    def status(self):
        response = self.query(b"\x0f")
        if len(response) != 4:
            raise ValueError("Estado PINE truncado")
        return struct.unpack("<I", response)[0]

    def string(self, command):
        if command not in (8, 12, 13):
            raise ValueError("Invalid identity command")
        response = self.query(bytes([command]))
        if len(response) < 5:
            raise ValueError("Identidad PINE truncada")
        size = struct.unpack_from("<I", response)[0]
        value = response[4:]
        if size != len(value) or not value.endswith(b"\0"):
            raise ValueError("Invalid PINE identity length")
        return value[:-1].decode("utf-8")

    def read(self, address, size):
        if (address < 0 or size <= 0 or address % 4 or size % 4
                or address + size > RAM_SIZE or size > 65536):
            raise ValueError("Read outside the EE range/alignment")
        request = b"".join(b"\x02" + struct.pack("<I", at)
                           for at in range(address, address + size, 4))
        response = self.query(request)
        if len(response) != size:
            raise ValueError("Lectura de RAM PINE truncada")
        return response

    def identity(self):
        if self.status() != 1:
            raise ValueError("Pause PCSX2 before obtaining the reference")
        serial, crc = self.string(12), self.string(13).lower()
        if serial != EXPECTED_ID or crc != EXPECTED_CRC:
            raise ValueError(f"Wrong edition: {serial}, {crc}")
        return {"serial": serial, "crc": crc, "version": self.string(8)}


def capture(pine):
    identity = pine.identity()
    before = pine.read(0x29BDF8, 8)
    ram = b"".join(pine.read(at, 65536) for at in range(0, RAM_SIZE, 65536))
    after = pine.read(0x29BDF8, 8)
    if pine.identity() != identity or before != after or ram[0x29BDF8:0x29BE00] != before:
        raise ValueError("The identity or the hierarchy changed during the read")
    metadata = dict(identity, ram_bytes=len(ram), sha256=hashlib.sha256(ram).hexdigest(),
                    state=struct.unpack_from("<I", ram, 0x29E560)[0],
                    hierarchy_stamp=struct.unpack("<Q", before)[0],
                    active_view=hex(struct.unpack_from("<I", ram, 0x33104C)[0]),
                    limitation="Pause and stable mark; it does not guarantee the same frame as the port.")
    return ram, metadata


def private_output(directory):
    directory = directory.resolve()
    repo = Path(__file__).resolve().parents[2]
    if directory.is_relative_to(repo) and not directory.is_relative_to(repo / "logs"):
        raise ValueError("Inside the repository, use logs/ to keep the RAM out of Git")
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=28012)
    parser.add_argument("--output", type=Path, required=True,
                        help="New directory outside the repository or inside logs/")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("Invalid port")
    directory = private_output(args.output)
    if directory.exists():
        parser.error("The output directory already exists; use another name")
    with socket.create_connection(("127.0.0.1", args.port), timeout=5) as connection:
        ram, metadata = capture(Pine(connection))
    # Create only after validating the read and do not overwrite earlier references.
    directory.mkdir(parents=True, exist_ok=False)
    with (directory / "eeMemory.bin").open("xb") as output:
        output.write(ram)
    with (directory / "reference.json").open("x", encoding="utf-8") as output:
        json.dump(metadata, output, indent=2, ensure_ascii=False)
        output.write("\n")
    print(json.dumps(metadata, ensure_ascii=False))


if __name__ == "__main__":
    main()
