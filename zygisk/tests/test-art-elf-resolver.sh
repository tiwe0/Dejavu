#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
CC=${CC:-cc}
CXX=${CXX:-c++}
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/host-tests"}
mkdir -p "${OUT_DIR}/xz"

XZ_DIR="${ROOT_DIR}/third_party/xz-embedded"
XZ_FLAGS=(
    -std=c11 -O2 -w -fvisibility=hidden
    -DXZ_USE_CRC64 -DXZ_USE_SHA256 -DXZ_DEC_ANY_CHECK -DXZ_DEC_CONCATENATED
    -DXZ_DEC_ARM64 -DXZ_DEC_X86 -DXZ_DEC_ARM -DXZ_DEC_ARMTHUMB
    -I"${XZ_DIR}"
)
XZ_OBJS=()
for src in xz_crc32.c xz_crc64.c xz_sha256.c xz_dec_bcj.c xz_dec_lzma2.c xz_dec_stream.c; do
    obj="${OUT_DIR}/xz/${src%.c}.o"
    "${CC}" "${XZ_FLAGS[@]}" -c "${XZ_DIR}/${src}" -o "${obj}"
    XZ_OBJS+=("${obj}")
done

"${CXX}" \
    -std=c++17 -O2 -Wall -Wextra -Werror \
    -DDEJAVU_WITH_XZ_EMBEDDED=1 \
    -DXZ_USE_CRC64 -DXZ_DEC_CONCATENATED \
    -I"${ROOT_DIR}/include" \
    -I"${XZ_DIR}" \
    "${ROOT_DIR}/src/art_elf_resolver.cpp" \
    "${ROOT_DIR}/tests/art_elf_resolver_test.cpp" \
    "${XZ_OBJS[@]}" \
    -o "${OUT_DIR}/art_elf_resolver_test"

# Also verify art_elf_resolver.cpp compiles cleanly without DEJAVU_WITH_XZ_EMBEDDED.
"${CXX}" \
    -std=c++17 -O2 -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    -c "${ROOT_DIR}/src/art_elf_resolver.cpp" \
    -o "${OUT_DIR}/art_elf_resolver_noxz.o"

TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

python3 - "${TMP_DIR}" <<'PY'
import lzma
import pathlib
import struct
import sys

out_dir = pathlib.Path(sys.argv[1])

def build_strtab(strings):
    data = bytearray(b"\0")
    offsets = {}
    for s in strings:
        offsets[s] = len(data)
        data.extend(s.encode("utf-8") + b"\0")
    return bytes(data), offsets

def build_symtab(entries, str_offsets):
    out = bytearray(struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0))
    for name, value, st_info, st_shndx in entries:
        out.extend(struct.pack("<IBBHQQ", str_offsets[name], st_info, 0, st_shndx, value, 16))
    return bytes(out)

def build_elf(sections_spec):
    shstr_names = [spec[0] for spec in sections_spec] + [".shstrtab"]
    shstrtab, sh_offsets = build_strtab(shstr_names)
    all_sections = list(sections_spec) + [(".shstrtab", 3, shstrtab, 0, 0)]

    ehdr_size = 64
    shdr_size = 64
    body = bytearray()
    shdrs = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for name, sh_type, data, sh_link, sh_entsize in all_sections:
        while len(body) % 8 != 0:
            body.append(0)
        offset = ehdr_size + len(body)
        body.extend(data)
        shdrs.append(
            struct.pack(
                "<IIQQQQIIQQ",
                sh_offsets[name],
                sh_type,
                0,
                0,
                offset,
                len(data),
                sh_link,
                0,
                8,
                sh_entsize,
            )
        )

    while len(body) % 8 != 0:
        body.append(0)
    shoff = ehdr_size + len(body)
    shnum = len(shdrs)
    shstrndx = shnum - 1

    e_ident = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH",
        e_ident,
        3,
        183,
        1,
        0, 0, shoff, 0,
        ehdr_size, 0, 0, shdr_size, shnum, shstrndx,
    )
    return ehdr + bytes(body) + b"".join(shdrs)

