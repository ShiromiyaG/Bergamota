"""A port's game addresses carried from the game they were found in to another
build of it (Ultra Sun's to Ultra Moon's, one region's to another's), by
signature. sdk/native.py uses it for port.toml's native_from.

    python -I sdk/addresses.py <reference> <target> <address>...

reference and target are an update (or a program) unpacked as the launcher
lays it, or as ctrtool does (exheader.bin, and code.bin or exefs/code.bin).
The port's code names the game's functions and words by their address in
the reference; for another game the address is looked up in that game's code
by the bytes around it in the reference, the way SpartanPort finds its patch
sites, except that no bytes are kept anywhere: the pattern is made from the
person's own copy of the reference each time.

- A function: its first instructions, the offsets of calls and jumps out of
  it masked (they change with where the rest of the code went), longer
  until it is found once. ARM or Thumb (an odd address).
- A word in the data (a vtable's entry, which PORT_PATCH changes): the
  function it points to carried over, and the word that points there in the
  target's data; when more than one does, the one whose neighbours (the
  other entries of the same vtable) agree.

An address that is not found once is left out, and native.py leaves out the
port's file that needs it: that feature is off for that game, as a
SpartanPort patch whose signature is not found is.

From decomp-tools' port_addresses.py (ShiromiyaG).
"""

import os
import struct
import sys

# instructions taken at first, and at most, for a function's pattern
FIRST, MOST = 8, 64
# neighbours compared for a word in the data
NEIGHBOURS = 8


class Program:
    """an executable's code.bin with its segments' addresses from the exheader"""

    def __init__(self, folder):
        exheader = open(os.path.join(folder, "exheader.bin"), "rb").read()
        code = os.path.join(folder, "code.bin")
        if not os.path.exists(code):
            code = os.path.join(folder, "exefs", "code.bin")
        self.bytes = open(code, "rb").read()
        self.segments = []
        offset = 0
        for at in (0x10, 0x20, 0x30):
            address, pages, size = struct.unpack_from("<III", exheader, at)
            self.segments.append((address, offset, size))
            offset += pages * 0x1000
        self.text = self.segments[0]

    def offset(self, address):
        for start, offset, size in self.segments:
            if start <= address < start + size:
                return offset + address - start
        return None

    def address(self, offset):
        for start, at, size in self.segments:
            if at <= offset < at + size:
                return start + offset - at
        return None

    def in_text(self, address):
        start, _, size = self.text
        return start <= (address & ~1) < start + size

    def word(self, address):
        offset = self.offset(address)
        if offset is None or offset + 4 > len(self.bytes):
            return None
        return struct.unpack_from("<I", self.bytes, offset)[0]

    def data_range(self):
        """where pointers live: rodata and data, as offsets"""
        (_, rodata, rodata_size), (_, data, data_size) = self.segments[1], self.segments[2]
        return [(rodata, rodata + rodata_size), (data, data + data_size)]


def arm_pattern(program, address, count):
    """(value, mask) words of count ARM instructions from address"""
    out = []
    for i in range(count):
        word = program.word(address + 4 * i)
        if word is None:
            break
        mask = 0xFFFFFFFF
        # b, bl and blx (immediate): the target moves with the code
        if (word >> 25) & 7 == 5:
            mask = 0xFF000000
        out.append((word, mask))
    return out


def thumb_pattern(program, address, count):
    """(value, mask) halfwords of count Thumb instructions from address"""
    out = []
    offset = program.offset(address)
    halves = []
    for i in range(count * 2):
        if offset is None or offset + 2 * i + 2 > len(program.bytes):
            break
        halves.append(struct.unpack_from("<H", program.bytes, offset + 2 * i)[0])
    i = 0
    while i < len(halves):
        half = halves[i]
        if half >> 11 in (0x1E,) and i + 1 < len(halves) and halves[i + 1] >> 11 in (0x1D, 0x1F):
            # bl / blx: a pair of halves, both offsets
            out += [(half, 0xF800), (halves[i + 1], 0xF800)]
            i += 2
            continue
        if half >> 11 == 0x1C:
            mask = 0xF800  # b: may leave the function
        else:
            mask = 0xFFFF
        out.append((half, mask))
        i += 1
    return out


def find_pattern(program, pattern, size, align):
    """offsets in the target's text where pattern ((value, mask) units of size
    bytes) matches, at most two"""
    fmt = "<I" if size == 4 else "<H"
    # an anchor: the longest run of whole units
    best, best_length, i = 0, 0, 0
    while i < len(pattern):
        j = i
        while j < len(pattern) and pattern[j][1] == (0xFFFFFFFF if size == 4 else 0xFFFF):
            j += 1
        if j - i > best_length:
            best, best_length = i, j - i
        i = j + 1
    data = program.bytes
    _, text, text_size = program.text
    end = text + text_size - len(pattern) * size
    found = []
    if best_length:
        anchor = b"".join(struct.pack(fmt, value) for value, _ in pattern[best:best + best_length])
        at = data.find(anchor, text + best * size)
        while at != -1 and at - best * size <= end:
            start = at - best * size
            if start % align == 0 and matches(data, start, pattern, size):
                found.append(start)
                if len(found) > 1:
                    break
            at = data.find(anchor, at + 1)
        return found
    for start in range(text, end + 1, align):
        if matches(data, start, pattern, size):
            found.append(start)
            if len(found) > 1:
                break
    return found


