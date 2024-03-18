#!/usr/bin/env python3

import argparse
import pathlib


def parse_args():
    p = argparse.ArgumentParser(description='Generate simplified NC1020 ROM from official simulator format.')
    p.add_argument('rom_path', type=pathlib.Path,
                   help='Path to the simulator ROM image file (usually named obj_lu.bin)')
    p.add_argument('nor_path', type=pathlib.Path,
                   help='Path to the simulator NOR flash image file (usually named nc1020.fls or flash.ini)')
    p.add_argument('output', type=pathlib.Path,
                   help='Path to output directory')
    return p, p.parse_args()


def unshuffle_pages(input_file, output_file, count):
    for i in range(count):
        page = input_file.read(0x8000)
        if len(page) != 0x8000:
            raise RuntimeError('Input file misaligned or truncated.')
        output_file.write(page[0x4000:])
        output_file.write(page[:0x4000])


def dump_bbs(input_file, output_file):
    input_file.seek(0)
    unshuffle_pages(input_file, output_file, 4)


def dump_rom(input_file, output_file):
    for volume in range(3):
        input_file.seek(0x8000*0x80 + volume * 0x8000 * 0x100)
        unshuffle_pages(input_file, output_file, 0x80)


def dump_nor(input_file, output_file):
    input_file.seek(0)
    unshuffle_pages(input_file, output_file, 0x20)


def main():
    p, args = parse_args()
    args.output.mkdir(exist_ok=True)
    with args.rom_path.open('rb') as rom_file:
        with (args.output / 'bbs.bin').open('wb') as bbs_file:
            dump_bbs(rom_file, bbs_file)
        with (args.output / 'rom.bin').open('wb') as rom_simp_file:
            dump_rom(rom_file, rom_simp_file)
    with args.nor_path.open('rb') as nor_file:
        with (args.output / 'nor.bin').open('wb') as nor_simp_file:
            dump_nor(nor_file, nor_simp_file)

if __name__ == '__main__':
    main()