inner_strtab, inner_offs = build_strtab([
    "_ZN3art11ClassLinker10FixupStaticEv",
    "shared_symbol",
    "abs_constant_symbol",
])
inner_symtab = build_symtab([
    ("_ZN3art11ClassLinker10FixupStaticEv", 0x2400, (1 << 4) | 2, 1),
    ("shared_symbol", 0x9999, (1 << 4) | 2, 1),
    ("abs_constant_symbol", 0x42, (1 << 4) | 1, 0xfff1),  # SHN_ABS
], inner_offs)
mini_elf = build_elf([
    (".symtab", 2, inner_symtab, 2, 24),
    (".strtab", 3, inner_strtab, 0, 0),
])

# Build a > 160 KiB mini-ELF so XZ decompression MUST span multiple 64 KiB output chunks!
big_names = ["_ZN3art11ClassLinker10FixupStaticEv"] + [
    f"_ZN3art11PaddingSym{i:04d}32bytes_padding_for_chunk_boundaryEv" for i in range(2500)
] + ["tail_symbol_after_128k"]
big_strtab, big_offs = build_strtab(big_names)
big_entries = [("_ZN3art11ClassLinker10FixupStaticEv", 0x2400, (1 << 4) | 2, 1)]
for i in range(2500):
    big_entries.append((big_names[i + 1], 0x3000 + i * 4, (1 << 4) | 2, 1))
big_entries.append(("tail_symbol_after_128k", 0x4800, (1 << 4) | 2, 1))
big_symtab = build_symtab(big_entries, big_offs)
big_mini_elf = build_elf([
    (".symtab", 2, big_symtab, 2, 24),
    (".strtab", 3, big_strtab, 0, 0),
])
assert len(big_mini_elf) > 160 * 1024

dyn_strtab, dyn_offs = build_strtab([
    "exported_art_fn",
    "shared_symbol",
])
dyn_symtab = build_symtab([
    ("exported_art_fn", 0x1000, (1 << 4) | 2, 1),
    ("shared_symbol", 0x1100, (1 << 4) | 2, 1),
], dyn_offs)

def write_outer(filename, debugdata_bytes):
    outer = build_elf([
        (".dynsym", 11, dyn_symtab, 2, 24),
        (".dynstr", 3, dyn_strtab, 0, 0),
        (".gnu_debugdata", 1, debugdata_bytes, 0, 0),
    ])
    (out_dir / filename).write_bytes(outer)

filters = [{"id": lzma.FILTER_LZMA2}]
if hasattr(lzma, "FILTER_ARM64"):
    filters = [{"id": lzma.FILTER_ARM64}, {"id": lzma.FILTER_LZMA2}]

valid_xz = lzma.compress(mini_elf, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, filters=filters)
write_outer("crc32.elf", valid_xz)
write_outer("crc64.elf", lzma.compress(mini_elf, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64))
write_outer("sha256.elf", lzma.compress(mini_elf, format=lzma.FORMAT_XZ, check=lzma.CHECK_SHA256))
write_outer("multichunk.elf", lzma.compress(big_mini_elf, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, filters=filters))

# .gnu_debugdata-only ELF (no .dynsym or .symtab in outer image)
(out_dir / "debugonly.elf").write_bytes(build_elf([
    (".gnu_debugdata", 1, valid_xz, 0, 0),
]))

# Outer .symtab-only ELF (no .dynsym or .gnu_debugdata)
sym_strtab, sym_offs = build_strtab(["outer_symtab_fn"])
sym_symtab = build_symtab([("outer_symtab_fn", 0x3200, (1 << 4) | 2, 1)], sym_offs)
(out_dir / "symtab.elf").write_bytes(build_elf([
    (".symtab", 2, sym_symtab, 2, 24),
    (".strtab", 3, sym_strtab, 0, 0),
]))

write_outer("corrupt.elf", b"\xfd7zXZ\x00not-a-valid-xz-stream")
write_outer("trailing.elf", valid_xz + b"GARBAGE!")
PY

"${OUT_DIR}/art_elf_resolver_test" \
    "${TMP_DIR}/crc32.elf" \
    "${TMP_DIR}/crc64.elf" \
    "${TMP_DIR}/sha256.elf" \
    "${TMP_DIR}/multichunk.elf" \
    "${TMP_DIR}/debugonly.elf" \
    "${TMP_DIR}/symtab.elf" \
    "${TMP_DIR}/corrupt.elf" \
    "${TMP_DIR}/trailing.elf"
