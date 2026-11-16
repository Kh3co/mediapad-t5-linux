#!/usr/bin/env python3
"""Build the T5 loader with the reference kernels or a newly built Linux kernel."""
import argparse
import gzip
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

KIT = Path(__file__).resolve().parent
PARTITION_SIZE = 24 * 1024 * 1024
HEADER_SIZE = 2048
HANDOFF = b't5-loader-handoff'
MARKER = b'T5D115_NOT_EXECUTED'.ljust(128, b'N')
ANDROID_SHA = '9c30f4cedf72ff5bc1ac6e3d4e2c6d6f00a78123d6edaf4b5e929640017a65ec'


def fdt_layout(dtb):
    header = list(struct.unpack_from('>10I', dtb))
    magic, total, start, strings, reserve, version, _, _, strings_size, struct_size = header
    if not (magic == 0xd00dfeed and version >= 17 and total == len(dtb)
            and 40 <= reserve < start and start + struct_size <= strings
            and strings + strings_size == total):
        raise ValueError('Unsupported FDT layout')
    return header


def chosen_properties(dtb, header):
    pos = header[2]
    strings = header[3]
    nodes = []
    chosen_start = None
    handoff_offset = None
    while pos < strings:
        token, = struct.unpack_from('>I', dtb, pos)
        pos += 4
        if token == 1:
            end = dtb.index(b'\0', pos)
            nodes.append(dtb[pos:end])
            pos = (end + 4) & ~3
            if nodes == [b'', b'chosen']:
                chosen_start = pos
        elif token == 2:
            nodes.pop()
        elif token == 3:
            length, name_offset = struct.unpack_from('>II', dtb, pos)
            pos += 8
            name_start = strings + name_offset
            name = dtb[name_start:dtb.index(b'\0', name_start)]
            if nodes == [b'', b'chosen'] and name == HANDOFF:
                if length != 128:
                    raise ValueError('Unexpected handoff property length')
                handoff_offset = pos
            pos = (pos + length + 3) & ~3
        elif token == 9:
            break
        elif token != 4:
            raise ValueError(f'Unknown FDT token {token}')
    return chosen_start, handoff_offset


def add_handoff(dtb, image_size):
    header = fdt_layout(dtb)
    insert, existing = chosen_properties(dtb, header)
    if insert is None or existing is not None:
        raise ValueError('Expected an unmodified AGS2-L03 DTB with /chosen')
    padding = struct.pack('>I', 4) if (image_size + insert + 12) % 8 else b''
    prop = padding + struct.pack('>III', 3, len(MARKER), header[8]) + MARKER
    name = HANDOFF + b'\0'
    header[1] += len(prop) + len(name)
    header[3] += len(prop)
    header[8] += len(name)
    header[9] += len(prop)
    return struct.pack('>10I', *header) + dtb[40:insert] + prop + dtb[insert:] + name


def make_partition(template, payload, cmdline=None):
    if (len(template) != PARTITION_SIZE or template[:8] != b'ANDROID!'
            or HEADER_SIZE + len(payload) > PARTITION_SIZE):
        raise ValueError('Invalid boot template or oversized payload')
    header = bytearray(template[:HEADER_SIZE])
    if cmdline is not None:
        header[64:576] = cmdline.ljust(512, b'\0')
    struct.pack_into('<I', header, 8, len(payload))
    header[576:608] = hashlib.sha1(payload + struct.pack('<III', len(payload), 0, 0)).digest() + bytes(12)
    return (header + payload).ljust(PARTITION_SIZE, b'\0')


