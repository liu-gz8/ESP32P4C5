"""校验 ESP32 flash 全片备份的结构是否合理。

不依赖 esptool,直接按偏移解析:
  - 0x2000  bootloader(镜像 magic 0xE9)
  - 0x8000  分区表(magic 0x50AA),逐条列出分区
  - 每个 app 类型分区的偏移处也应是 0xE9

用法:
    python verify_flash_backup.py <backup.bin>
"""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

ESP_IMAGE_MAGIC = 0xE9
PARTITION_MAGIC = 0x50AA
# 分区表偏移由 bootloader 的 CONFIG_PARTITION_TABLE_OFFSET 决定,
# 可能是 0x8000 或 0x9000,所以两个位置都探一下。
PARTITION_TABLE_CANDIDATES = (0x8000, 0x9000)
ENTRY_SIZE = 32
MAX_ENTRIES = 95
TYPE_NAMES = {0x00: "app", 0x01: "data"}
SUBTYPE_NAMES = {
    (0x00, 0x00): "factory",
    (0x00, 0x10): "ota_0",
    (0x00, 0x11): "ota_1",
    (0x01, 0x00): "ota",
    (0x01, 0x01): "phy",
    (0x01, 0x02): "nvs",
    (0x01, 0x82): "spiffs",
    (0x01, 0x81): "fat",
}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    path = Path(sys.argv[1])
    data = path.read_bytes()
    print(f"文件: {path}")
    print(f"大小: {len(data)} 字节 ({len(data) / 1024 / 1024:.1f} MB)")
    print(f"sha256: {hashlib.sha256(data).hexdigest().upper()}")

    ok = True

    if data[0x2000] == ESP_IMAGE_MAGIC:
        print("0x2000 bootloader: 镜像 magic 0xE9 正确")
    else:
        print(f"0x2000 bootloader: magic 异常(读到 0x{data[0x2000]:02X})")
        ok = False

    partition_table_offset = None
    for candidate in PARTITION_TABLE_CANDIDATES:
        if struct.unpack_from("<H", data, candidate)[0] == PARTITION_MAGIC:
            partition_table_offset = candidate
            break

    if partition_table_offset is None:
        print("分区表: 在 0x8000 与 0x9000 都没找到 magic 0x50AA")
        return 1
    print(f"0x{partition_table_offset:X} 分区表: magic 0x50AA 正确\n")

    print(f"{'名称':<12}{'类型':<8}{'子类型':<10}{'偏移':<12}{'大小':<12}")
    print("-" * 56)
    for index in range(MAX_ENTRIES):
        base = partition_table_offset + index * ENTRY_SIZE
        entry = data[base : base + ENTRY_SIZE]
        if len(entry) < ENTRY_SIZE or entry[0:2] == b"\xff\xff":
            break
        # on-flash 条目布局:magic(2) type(1) subtype(1) offset(4) size(4) label(16) flags(4)
        ptype, subtype = entry[2], entry[3]
        offset, size = struct.unpack_from("<II", entry, 4)
        label = entry[12:28].split(b"\x00")[0].decode("ascii", "replace")
        type_name = TYPE_NAMES.get(ptype, f"0x{ptype:02X}")
        subtype_name = SUBTYPE_NAMES.get((ptype, subtype), f"0x{subtype:02X}")
        print(f"{label:<12}{type_name:<8}{subtype_name:<10}0x{offset:08X}  0x{size:08X}")

        if ptype == 0x00 and offset + 4 <= len(data):
            if data[offset] != ESP_IMAGE_MAGIC:
                print(f"  !! app 分区 {label} 在 0x{offset:X} 处没有镜像 magic")
                ok = False

    print()
    print("结论: " + ("结构正常,可用于复原" if ok else "结构异常,不要用它做复原依据"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
