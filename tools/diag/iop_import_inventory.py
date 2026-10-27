"""Offline inventory of the IOP imports: every library:ordinal the game's IRX modules import, and whether ps2xIOP
handles it (an HLE dispatcher case), resolves it to a loaded module's export (libsd, sio2man ... are real IRX), or
neither (MISSING: the call logs "[IOP] missing import" and returns 0).

Usage: python -I iop_import_inventory.py <dir with .IRX> <ps2xIOP src dir>
"""
import glob
import os
import re
import struct
import sys

sys.stdout.reconfigure(encoding='utf-8')
irx_dir, src = sys.argv[1], sys.argv[2]

NAMES = {
    ('sysmem', 4): 'AllocSysMemory', ('sysmem', 5): 'FreeSysMemory', ('sysmem', 7): 'QueryMaxFreeMemSize',
    ('sysmem', 8): 'QueryTotalFreeMemSize', ('sysmem', 14): 'Kprintf', ('loadcore', 5): 'FlushDcache',
    ('loadcore', 6): 'RegisterLibraryEntries', ('loadcore', 7): 'ReleaseLibraryEntries', ('thbase', 4): 'CreateThread',
    ('thbase', 5): 'DeleteThread', ('thbase', 6): 'StartThread', ('thbase', 10): 'ExitDeleteThread',
    ('thbase', 14): 'ChangeThreadPriority', ('thbase', 20): 'GetThreadId', ('thbase', 22): 'ReferThreadStatus',
    ('thbase', 24): 'SleepThread', ('thbase', 26): 'iWakeupThread', ('thbase', 28): 'iCancelWakeupThread?',
    ('thbase', 33): 'DelayThread', ('thbase', 39): 'USec2SysClock', ('thsemap', 4): 'CreateSema',
    ('thsemap', 5): 'DeleteSema', ('thsemap', 6): 'SignalSema', ('thsemap', 7): 'iSignalSema', ('thsemap', 8): 'WaitSema',
    ('intrman', 4): 'RegisterIntrHandler', ('intrman', 6): 'EnableIntr', ('intrman', 17): 'CpuSuspendIntr',
    ('intrman', 18): 'CpuResumeIntr', ('intrman', 23): 'QueryIntrContext', ('sifman', 7): 'sceSifSetDma',
    ('sifman', 8): 'sceSifDmaStat', ('sifcmd', 14): 'sceSifInitRpc', ('sifcmd', 15): 'sceSifBindRpc',
    ('sifcmd', 16): 'sceSifCallRpc', ('sifcmd', 17): 'sceSifRegisterRpc', ('sifcmd', 18): 'sceSifCheckStatRpc',
    ('sifcmd', 19): 'sceSifSetRpcQueue', ('sifcmd', 22): 'sceSifRpcLoop', ('sifcmd', 24): 'sceSifGetOtherData?',
    ('sifcmd', 25): 'sceSifRemoveRpc?', ('timrman', 4): 'AllocHardTimer', ('timrman', 20): 'SetTimerHandler',
    ('vblank', 5): 'RegisterVblankHandler?', ('stdio', 4): 'printf', ('sysclib', 12): 'memcpy', ('sysclib', 14): 'memset',
}

# Which source function handles each HLE library (case labels inside it are the handled ordinals).
HLE = {
    'sysmem': ('imports/iop_sysmem.cpp', 'dispatchImport'), 'loadcore': ('imports/iop_loadcore.cpp', 'dispatchImport'),
    'thbase': ('core/iop_kernel.cpp', 'dispatchThreadImport'), 'threadman': ('core/iop_kernel.cpp', 'dispatchThreadImport'),
    'thsemap': ('core/iop_kernel.cpp', 'dispatchSemaphoreImport'), 'thevent': ('core/iop_kernel.cpp', 'dispatchEventImport'),
    'intrman': ('imports/iop_intrman.cpp', 'dispatchImport'), 'sifman': ('services/iop_rpc.cpp', 'dispatchSifManImport'),
    'sifcmd': ('services/iop_rpc.cpp', 'dispatchSifCmdImport'), 'sysclib': ('imports/iop_sysclib.cpp', 'dispatchImport'),
    'stdio': ('imports/iop_stdio.cpp', 'dispatchImport'), 'ioman': ('imports/iop_ioman.cpp', 'dispatchImport'),
    'timrman': ('imports/iop_timrman.cpp', 'dispatchImport'), 'vblank': ('imports/iop_vblank.cpp', 'dispatchImport'),
    'cdvdman': ('imports/iop_cdvd.cpp', 'dispatchImport'), 'heaplib': ('imports/iop_heaplib.cpp', 'dispatchImport'),
    'dmacman': ('iop_emulator.cpp', 'dispatchDmacman'),
}


def handled_cases(path, func):
    s = open(os.path.join(src, path), encoding='utf-8', errors='replace').read()
    m = re.search(r'\b' + func + r'\s*\([^)]*\)\s*(const)?\s*\{', s)
    if not m:
        return None
    i, depth = m.end() - 1, 0
    j = i
    while j < len(s):
        if s[j] == '{':
            depth += 1
        elif s[j] == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    body = s[i:j]
    cases = {int(x, 0) for x in re.findall(r'case\s+(0x[0-9A-Fa-f]+|\d+)u?\s*:', body)}
    has_default_ok = bool(re.search(r'default\s*:\s*(?:[^;]*;\s*)*?return\s+true', body))
    return cases, has_default_ok


def imports_of(data):
    out = []
    i = 0
    while i + 20 <= len(data):
        if struct.unpack_from('<I', data, i)[0] == 0x41E00000:
            name = data[i + 12:i + 20].split(b'\0')[0].decode('latin1')
            j = i + 20
            while j + 8 <= len(data):
                a, b = struct.unpack_from('<II', data, j)
                if a == 0x03E00008 and (b >> 16) == 0x2400:
                    out.append((name, b & 0xFFFF))
                    j += 8
                else:
                    break
            i = j
        else:
            i += 4
    return out


def exports_of(data):
    libs = set()
    i = 0
    while i + 20 <= len(data):
        if struct.unpack_from('<I', data, i)[0] == 0x41C00000:
            libs.add(data[i + 12:i + 20].split(b'\0')[0].decode('latin1'))
        i += 4
    return libs


users = {}
exported = set()
for p in sorted(glob.glob(os.path.join(irx_dir, '*.IRX'))):
    data = open(p, 'rb').read()
    exported |= exports_of(data)
    for lib, o in imports_of(data):
        users.setdefault((lib, o), []).append(os.path.basename(p))
cache = {}
missing = []
print(f'{len(users)} distinct imports from {len(glob.glob(os.path.join(irx_dir, "*.IRX")))} modules; exported by the modules: {sorted(exported)}')
for (lib, o) in sorted(users):
    if lib in exported:
        state = 'module'
    elif lib in HLE:
        if lib not in cache:
            cache[lib] = handled_cases(*HLE[lib])
        res = cache[lib]
        if res is None:
            state = 'HLE?'
        else:
            cases, default_ok = res
            state = 'HLE' if o in cases else ('HLE-default' if default_ok else 'MISSING')
    else:
        state = 'MISSING-lib'
    if state not in ('module', 'HLE'):
        missing.append((lib, o, state))
    print(f'  {lib:9s}{o:4d} {NAMES.get((lib, o), ""):26s} {state:12s} {",".join(sorted(set(users[(lib, o)])))}')
print(f'\nnot handled: {len(missing)}')
for lib, o, st in missing:
    print(f'  {lib}:{o} {NAMES.get((lib, o), "")} {st} used by {",".join(sorted(set(users[(lib, o)])))}')
