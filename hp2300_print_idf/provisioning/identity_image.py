"""Create an ESP32-S3 BLOCK3 identity image; never burns eFuse.

Manufacturing must independently reserve the UUID on the server, verify BLOCK3
is empty and not used for Custom MAC, burn and read back the complete image,
then permanently write-protect BLOCK3. This tool deliberately performs none
of those irreversible device operations.
"""

from __future__ import annotations

import argparse
import pathlib
import uuid
import zlib


def identity_image(device_id: str) -> bytes:
    parsed = uuid.UUID(device_id)
    if str(parsed) != device_id:
        raise ValueError("device_id must be a canonical lowercase UUID")
    prefix = b"BIPS" + bytes((1, 0, 0, 0)) + parsed.bytes
    crc = zlib.crc32(prefix).to_bytes(4, "little")
    image = prefix + crc + bytes(4)
    assert len(image) == 32
    return image


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-id", required=True, help="server-reserved UUID")
    parser.add_argument("--output", type=pathlib.Path, required=True,
                        help="new 32-byte BLOCK3 image file; refuses overwrite")
    args = parser.parse_args()
    image = identity_image(args.device_id)
    with args.output.open("xb") as output:
        output.write(image)
    print(f"Generated {args.output} for {args.device_id} ({len(image)} bytes)")
    print("NOT BURNED: server reservation, eFuse allocation and read-back remain mandatory")


if __name__ == "__main__":
    main()
