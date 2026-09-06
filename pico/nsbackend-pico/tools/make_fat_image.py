#!/usr/bin/env python3
#
# Helper script to generate a 1MB FAT filesystem image from a directory of files
# (e.g. fatfs_data/) and optionally convert it to a UF2 file for flashing to Raspberry Pi Pico.
#
# Design:
# 1. Attempts to locate fatfsgen.py (e.g. from ESP-IDF) and run it with Python virtualenv.
# 2. As a fallback, generates a FAT12 image using mkfs.fat and writes directory files into it.
# 3. If picotool is installed and a uf2_file path is given, converts the binary image to UF2.
#

import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
from typing import List, Optional, Tuple


def find_idf_python_with_construct() -> Optional[str]:
    """Finds a Python interpreter that has the 'construct' module installed."""
    candidates: List[str] = [sys.executable]

    home_dir: str = os.path.expanduser("~")
    glob_pattern: str = os.path.join(home_dir, ".espressif/python_env/*/bin/python")
    candidates.extend(glob.glob(glob_pattern))

    for py_bin in candidates:
        if os.path.isfile(py_bin) and os.access(py_bin, os.X_OK):
            try:
                res = subprocess.run(
                    [py_bin, "-c", "import construct"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
                if res.returncode == 0:
                    return py_bin
            except Exception:
                continue
    return None


def find_fatfsgen_script() -> Optional[str]:
    """Finds fatfsgen.py in standard locations."""
    idf_path: str = os.environ.get("IDF_PATH", os.path.expanduser("~/esp-idf"))
    candidate: str = os.path.join(idf_path, "components/fatfs/fatfsgen.py")
    if os.path.isfile(candidate):
        return candidate

    search_glob: str = os.path.expanduser("~/**/components/fatfs/fatfsgen.py")
    matches = glob.glob(search_glob, recursive=True)
    if matches:
        return matches[0]

    return None


def generate_fat_via_fatfsgen(
    py_bin: str,
    fatfsgen_path: str,
    input_dir: str,
    output_file: str,
    partition_size: int,
    sector_size: int,
) -> bool:
    """Generates FAT image using fatfsgen.py."""
    cmd: List[str] = [
        py_bin,
        fatfsgen_path,
        "--partition_size",
        str(partition_size),
        "--sector_size",
        str(sector_size),
        "--long_name_support",
        "--output_file",
        output_file,
        input_dir,
    ]
    print(f"Running fatfsgen: {' '.join(cmd)}")
    result = subprocess.run(cmd, check=False)
    return result.returncode == 0


def generate_fat_fallback(
    input_dir: str,
    output_file: str,
    partition_size: int,
) -> bool:
    """Fallback FAT12 image generator using mkfs.fat and manual file placement."""
    mkfs_bin: Optional[str] = shutil.which("mkfs.fat")
    if not mkfs_bin and os.path.isfile("/usr/sbin/mkfs.fat"):
        mkfs_bin = "/usr/sbin/mkfs.fat"

    if not mkfs_bin:
        print("Error: Neither fatfsgen.py nor mkfs.fat could be found.", file=sys.stderr)
        return False

    # Create empty image filled with zeros
    with open(output_file, "wb") as f:
        f.truncate(partition_size)

    # Format as FAT12 with 512-byte sectors and 4 sectors per cluster
    cmd: List[str] = [mkfs_bin, "-F", "12", "-S", "512", "-s", "4", "-n", "PICO_FAT", output_file]
    print(f"Formatting FAT12 image: {' '.join(cmd)}")
    res = subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL)
    if res.returncode != 0:
        print("mkfs.fat failed", file=sys.stderr)
        return False

    # Read boot sector parameters
    with open(output_file, "r+b") as f:
        bs: bytes = f.read(512)
        bytes_per_sector: int = struct.unpack_from("<H", bs, 11)[0]
        sectors_per_cluster: int = bs[13]
        reserved_sectors: int = struct.unpack_from("<H", bs, 14)[0]
        num_fats: int = bs[16]
        root_entries: int = struct.unpack_from("<H", bs, 17)[0]
        sectors_per_fat: int = struct.unpack_from("<H", bs, 22)[0]

        fat1_offset: int = reserved_sectors * bytes_per_sector
        fat_size_bytes: int = sectors_per_fat * bytes_per_sector
        root_dir_offset: int = fat1_offset + (num_fats * fat_size_bytes)
        root_dir_size_bytes: int = root_entries * 32
        data_cluster_offset: int = root_dir_offset + root_dir_size_bytes
        cluster_size_bytes: int = sectors_per_cluster * bytes_per_sector

        # Read existing FAT table
        f.seek(fat1_offset)
        fat_table: bytearray = bytearray(f.read(fat_size_bytes))

        def write_fat12_entry(cluster: int, value: int) -> None:
            byte_index: int = (cluster * 3) // 2
            if cluster % 2 == 0:
                fat_table[byte_index] = value & 0xFF
                fat_table[byte_index + 1] = (fat_table[byte_index + 1] & 0xF0) | ((value >> 8) & 0x0F)
            else:
                fat_table[byte_index] = (fat_table[byte_index] & 0x0F) | ((value & 0x0F) << 4)
                fat_table[byte_index + 1] = (value >> 4) & 0xFF

        next_cluster: int = 2
        dir_entry_idx: int = 0

        # Scan input directory files
        for fname in sorted(os.listdir(input_dir)):
            full_path: str = os.path.join(input_dir, fname)
            if not os.path.isfile(full_path):
                continue

            with open(full_path, "rb") as item_file:
                content: bytes = item_file.read()

            file_size: int = len(content)
            clusters_needed: int = max(1, (file_size + cluster_size_bytes - 1) // cluster_size_bytes)

            start_cluster: int = next_cluster
            curr_cluster: int = start_cluster

            # Write file data into clusters
            for c_idx in range(clusters_needed):
                c_offset: int = data_cluster_offset + ((curr_cluster - 2) * cluster_size_bytes)
                f.seek(c_offset)
                chunk = content[c_idx * cluster_size_bytes : (c_idx + 1) * cluster_size_bytes]
                f.write(chunk)

                if c_idx == clusters_needed - 1:
                    write_fat12_entry(curr_cluster, 0x0FFF)
                else:
                    write_fat12_entry(curr_cluster, curr_cluster + 1)
                    curr_cluster += 1
                next_cluster += 1

            # Prepare 8.3 filename entry
            name_parts = fname.rsplit(".", 1)
            base_name = name_parts[0].replace("-", "_").upper()[:8].ljust(8)
            ext_name = (name_parts[1].upper()[:3] if len(name_parts) > 1 else "").ljust(3)
            short_name_bytes = (base_name + ext_name).encode("ascii", errors="replace")

            # Write directory entry (32 bytes)
            # 0..10: short name, 11: attr (0x20 archive), 26..27: cluster, 28..31: size
            dentry: bytearray = bytearray(32)
            dentry[0:11] = short_name_bytes
            dentry[11] = 0x20
            struct.pack_into("<H", dentry, 26, start_cluster)
            struct.pack_into("<I", dentry, 28, file_size)

            f.seek(root_dir_offset + (dir_entry_idx * 32))
            f.write(dentry)
            dir_entry_idx += 1

        # Write updated FAT tables (both copies)
        f.seek(fat1_offset)
        f.write(fat_table)
        f.seek(fat1_offset + fat_size_bytes)
        f.write(fat_table)

    return True


def convert_to_uf2(bin_path: str, uf2_path: str, offset: str = "0x10100000") -> bool:
    """Converts a raw binary image to UF2 format using picotool."""
    picotool_bin: Optional[str] = shutil.which("picotool")
    if not picotool_bin:
        home_local: str = os.path.expanduser("~/.local/bin/picotool")
        if os.path.isfile(home_local):
            picotool_bin = home_local

    if not picotool_bin:
        print("Warning: picotool not found. Cannot convert to UF2.", file=sys.stderr)
        return False

    cmd: List[str] = [picotool_bin, "uf2", "convert", bin_path, "-o", offset, uf2_path]
    print(f"Converting to UF2: {' '.join(cmd)}")
    res = subprocess.run(cmd, check=False)
    return res.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Create FAT filesystem binary and UF2 image.")
    parser.add_argument("--input_dir", type=str, required=True, help="Input directory containing files")
    parser.add_argument("--output_file", type=str, required=True, help="Output FAT binary image file")
    parser.add_argument("--partition_size", type=int, default=1048576, help="Partition size in bytes (default: 1MB)")
    parser.add_argument("--sector_size", type=int, default=512, help="Sector size in bytes (default: 512)")
    parser.add_argument("--uf2_file", type=str, default="", help="Optional output UF2 file")
    parser.add_argument("--uf2_offset", type=str, default="0x10100000", help="Load offset for UF2 (default: 0x10100000)")

    args = parser.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.output_file)), exist_ok=True)

    py_bin: Optional[str] = find_idf_python_with_construct()
    fatfsgen_path: Optional[str] = find_fatfsgen_script()

    success: bool = False
    if py_bin and fatfsgen_path:
        success = generate_fat_via_fatfsgen(
            py_bin, fatfsgen_path, args.input_dir, args.output_file, args.partition_size, args.sector_size
        )

    if not success:
        print("Using fallback FAT generator...")
        success = generate_fat_fallback(args.input_dir, args.output_file, args.partition_size)

    if not success:
        print("Error: Failed to generate FAT image.", file=sys.stderr)
        return 1

    print(f"Generated FAT image: {args.output_file} ({os.path.getsize(args.output_file)} bytes)")

    if args.uf2_file:
        if not convert_to_uf2(args.output_file, args.uf2_file, args.uf2_offset):
            print("Warning: UF2 conversion failed.", file=sys.stderr)
        else:
            print(f"Generated UF2 image: {args.uf2_file}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
