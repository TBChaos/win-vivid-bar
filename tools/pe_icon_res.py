#!/usr/bin/env python
# tools/pe_icon_res.py — 校验 Windows PE 是否内嵌图标资源（RT_GROUP_ICON / RT_ICON）。
#
# 用法：
#   python tools/pe_icon_res.py [path\to\openDock.exe]   （缺省扫 release/openDock.exe）
# 退出码：0 = 已内嵌图标组；1 = 未找到（或文件不是 PE）。
#
# 背景：CMake/Ninja 曾把 app.res 当作 order-only 依赖漏链接，导致 exe 无图标，
# 文件/任务栏/托盘图标全部回退系统默认。用本脚本可脱离 GUI 快速核验构建产物。
import struct
import sys

PATH = sys.argv[1] if len(sys.argv) > 1 else r"D:\code\openDock\release\openDock.exe"

RT_GROUP_ICON = 14
RT_ICON = 3
TYPE_NAMES = {RT_GROUP_ICON: "GROUP_ICON", RT_ICON: "ICON", 24: "MANIFEST"}


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def main():
    with open(PATH, "rb") as f:
        data = f.read()

    if data[:2] != b"MZ":
        print("!! not a PE file: %s" % PATH)
        return 1
    pe_off = u32(data, 0x3C)
    if data[pe_off:pe_off + 4] != b"PE\x00\x00":
        print("!! no PE signature: %s" % PATH)
        return 1

    # COFF header + optional header -> data directory[2] (resource) RVA/size
    coff = pe_off + 4
    num_sections = u16(data, coff + 2)
    opt_hdr_size = u16(data, coff + 16)
    opt_hdr_off = coff + 20
    magic = u16(data, opt_hdr_off)               # 0x10B PE32, 0x20B PE32+
    dd_base = opt_hdr_off + (112 if magic == 0x20B else 96)
    rsrc_rva = u32(data, dd_base + 2 * 8)        # dir[2].VirtualAddress
    rsrc_size = u32(data, dd_base + 2 * 8 + 4)   # dir[2].Size

    # Section table -> RVA-to-file-offset mapper
    sec_off = opt_hdr_off + opt_hdr_size
    sections = []
    for i in range(num_sections):
        so = sec_off + i * 40
        sections.append((
            data[so:so + 8].rstrip(b"\x00").decode("latin1", "replace"),
            u32(data, so + 12),   # vaddr
            u32(data, so + 8),    # vsize
            u32(data, so + 20),   # raw offset
            u32(data, so + 16),   # raw size
        ))

    def rva_to_off(rva):
        for (_, vaddr, vsize, roff, rsize) in sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return roff + (rva - vaddr)
        return None

    base = rva_to_off(rsrc_rva)
    if base is None:
        print("!! no .rsrc section -> exe has NO embedded resources (no icon)")
        return 1

    def read_dir(off):
        named = u16(data, off + 12)
        ids = u16(data, off + 14)
        out = []
        ent = off + 16
        for _ in range(named + ids):
            name_field = u32(data, ent)
            data_field = u32(data, ent + 4)
            out.append((name_field, bool(name_field & 0x80000000),
                        bool(data_field & 0x80000000), data_field))
            ent += 8
        return out

    def name_string(name_off):
        abs_off = base + (name_off & 0x7FFFFFFF)
        length = u16(data, abs_off)
        return data[abs_off + 2:abs_off + 2 + length * 2].decode("utf-16-le", "replace")

    type_entries = read_dir(base)
    types = []
    for (nf, is_name, _, _) in type_entries:
        tid = nf & 0x7FFFFFFF
        types.append(TYPE_NAMES.get(tid, "type_%d" % tid) if not is_name
                     else name_string(nf))
    print("resource types: %s" % (", ".join(types) if types else "<none>"))

    groups = []
    for (nf, is_name, _, df) in type_entries:
        tid = nf & 0x7FFFFFFF
        if tid == RT_GROUP_ICON:
            child = base + (df & 0x7FFFFFFF)
            for (gnf, gis, _, _) in read_dir(child):
                gname = name_string(gnf) if gis else str(gnf & 0x7FFFFFFF)
                groups.append("%s [%s]" % (gname, "STRING" if gis else "INTEGER"))
        elif tid == RT_ICON:
            child = base + (df & 0x7FFFFFFF)
            print("RT_ICON (type 3): %d image frame(s)" %
                  (u16(data, child + 12) + u16(data, child + 14)))

    if groups:
        print("RT_GROUP_ICON (type 14) found; icon groups: %s" % ", ".join(groups))
        print("OK: %s embeds the application icon" % PATH)
        return 0
    print("!! No RT_GROUP_ICON resource found in %s" % PATH)
    return 1


if __name__ == "__main__":
    sys.exit(main())
