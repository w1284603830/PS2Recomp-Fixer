"""Annotate a DumpRange.java listing of an IRX .text with its import stubs (library#ordinal) and print a range.

Usage: python -I irx_annotate.py <irx> <listing.asm> <start hex> <end hex>
"""
import re
import struct
import sys

NAMES = {
    ("sysmem", 4): "AllocSysMemory", ("sysmem", 5): "FreeSysMemory", ("sysmem", 7): "QueryMaxFreeMemSize",
    ("sysmem", 8): "QueryTotalFreeMemSize", ("loadcore", 6): "RegisterLibraryEntries", ("loadcore", 12): "QueryBootMode",
    ("thbase", 4): "CreateThread", ("thbase", 5): "DeleteThread", ("thbase", 6): "StartThread", ("thbase", 8): "ExitThread",
    ("thbase", 20): "GetThreadId", ("thbase", 24): "SleepThread", ("thbase", 25): "WakeupThread", ("thbase", 26): "iWakeupThread",
    ("thbase", 33): "DelayThread", ("thbase", 34): "GetSystemTime", ("thbase", 39): "USec2SysClock", ("thbase", 40): "SysClock2USec",
    ("thsemap", 4): "CreateSema", ("thsemap", 5): "DeleteSema", ("thsemap", 6): "SignalSema", ("thsemap", 7): "iSignalSema",
    ("thsemap", 8): "WaitSema", ("thsemap", 9): "PollSema", ("thevent", 4): "CreateEventFlag", ("thevent", 6): "SetEventFlag",
    ("thevent", 7): "iSetEventFlag", ("thevent", 8): "ClearEventFlag", ("thevent", 10): "WaitEventFlag",
    ("intrman", 4): "RegisterIntrHandler", ("intrman", 5): "ReleaseIntrHandler", ("intrman", 6): "EnableIntr",
    ("intrman", 7): "DisableIntr", ("intrman", 17): "CpuSuspendIntr", ("intrman", 18): "CpuResumeIntr",
    ("sifman", 5): "sceSifInit", ("sifman", 7): "sceSifSetDma", ("sifman", 8): "sceSifDmaStat", ("sifman", 29): "sceSifCheckInit",
    ("sifcmd", 14): "sceSifInitRpc", ("sifcmd", 15): "sceSifBindRpc", ("sifcmd", 16): "sceSifCallRpc",
    ("sifcmd", 17): "sceSifRegisterRpc", ("sifcmd", 18): "sceSifCheckStatRpc", ("sifcmd", 19): "sceSifSetRpcQueue",
    ("sifcmd", 20): "sceSifGetNextRequest", ("sifcmd", 21): "sceSifExecRequest", ("sifcmd", 22): "sceSifRpcLoop",
    ("stdio", 4): "printf", ("sysclib", 12): "memcpy", ("sysclib", 14): "memset", ("sysclib", 23): "strcpy",
    ("sysclib", 27): "strlen", ("timrman", 4): "AllocHardTimer", ("timrman", 20): "SetTimerHandler",
    ("vblank", 4): "WaitVblankStart", ("vblank", 8): "RegisterVblankHandler",
}


def imports(irx):
    d = open(irx, "rb").read()
    text_off = 0xB0
    shoff = struct.unpack_from("<I", d, 32)[0]
    shnum = struct.unpack_from("<H", d, 48)[0]
    for i in range(shnum):
        sh = struct.unpack_from("<10I", d, shoff + i * 40)
        if sh[1] == 1 and sh[3] == 0 and sh[5] > 0x100:
            text_off, text_size = sh[4], sh[5]
            break
    text = d[text_off:text_off + text_size]
    stubs = {}
    for pos in range(0, len(text) - 20, 4):
        if struct.unpack_from("<I", text, pos)[0] != 0x41E00000:
            continue
        name = text[pos + 12:pos + 20].split(b"\0")[0].decode("ascii", "replace")
        p = pos + 20
        while p + 8 <= len(text):
            jr, addiu = struct.unpack_from("<II", text, p)
            if jr != 0x03E00008 or (addiu >> 16) != 0x2400:
                break
            ordinal = addiu & 0xFFFF
            stubs[p] = "%s#%d %s" % (name, ordinal, NAMES.get((name, ordinal), ""))
            p += 8
    return stubs


def main():
    irx, listing, start, end = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
    stubs = imports(irx)
    for line in open(listing):
        off = int(line[:6], 16)
        if not (start <= off < end):
            continue
        note = ""
        m = re.search(r"jal 0x([0-9a-fA-F]+)", line)
        if m and int(m.group(1), 16) in stubs:
            note = "    ; -> " + stubs[int(m.group(1), 16)]
        elif off in stubs:
            note = "    ; stub " + stubs[off]
        print(line.rstrip() + note)


if __name__ == "__main__":
    main()
