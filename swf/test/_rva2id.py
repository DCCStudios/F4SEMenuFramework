# Maps a runtime RVA back to its Address Library ID. Fallout 4's
# version-*.bin format is flat: u64 count, then count x (u64 id, u64 offset).
# Scratch diagnostic tool.
import struct, sys, bisect

def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    count = struct.unpack_from('<Q', data, 0)[0]
    pairs = []
    off = 8
    for _ in range(count):
        i, o = struct.unpack_from('<QQ', data, off)
        off += 16
        pairs.append((i, o))
    return pairs

def main():
    binpath = sys.argv[1]
    rvas = [int(a, 0) for a in sys.argv[2:]]
    pairs = load(binpath)
    print(f'{len(pairs)} entries')
    by_off = sorted((o, i) for i, o in pairs)
    offs = [o for o, _ in by_off]
    for rva in rvas:
        pos = bisect.bisect_right(offs, rva) - 1
        if pos < 0:
            print(f'RVA {rva:#x}: below first entry')
            continue
        o, i = by_off[pos]
        print(f'RVA {rva:#x}: nearest ID {i} at offset {o:#x} (delta +{rva - o:#x})')
        if pos + 1 < len(by_off):
            o2, i2 = by_off[pos + 1]
            print(f'           next ID {i2} at offset {o2:#x}')

if __name__ == '__main__':
    main()
