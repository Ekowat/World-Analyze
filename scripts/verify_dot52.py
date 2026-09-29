"""Audit every signature against a supplied ELF; requires only Python 3."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def executable_sections(data):
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected a little-endian ELF64 file')
    if struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('Expected AArch64')
    shoff = struct.unpack_from('<Q', data, 40)[0]
    size, count = struct.unpack_from('<HH', data, 58)
    for i in range(count):
        _, _, flags, addr, off, length, *_ = struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * size)
        if flags & 4:
            yield addr, data[off:off + length]


def matches(pattern, sections):
    parts = pattern.split()
    chunks, current, start = [], [], 0
    for i, value in enumerate(parts + ['?']):
        if value != '?':
            if not current:
                start = i
            current.append(int(value, 16))
        elif current:
            chunks.append((start, bytes(current)))
            current = []
    anchor_offset, anchor = max(chunks, key=lambda x: len(x[1]))
    hits = []
    for base, data in sections:
        pos = 0
        while True:
            found = data.find(anchor, pos)
            if found < 0:
                break
            pos = found + 1
            candidate = found - anchor_offset
            if candidate >= 0 and candidate + len(parts) <= len(data) and all(
                data[candidate + offset:candidate + offset + len(chunk)] == chunk
                for offset, chunk in chunks
            ):
                hits.append(base + candidate)
    return hits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    data = args.binary.read_bytes()
    profile = json.loads((root / 'compatibility/dot52-bindings.json').read_text())
    digest = hashlib.sha256(data).hexdigest()
    if digest != profile['sha256']:
        raise SystemExit('Target SHA-256 differs from the audited dot52 binary')
    definitions = re.findall(r'SignatureDefinition\{SignatureId::(\w+), "([^"]+)"',
                            (root / 'src/core/memory/Signatures.cpp').read_text())
    expected = {row['id']: int(row['rva'], 16) for row in profile['signatures']}
    assert len(definitions) == len(expected) == 110
    sections = list(executable_sections(data))
    failures = []
    for name, pattern in definitions:
        found = matches(pattern, sections)
        if found != [expected[name]]:
            failures.append((name, [hex(x) for x in found]))
    if failures:
        raise SystemExit(json.dumps(failures, indent=2))
    print('PASS: 110/110 signatures match exactly once at their recorded ARM64 RVAs.')
    print('Target SHA-256: ' + digest)
    print('This checks byte matching, not Android runtime behavior or every object layout.')


if __name__ == '__main__':
    main()
