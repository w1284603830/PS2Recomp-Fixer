"""Sampling profiler for one named thread of a running process (Windows x64), symbolized with the linker map.

  thread_profile.py <pid> <thread-name> <seconds> <exe.map> <out.json> [--hz 1000] [--map module.dll=<module.map>]
                    [--pdb module.dll=<dll on disk>]

--map symbolizes samples in that DLL with its MSVC /MAP file (preferred load address from the map header), e.g.
--map pcsx2gs.dll=<build>/pcsx2gs.map for the GS thread ("dod GS"); prefer it: the system
dbghelp may not read a newer toolset's PDB (it then reports exports only).

--pdb symbolizes samples in that module with dbghelp and the DLL's PDB (found through the path in its debug directory or
next to the DLL), e.g. --pdb pcsx2gs.dll=<build>/pcsx2gs.dll for the GS thread.

Suspends the thread briefly at about <hz>, reads RIP (GetThreadContext CONTEXT_CONTROL), resumes it. Addresses in
the EXE are mapped to the nearest preceding public symbol from the MSVC /MAP file; other loaded modules are counted
per module; anything outside every module is reported as JIT/anonymous code (the microVU recompiler output).
Read-only apart from the suspend/resume; diagnostics only.
"""
import bisect
import collections
import ctypes
import json
import re
import sys
import time
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040 | 0x0800  # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION | QUERY_LIMITED
PROCESS_ACCESS = 0x0400 | 0x0010  # QUERY_INFORMATION | VM_READ
CONTEXT_CONTROL = 0x00100001
k32.OpenThread.restype = wintypes.HANDLE
k32.OpenProcess.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.SuspendThread.argtypes = [wintypes.HANDLE]
k32.ResumeThread.argtypes = [wintypes.HANDLE]
k32.GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
GetThreadDescription = k32.GetThreadDescription
GetThreadDescription.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ThreadID', wintypes.DWORD),
                ('th32OwnerProcessID', wintypes.DWORD), ('tpBasePri', wintypes.LONG), ('tpDeltaPri', wintypes.LONG),
                ('dwFlags', wintypes.DWORD)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [('lpBaseOfDll', ctypes.c_void_p), ('SizeOfImage', wintypes.DWORD), ('EntryPoint', ctypes.c_void_p)]


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wintypes.ULONG), ('TypeIndex', wintypes.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wintypes.ULONG), ('Size', wintypes.ULONG), ('ModBase', ctypes.c_uint64),
                ('Flags', wintypes.ULONG), ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64),
                ('Register', wintypes.ULONG), ('Scope', wintypes.ULONG), ('Tag', wintypes.ULONG),
                ('NameLen', wintypes.ULONG), ('MaxNameLen', wintypes.ULONG), ('Name', ctypes.c_wchar * 512)]


class PdbSymbols:
    """Offline symbolization of one DLL with dbghelp (its PDB); addresses are offsets from the module base."""
    BASE = 0x10000000

    def __init__(self, dll_path):
        import os
        self.dbghelp = ctypes.WinDLL('dbghelp')
        self.proc = wintypes.HANDLE(0x5A5A)
        self.dbghelp.SymSetOptions(0x2 | 0x4)  # SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS
        search = os.path.dirname(os.path.abspath(dll_path))
        self.dbghelp.SymInitializeW(self.proc, ctypes.c_wchar_p(search), False)
        self.dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
        self.dbghelp.SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                                  ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        self.dbghelp.SymFromAddrW.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                              ctypes.POINTER(SYMBOL_INFOW)]
        if not self.dbghelp.SymLoadModuleExW(self.proc, None, os.path.abspath(dll_path), None, self.BASE, 0, None, 0):
            raise SystemExit('dbghelp could not load ' + dll_path)
        self.cache = {}

    def name(self, offset):
        if offset in self.cache:
            return self.cache[offset]
        info = SYMBOL_INFOW()
        info.SizeOfStruct = 88  # sizeof(SYMBOL_INFOW) with its one-character Name, as dbghelp checks
        info.MaxNameLen = 511
        disp = ctypes.c_uint64()
        ok = self.dbghelp.SymFromAddrW(self.proc, self.BASE + offset, ctypes.byref(disp), ctypes.byref(info))
        result = info.Name if ok else '?+0x%X' % offset
        self.cache[offset] = result
        return result


def find_thread(pid, name):
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Thread32First(snap, ctypes.byref(e))
    found = None
    while ok:
        if e.th32OwnerProcessID == pid:
            h = k32.OpenThread(0x0800, False, e.th32ThreadID)
            if h:
                p = ctypes.c_wchar_p()
                if GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value == name:
                    found = e.th32ThreadID
                k32.CloseHandle(h)
        if found:
            break
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return found


def modules(pid):
    hp = k32.OpenProcess(PROCESS_ACCESS, False, pid)
    arr = (ctypes.c_void_p * 1024)()
    needed = wintypes.DWORD()
    psapi.EnumProcessModulesEx(hp, arr, ctypes.sizeof(arr), ctypes.byref(needed), 3)
    out = []
    for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
        info = MODULEINFO()
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleInformation(hp, ctypes.c_void_p(arr[i]), ctypes.byref(info), ctypes.sizeof(info))
        psapi.GetModuleBaseNameW(hp, ctypes.c_void_p(arr[i]), name, 260)
        out.append((info.lpBaseOfDll or 0, (info.lpBaseOfDll or 0) + info.SizeOfImage, name.value))
    k32.CloseHandle(hp)
    return sorted(out)


