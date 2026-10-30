"""Generate disposable codec/tag fixtures; never writes to the music library."""
import argparse
import math
import shutil
from pathlib import Path
import struct
import subprocess
import wave

parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--ffmpeg", required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
tone = args.output / "tone.wav"
with wave.open(str(tone), "wb") as out:
    out.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
    out.writeframes(b"".join(struct.pack("<hh", *([int(3276 * math.sin(i * 2 * math.pi * 1000 / 48000))] * 2))
                             for i in range(48000 * 3)))


def encode(name, codec, tags=(), extra=()):
    command = [args.ffmpeg, "-v", "error", "-y", "-i", str(tone), "-c:a", codec]
    for key, value in tags:
        command += ["-metadata", f"{key}={value}"]
    subprocess.run(command + list(extra) + [str(args.output / name)], check=True)


rg = [("REPLAYGAIN_TRACK_GAIN", "+6.00 dB"), ("REPLAYGAIN_TRACK_PEAK", "0.1"),
      ("REPLAYGAIN_REFERENCE_LOUDNESS", "89 dB")]
for ext, codec in [("flac", "flac"), ("mp3", "libmp3lame"), ("ogg", "libvorbis"), ("m4a", "aac")]:
    encode(f"rg.{ext}", codec, rg, ["-movflags", "use_metadata_tags"] if ext == "m4a" else [])
encode("missing.flac", "flac")
long_directory = args.output / ("x" * 180) / ("y" * 80)
long_directory.mkdir(parents=True, exist_ok=True)
shutil.copyfile(args.output / "missing.flac", long_directory / "long.flac")
encode("invalid.flac", "flac", [("REPLAYGAIN_TRACK_GAIN", "NaN")])
encode("r128.opus", "libopus", [("R128_TRACK_GAIN", "-512")])
encode("header.opus", "libopus", [("R128_TRACK_GAIN", "-512")])
encode("soundcheck.m4a", "aac", [("iTunNORM", "00001000 00001000 00000000 00000000")],
       ["-movflags", "use_metadata_tags"])

# Add a conflicting Vorbis-comment field without asking a tag writer to merge it.
data = (args.output / "rg.flac").read_bytes()
offset = 4
while True:
    kind = data[offset]
    size = int.from_bytes(data[offset + 1:offset + 4], "big")
    end = offset + 4 + size
    if kind & 127 == 4:
        block = data[offset + 4:end]
        vendor_length = struct.unpack_from("<I", block)[0]
        count_offset = 4 + vendor_length
        count = struct.unpack_from("<I", block, count_offset)[0]
        field = b"REPLAYGAIN_TRACK_GAIN=-8.00 dB"
        block = (block[:count_offset] + struct.pack("<I", count + 1) + block[count_offset + 4:]
                 + struct.pack("<I", len(field)) + field)
        data = data[:offset] + bytes([kind]) + len(block).to_bytes(3, "big") + block + data[end:]
        (args.output / "duplicate.flac").write_bytes(data)
        break
    if kind & 128:
        raise RuntimeError("FLAC comment block missing")
    offset = end

# Opus output gain is signed Q7.8 in OpusHead. Recompute the Ogg page CRC.
data = bytearray((args.output / "header.opus").read_bytes())
segments = data[26]
start = 27 + segments
assert data[start:start + 8] == b"OpusHead"
struct.pack_into("<h", data, start + 16, 6 * 256)
page_end = start + sum(data[27:start])
data[22:26] = b"\0" * 4
crc = 0
for byte in data[:page_end]:
    crc ^= byte << 24
    for _ in range(8):
        crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
struct.pack_into("<I", data, 22, crc)
(args.output / "header.opus").write_bytes(data)
print(f"Generated codec/tag fixtures in {args.output}")