def matches(data, start, pattern, size):
    fmt = "<I" if size == 4 else "<H"
    for i, (value, mask) in enumerate(pattern):
        if (struct.unpack_from(fmt, data, start + i * size)[0] ^ value) & mask:
            return False
    return True


def map_function(reference, target, address):
    """address in the target of the reference's function at address (odd for
    Thumb), or None"""
    thumb = address & 1
    at = address & ~1
    size = 2 if thumb else 4
    make = thumb_pattern if thumb else arm_pattern

    def search(start, count, skip):
        """the target's matches of count units from start, skip of them before
        the function"""
        pattern = make(reference, start, count)
        if len(pattern) <= skip:
            return None, pattern
        found = find_pattern(target, pattern, size, size)
        return [offset + (at - start) for offset in found], pattern

    count = FIRST
    while count <= MOST:
        found, pattern = search(at, count, 0)
        if not pattern:
            return None
        if found and len(found) == 1:
            return target.address(found[0]) | thumb
        if not found:
            return None
        if len(pattern) < count:
            break  # the end of the code, still more than one
        count *= 2
    # the same code in more than one place (a small function made twice):
    # the one that follows the same code as in the reference
    for back in (4, 8, 16, 32):
        found, pattern = search(at - back * 4, back * 4 // size + FIRST, back * 4 // size)
        if found and len(found) == 1:
            return target.address(found[0]) | thumb
        if not found:
            return None
    # still more than one: the one as far from where it was as the code
    # around it moved, code found once before it and after it agreeing
    shifts = []
    for direction in (-1, 1):
        for step in range(1, 33):
            near = at + direction * step * 64
            if not reference.in_text(near):
                break
            pattern = arm_pattern(reference, near, FIRST)
            there = find_pattern(target, pattern, 4, 4) if len(pattern) == FIRST else []
            if len(there) == 1:
                shifts.append(there[0] - reference.offset(near))
                break
    if shifts and all(shift == shifts[0] for shift in shifts):
        offset = reference.offset(at) + shifts[0]
        pattern = make(reference, at, FIRST)
        if 0 <= offset and offset + len(pattern) * size <= len(target.bytes) and matches(target.bytes, offset, pattern, size):
            return target.address(offset) | thumb
    return None


def map_word(reference, target, address, functions):
    """address in the target's data of the reference's word at address, which
    points to code, or None. functions caches map_function."""
    def mapped(value):
        if value is None or not reference.in_text(value):
            return None
        if value not in functions:
            functions[value] = map_function(reference, target, value)
        return functions[value]

    value = mapped(reference.word(address))
    if value is None:
        return None
    word = struct.pack("<I", value)
    candidates = []
    for start, end in target.data_range():
        at = target.bytes.find(word, start, end)
        while at != -1:
            if at % 4 == 0:
                candidates.append(at)
            at = target.bytes.find(word, at + 1, end)
    if len(candidates) == 1:
        return target.address(candidates[0])
    # the other entries around it: the same vtable carried over
    best, best_score, tie = None, -1, False
    for candidate in candidates:
        score = 0
        for d in range(-NEIGHBOURS, NEIGHBOURS + 1):
            if not d:
                continue
            theirs = mapped(reference.word(address + 4 * d))
            if theirs is None:
                continue
            offset = candidate + 4 * d
            if 0 <= offset and offset + 4 <= len(target.bytes) and struct.unpack_from("<I", target.bytes, offset)[0] == theirs:
                score += 1
        if score > best_score:
            best, best_score, tie = candidate, score, False
        elif score == best_score:
            tie = True
    if best is None or tie or best_score == 0:
        return None
    return target.address(best)


def map_addresses(reference, target, functions=(), words=()):
    """{reference address: target address or None} for functions (odd for
    Thumb) and data words"""
    out, cache = {}, {}
    for address in functions:
        if address not in cache:
            cache[address] = map_function(reference, target, address)
        out[address] = cache[address]
    for address in words:
        out[address] = map_word(reference, target, address, cache)
    return out


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    reference, target = Program(sys.argv[1]), Program(sys.argv[2])
    for text in sys.argv[3:]:
        address = int(text, 16)
        if reference.in_text(address):
            result = map_function(reference, target, address)
        else:
            result = map_word(reference, target, address, {})
        print(f"0x{address:08X} -> " + (f"0x{result:08X}" if result is not None else "not found"))


if __name__ == "__main__":
    main()
