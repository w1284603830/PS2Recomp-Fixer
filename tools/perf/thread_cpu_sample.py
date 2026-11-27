"""Sample per-thread CPU use of a running process by thread description (Windows 10 1607+).

  thread_cpu_sample.py <pid> <seconds> <out.json> [--interval 1.0]

Reads each thread's kernel+user time and its SetThreadDescription name (GameThread, GS worker, IOP, audio, ...)
once per interval and writes per-interval CPU % per named thread plus a summary (mean / p95 / max). Read-only:
it only opens threads with THREAD_QUERY_LIMITED_INFORMATION.
"""
import ctypes
import json
import sys
import time
from ctypes import wintypes

TH32CS_SNAPTHREAD = 0x00000004
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ThreadID', wintypes.DWORD),
                ('th32OwnerProcessID', wintypes.DWORD), ('tpBasePri', wintypes.LONG), ('tpDeltaPri', wintypes.LONG),
                ('dwFlags', wintypes.DWORD)]


kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.OpenThread.restype = wintypes.HANDLE
kernel32.GetThreadTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
try:
    GetThreadDescription = kernel32.GetThreadDescription
    GetThreadDescription.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
except AttributeError:
    GetThreadDescription = None


def threads(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    out = []
    ok = kernel32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            out.append(entry.th32ThreadID)
        ok = kernel32.Thread32Next(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return out


def sample(pid):
    result = {}
    for tid in threads(pid):
        h = kernel32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
        if not h:
            continue
        times = [wintypes.FILETIME() for _ in range(4)]
        name = ''
        if kernel32.GetThreadTimes(h, *(ctypes.byref(t) for t in times)):
            cpu = sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in times[2:]) / 1e7
            if GetThreadDescription:
                p = ctypes.c_wchar_p()
                if GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value:
                    name = p.value
            result[tid] = (name or f'tid{tid}', cpu)
        kernel32.CloseHandle(h)
    return result


def main():
    pid, seconds, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    interval = float(sys.argv[sys.argv.index('--interval') + 1]) if '--interval' in sys.argv else 1.0
    prev, prev_t = sample(pid), time.perf_counter()
    rows = []
    end = prev_t + seconds
    while time.perf_counter() < end:
        time.sleep(interval)
        cur, now = sample(pid), time.perf_counter()
        if not cur:
            break
        dt = now - prev_t
        row = {}
        for tid, (name, cpu) in cur.items():
            if tid in prev:
                pct = 100.0 * (cpu - prev[tid][1]) / dt
                if pct >= 0.5:
                    row[f'{name}#{tid}'] = round(pct, 1)
        rows.append({'t': round(now - (end - seconds), 1), 'cpu': row})
        prev, prev_t = cur, now
    summary = {}
    for r in rows:
        for k, v in r['cpu'].items():
            summary.setdefault(k, []).append(v)
    stats = {k: {'mean': round(sum(v) / len(rows), 1), 'p95': sorted(v)[int(0.95 * (len(v) - 1))], 'max': max(v),
                 'samples': len(v)} for k, v in summary.items()}
    json.dump({'pid': pid, 'interval': interval, 'rows': rows,
               'summary': dict(sorted(stats.items(), key=lambda kv: -kv[1]['mean']))},
              open(out, 'w', encoding='utf-8'), indent=1)
    for k, v in list(sorted(stats.items(), key=lambda kv: -kv[1]['mean']))[:12]:
        print(f'{k:40s} mean {v["mean"]:6.1f}%  p95 {v["p95"]:6.1f}%  max {v["max"]:6.1f}%')


if __name__ == '__main__':
    main()