def load_map(path):
    syms = []
    pat = re.compile(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s')
    for line in open(path, encoding='latin-1'):
        m = pat.match(line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


class MapSymbols:
    """A DLL's MSVC /MAP file: offsets from the module base to the nearest preceding public symbol."""

    def __init__(self, path):
        self.base = 0x180000000
        for line in open(path, encoding='latin-1'):
            m = re.search(r'Preferred load address is ([0-9a-fA-F]+)', line)
            if m:
                self.base = int(m.group(1), 16)
                break
        self.addrs, self.names = load_map(path)

    def name(self, offset):
        i = bisect.bisect_right(self.addrs, self.base + offset) - 1
        return self.names[i] if i >= 0 else '?+0x%X' % offset


def category(sym):
    s = sym.lower()
    if 'kzvu' in s or 'microvu' in s or 'mvu' in s or 'vu1' in s:
        return 'vu (kzvu)'
    if s.startswith('?sub_') or s.startswith('sub_') or 'chunk_' in s or 'ultraman_rx' in s or '?entry_' in s:
        return 'guest EE code (AOT)'
    if 'ee_interp' in s or 'interpret' in s:
        return 'EE interpreter'
    if 'ipu' in s:
        return 'IPU'
    if 'gif' in s or 'vif' in s or 'gs' in s or 'dod_pcsx2' in s or 'pcsx2' in s:
        return 'GIF/VIF/GS bridge'
    if 'dma' in s or 'memory' in s or 'read' in s or 'write' in s or 'load' in s or 'store' in s:
        return 'memory/DMA'
    if 'scheduler' in s or 'dispatch' in s or 'lookup' in s or 'branch' in s:
        return 'dispatch/scheduler'
    return 'runtime other'


def main():
    pid, tname, seconds, mapfile, out = int(sys.argv[1]), sys.argv[2], float(sys.argv[3]), sys.argv[4], sys.argv[5]
    hz = float(sys.argv[sys.argv.index('--hz') + 1]) if '--hz' in sys.argv else 1000.0
    pdbs = {}
    for i, a in enumerate(sys.argv):
        if a == '--pdb':
            module, _, dll = sys.argv[i + 1].partition('=')
            pdbs[module.lower()] = PdbSymbols(dll)
        elif a == '--map':
            module, _, mp = sys.argv[i + 1].partition('=')
            pdbs[module.lower()] = MapSymbols(mp)
    tid = find_thread(pid, tname)
    if not tid:
        raise SystemExit('thread not found: ' + tname)
    mods = modules(pid)
    exe = next(m for m in mods if m[2].lower().endswith('.exe'))
    addrs, names = load_map(mapfile)
    preferred = 0x140000000
    h = k32.OpenThread(THREAD_ACCESS, False, tid)
    buf = ctypes.create_string_buffer(1232 + 16)
    ctx = (ctypes.addressof(buf) + 15) & ~15
    ctypes.c_uint32.from_address(ctx + 0x30).value = CONTEXT_CONTROL
    rips = []
    end = time.perf_counter() + seconds
    period = 1.0 / hz
    while time.perf_counter() < end:
        if k32.SuspendThread(h) != 0xFFFFFFFF:
            ctypes.c_uint32.from_address(ctx + 0x30).value = CONTEXT_CONTROL
            if k32.GetThreadContext(h, ctypes.c_void_p(ctx)):
                rips.append(ctypes.c_uint64.from_address(ctx + 0xF8).value)
            k32.ResumeThread(h)
        time.sleep(period)
    k32.CloseHandle(h)
    funcs, cats, modc = collections.Counter(), collections.Counter(), collections.Counter()
    starts = [m[0] for m in mods]
    for rip in rips:
        if exe[0] <= rip < exe[1]:
            va = rip - exe[0] + preferred
            i = bisect.bisect_right(addrs, va) - 1
            sym = names[i] if i >= 0 else '?'
            funcs[sym] += 1
            cats[category(sym)] += 1
            modc[exe[2]] += 1
            continue
        i = bisect.bisect_right(starts, rip) - 1
        if i >= 0 and mods[i][0] <= rip < mods[i][1]:
            modc[mods[i][2]] += 1
            pdb = pdbs.get(mods[i][2].lower())
            if pdb:
                sym = pdb.name(rip - mods[i][0])
                funcs[mods[i][2] + '!' + sym] += 1
                cls = sym.split('::')[0] if '::' in sym else sym
                cats[mods[i][2] + ' ' + cls] += 1
            else:
                cats['module ' + mods[i][2]] += 1
        else:
            modc['<JIT/anonymous>'] += 1
            cats['JIT (microVU output)'] += 1
    n = max(1, len(rips))
    report = {'pid': pid, 'thread': tname, 'samples': len(rips), 'seconds': seconds,
              'categories': {k: round(100.0 * v / n, 1) for k, v in cats.most_common()},
              'modules': {k: round(100.0 * v / n, 1) for k, v in modc.most_common(20)},
              'functions': [(k, round(100.0 * v / n, 2)) for k, v in funcs.most_common(60)]}
    json.dump(report, open(out, 'w', encoding='utf-8'), indent=1)
    print('samples', len(rips))
    for k, v in report['categories'].items():
        print(f'  {v:5.1f}%  {k}')
    print('top functions:')
    for k, v in report['functions'][:30]:
        print(f'  {v:5.2f}%  {k[:110]}')


if __name__ == '__main__':
    main()
