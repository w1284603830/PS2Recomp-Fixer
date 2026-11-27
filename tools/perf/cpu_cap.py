"""Weak-host simulation for any running port: cap a process to N logical CPUs of this machine on a schedule.

  cpu_cap.py <pid> --cpus 2,4 --step 140:0.7 --step 168:0.5 --step 224:0 [--log <stderr.log>] [--out result.json]

Times are seconds after this tool starts (start it together with the game). At each step the process is placed in a
Job Object with a hard CPU-rate cap of C logical CPUs (C/<logical CPUs> of the machine) and affinity to --cpus;
C = 0 lifts the cap. Start the cap only after the scripted route reaches the content (scripted input usually uses host
time and would desynchronise if the boot ran slowly). With --log, lines containing "fps" are time-stamped and tagged
with the cap in force (point it at the port's stderr log with an fps counter enabled).

A duty-cycle cap approximates a slower CPU; it does not model one. Compare builds side by side (simultaneous runs on
disjoint --cpus sets) and read the game's own frame rate, not only presented fields. Background load on the machine
moves the results by several fps: repeat the baseline in the same batch.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateJobObjectW.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE


class BASIC_LIMIT(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                ('LimitFlags', wt.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wt.DWORD),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', wt.DWORD), ('SchedulingClass', wt.DWORD)]


class CPU_RATE(ctypes.Structure):
    _fields_ = [('ControlFlags', wt.DWORD), ('CpuRate', wt.DWORD)]


def set_cap(job, cpus, mask, logical):
    basic, rate = BASIC_LIMIT(), CPU_RATE()
    if cpus > 0:
        basic.LimitFlags = 0x10  # JOB_OBJECT_LIMIT_AFFINITY
        basic.Affinity = mask
        rate.ControlFlags = 0x1 | 0x4  # ENABLE | HARD_CAP
        rate.CpuRate = max(1, min(10000, round(cpus / logical * 10000)))
    k32.SetInformationJobObject(job, 2, ctypes.byref(basic), ctypes.sizeof(basic))
    k32.SetInformationJobObject(job, 15, ctypes.byref(rate), ctypes.sizeof(rate))
    return rate.CpuRate


def main():
    p = argparse.ArgumentParser()
    p.add_argument('pid', type=int)
    p.add_argument('--cpus', default='2,4')
    p.add_argument('--step', action='append', default=[], help='T:C (seconds : logical CPUs, 0 = no cap)')
    p.add_argument('--log', default='')
    p.add_argument('--out', default='cpu_cap.json')
    a = p.parse_args()
    logical = os.cpu_count() or 1
    mask = sum(1 << int(c) for c in a.cpus.split(','))
    steps = sorted((float(t), float(c)) for t, c in (s.split(':') for s in a.step))
    job = k32.CreateJobObjectW(None, None)
    proc = k32.OpenProcess(0x0100 | 0x0001 | 0x00100000, False, a.pid)  # SET_QUOTA | TERMINATE | SYNCHRONIZE
    if not proc or not k32.AssignProcessToJobObject(job, proc):
        raise SystemExit('cannot attach to pid %d (error %d)' % (a.pid, ctypes.get_last_error()))
    start, cap, offset = time.monotonic(), 0.0, 0
    record = {'pid': a.pid, 'cpus': a.cpus, 'logical': logical, 'applied': [], 'lines': []}
    while k32.WaitForSingleObject(proc, 500) == 0x102:  # WAIT_TIMEOUT: still running
        now = time.monotonic() - start
        while steps and steps[0][0] <= now:
            _, cap = steps.pop(0)
            record['applied'].append({'t': round(now, 2), 'cpus': cap, 'rate': set_cap(job, cap, mask, logical)})
        if a.log and os.path.exists(a.log):
            with open(a.log, 'rb') as f:
                f.seek(offset)
                chunk = f.read()
            cut = chunk.rfind(b'\n') + 1
            offset += cut
            for line in chunk[:cut].decode('utf-8', 'replace').splitlines():
                if 'fps' in line:
                    record['lines'].append({'t': round(now, 1), 'cpus': cap, 'line': line.strip()})
    json.dump(record, open(a.out, 'w', encoding='utf-8'), indent=1)
    print(a.out, len(record['lines']), 'fps lines')


if __name__ == '__main__':
    main()