def build_loader(directory):
    def run(*args):
        return subprocess.run(args, cwd=directory, check=True, text=True, stdout=subprocess.PIPE).stdout

    flags = ('--target=aarch64-linux-gnu -O2 -DNDEBUG -DT5_BOOT_PROGRESS -ffreestanding -fno-builtin '
             '-fno-stack-protector -fno-pic -fno-jump-tables -fno-unwind-tables '
             '-fno-asynchronous-unwind-tables -mgeneral-regs-only -mstrict-align '
             '-Wall -Wextra -Werror').split()
    run('clang', '--target=aarch64-linux-gnu', '-c', 'loader.S', '-o', 'loader.o')
    sources = ['menu.c', 'boot.c', 'tinf/tinflate.c', 'tinf/tinfgzip.c', 'tinf/crc32.c']
    objects = ['loader.o']
    for source in sources:
        obj = Path(source).stem + '.o'
        run('clang', *flags, '-c', source, '-o', obj)
        objects.append(obj)
    run('ld.lld', '-T', 'link.ld', '--emit-relocs', '-Map=loader.map', *objects, '-o', 'loader.elf')
    run('llvm-objcopy', '-O', 'binary', 'loader.elf', 'loader.bin')
    if 'R_AARCH64_ABS' in run('llvm-readelf', '-rW', 'loader.elf'):
        raise ValueError('Absolute relocation in the movable loader')
    symbols = {}
    for line in run('llvm-nm', '-n', 'loader.elf').splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    raw = (directory / 'loader.bin').read_bytes()
    if len(raw) != symbols['image_end'] or len(raw) % 8 or 0x4000000 + len(raw) >= 0x7b00000:
        raise ValueError('Invalid relocated loader size')
    for name in ('android', 'linux'):
        if raw[symbols[name+'_gz_start']:symbols[name+'_gz_end']] != (directory / (name+'.gz')).read_bytes():
            raise ValueError(f'{name} payload differs after linking')
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux-dir', type=Path, required=True, help='Directory with Image and board.dtb')
    parser.add_argument('--android-template', type=Path, required=True,
                        help='Local unmodified AGS2-L03 Android kernel partition image')
    parser.add_argument('--output-dir', type=Path, default=KIT / 'out')
    parser.add_argument('--standalone-only', action='store_true', help='Package direct Linux boot only')
    args = parser.parse_args()
    linux_dir = args.linux_dir
    image = (linux_dir / 'Image').read_bytes()
    dtb = (linux_dir / 'board.dtb').read_bytes()
    if image[56:60] != b'ARM\x64':
        raise ValueError('Expected an uncompressed arm64 Image')
    if struct.unpack_from('<Q', image, 16)[0] + len(dtb) + 256 > 0x2189000:
        raise ValueError('Linux runtime image exceeds the reserved boot memory')
    output = args.output_dir
    output.mkdir(exist_ok=True)
    android_template = args.android_template.read_bytes()
    if hashlib.sha256(android_template).hexdigest() != ANDROID_SHA:
        raise ValueError('Android template SHA-256 mismatch for the tested AGS2-L03 firmware')
    linux_cmdline = (
        b'console=tty0 console=ttyGS0,115200 loglevel=8 ignore_loglevel keep_bootcon '
        b'printk.time=1 log_buf_len=1M clk_ignore_unused pd_ignore_unused nokaslr '
        b'cpuidle.off=1 maxcpus=8 fbcon=rotate:1 cma=96M systemd.gpt_auto=no '
        b'systemd.swap=no systemd.show_status=0 systemd.log_target=kmsg panic=0 rdinit=/init'
    )
    standalone = make_partition(android_template, image + dtb, linux_cmdline)
    (output / 'kernel-linux-ramdisk.img').write_bytes(standalone)
    if args.standalone_only:
        return

    android_size, = struct.unpack_from('<I', android_template, 8)
    android_gz = android_template[HEADER_SIZE:HEADER_SIZE + android_size]
    android_raw = gzip.decompress(android_gz)
    marked_dtb = add_handoff(dtb, len(image))
    linux_raw = image + marked_dtb
    linux_gz = gzip.compress(linux_raw, compresslevel=6, mtime=0)
    _, offset = chosen_properties(marked_dtb, fdt_layout(marked_dtb))
    if offset is None or (len(image) + offset) % 8 or marked_dtb[offset:offset+128] != MARKER:
        raise ValueError('Invalid or misaligned Linux handoff marker')

    with tempfile.TemporaryDirectory(prefix='dualboot-', dir=output) as tmp:
        directory = Path(tmp)
        shutil.copytree(KIT / 'dualboot', directory, dirs_exist_ok=True)
        (directory / 'linux.gz').write_bytes(linux_gz)
        (directory / 'android.gz').write_bytes(android_gz)
        (directory / 'payload-layout.h').write_text(
            f'#define ANDROID_RAW_SIZE {len(android_raw)}\n'
            f'#define LINUX_RAW_SIZE {len(linux_raw)}\n'
            f'#define LINUX_HANDOFF_OFFSET {len(image) + offset}\n', encoding='ascii')
        raw = build_loader(directory)
        (output / 'kernel-dualboot.img').write_bytes(make_partition(android_template, raw))
        for name in ('loader.elf', 'loader.map'):
            shutil.copyfile(directory / name, output / name)
    for name in ('kernel-linux-ramdisk.img', 'kernel-dualboot.img'):
        print(hashlib.sha256((output / name).read_bytes()).hexdigest(), name)


if __name__ == '__main__':
    main()
