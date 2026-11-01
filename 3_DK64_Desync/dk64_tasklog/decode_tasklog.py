#!/usr/bin/env python3
"""Decode a bar_tasklog SRAM image (the .srm the EverDrive writes, or the 96 KiB buffer cut out of an emulator
RAM dump) into a per-frame timeline of Beetle Adventure Racing's RSP/RDP/CPU work.

usage: decode_tasklog.py log.srm [--events] [--csv out.csv]
Entry: bits 31..16 = COP0 Count delta / 16 since the previous entry (46.875 MHz; 1 VI = 781250 ticks), 15..12 kind,
11..0 payload. kind 1 osSpTaskLoad (type<<8 | flags), 2 osSpTaskStartGo (SP go), 3 interrupt event (event<<8 | extra),
4 osSpTaskYield. Events: 4 SP (extra: bit0 halt, bit1 broke, bit2 sig0=yield requested, bit3 sig1=yielded, bit4 sig2=done),
6 AI, 7 VI (extra = line/2), 9 DP (extra = DPC_STATUS low byte), 5 SI, 8 PI, 2 CART, 3 COUNTER.
"""
import struct, sys

VI_TICKS = 781250.0
VERSION = 1
EVENTS = {0: 'SW1', 1: 'SW2', 2: 'CART', 3: 'COUNTER', 4: 'SP', 5: 'SI', 6: 'AI', 7: 'VI', 8: 'PI', 9: 'DP', 10: 'CPUBRK', 11: 'SPBRK', 12: 'FAULT', 13: 'THREAD', 14: 'PRENMI'}

def load(path):
    d = open(path, 'rb').read()
    if d[:4] in (b'BARL', b'DKTL'): words = struct.unpack('>%dI' % (len(d) // 4), d[:len(d) & ~3])
    elif d[:4] in (b'LRAB', b'LTKD'): words = struct.unpack('<%dI' % (len(d) // 4), d[:len(d) & ~3])
    elif d[2:4] + d[:2] in (b'BARL', b'DKTL'):  # halfword swapped
        words = [struct.unpack('>I', d[i+2:i+4] + d[i:i+2])[0] for i in range(0, len(d) & ~3, 4)]
    else:
        sys.exit('no BARL/DKTL header (first bytes %s)' % d[:8].hex())
    magic, start_vi, count0, version = words[:4]
    entries = []
    global VERSION; VERSION = version
    t = 0  # ticks since enable
    for w in words[4:]:
        if w == 0: break
        delta = (w >> 16) * 16
        kind = (w >> 12) & 0xF
        payload = w & 0xFFF
        t += delta
        entries.append((t, kind, payload))
    return start_vi, count0, entries

def main():
    args = sys.argv[1:]
    show_events = '--events' in args
    csv = None
    if '--csv' in args: csv = args[args.index('--csv') + 1]
    path = [a for a in args if not a.startswith('--') and a != csv][0]
    start_vi, count0, entries = load(path)
    print('# format v%d, start at VI #%d, %d entries, %.1f VI logged' % (VERSION, start_vi, len(entries), entries[-1][0] / VI_TICKS if entries else 0))

    # reconstruct: which task is running (last load type), gfx frames
    vi_n = start_vi
    cur_type = None          # type of the task last loaded
    cur_flags = 0
    frames = []              # dicts per gfx frame
    frame = None
    gfx_running = False
    last_vi_t = None
    pi_start = None
    for t, kind, p in entries:
        if kind == 1:
            cur_type = p >> 8; cur_flags = p & 0xFF
            if cur_type == 1 and not (cur_flags & 1):   # fresh gfx task (not a resume of a yielded one)
                if frame: frames.append(frame)
                frame = {'start_vi': vi_n, 'load': t, 'go': None, 'segments': [], 'yields': 0, 'done': None, 'dp': None, 'audio': 0, 'vi_at_done': None, 'pi_n': 0, 'pi_bytes': 0, 'pi_time': 0.0}
            if cur_type == 2 and frame: frame['audio'] += 1
        elif kind == 2:
            if VERSION >= 3: cur_type = 1
            if cur_type == 1 and frame:
                if frame['go'] is None: frame['go'] = t
                frame['segments'].append([t, None])
                gfx_running = True
            elif cur_type == 2:
                gfx_running = False
        elif kind == 4:
            if frame and cur_type == 1: frame['yields'] += 1
        elif kind == 6:                                     # v2 (version 5/6): PI DMA start, payload = bytes / 8
            pi_start = t
            if frame: frame['pi_n'] += 1; frame['pi_bytes'] += p * 8
        elif kind == 3:
            ev = p >> 8; extra = p & 0xFF
            if ev == 7:
                vi_n += 1; last_vi_t = t
            elif ev == 4 and frame:
                if VERSION >= 3:
                    cur_type = (extra >> 5) & 3
                    if cur_type == 2: frame['audio'] += 1
                    extra &= 0x1F
                if cur_type == 1 and frame['segments'] and frame['segments'][-1][1] is None:
                    frame['segments'][-1][1] = t
                    if extra & 0x10 and not (extra & 0x08):      # sig2 (task done), not yielded
                        frame['done'] = t; frame['vi_at_done'] = vi_n
                    elif not (extra & 0x08) and frame['done'] is None and not (extra & 0x04):
                        # halted without yield flags: treat as done
                        frame['done'] = t; frame['vi_at_done'] = vi_n
            elif ev == 9 and frame and frame['dp'] is None and frame['go'] is not None:
                frame['dp'] = t
            elif ev == 8 and frame and pi_start is not None:
                frame['pi_time'] += (t - pi_start) / VI_TICKS; pi_start = None
        if show_events:
            if kind == 3: s = 'EV %-7s %02x' % (EVENTS.get(p >> 8, str(p >> 8)), p & 0xFF)
            elif kind == 1: s = 'LOAD type %d flags %02x' % (p >> 8, p & 0xFF)
            elif kind == 2: s = 'GO   (%s)' % ('gfx' if cur_type == 1 else 'audio' if cur_type == 2 else cur_type)
            elif kind == 4: s = 'YIELD request'
            elif kind == 6: s = 'PI DMA start %d bytes' % (p * 8)
            else: s = 'CPU idle wait ended (kind %d)' % kind
            print('%10.4f VI  %s' % (t / VI_TICKS, s))
    if frame: frames.append(frame)

    print('# %d gfx frames' % len(frames))
    print('# frame  startVI  go->done[VI]  rsp_busy  segs  yields  dp-go   next_go-go  cpu_gap(next_go-done)  audio' + ('   pi_n  pi_bytes  pi_time[VI]' if VERSION >= 5 else ''))
    rows = []
    for i, f in enumerate(frames):
        nxt = frames[i + 1]['go'] if i + 1 < len(frames) and frames[i + 1]['go'] else None
        go = f['go']
        if go is None: continue
        busy = sum((b - a) for a, b in f['segments'] if b is not None) / VI_TICKS
        done = (f['done'] - go) / VI_TICKS if f['done'] else float('nan')
        dp = (f['dp'] - go) / VI_TICKS if f['dp'] else float('nan')
        period = (nxt - go) / VI_TICKS if nxt else float('nan')
        gap = (nxt - f['done']) / VI_TICKS if (nxt and f['done']) else float('nan')
        rows.append((i, f['start_vi'], done, busy, len(f['segments']), f['yields'], dp, period, gap, f['audio']))
        line = '%6d  %7d  %11.3f  %8.3f  %4d  %6d  %6.3f  %10.3f  %21.3f  %5d' % rows[-1]
        if VERSION >= 5: line += '  %5d  %8d  %11.3f' % (f['pi_n'], f['pi_bytes'], f['pi_time'])
        print(line)
    if csv:
        with open(csv, 'w') as fh:
            fh.write('frame,start_vi,go_to_done_vi,rsp_busy_vi,segments,yields,dp_vi,period_vi,cpu_gap_vi,audio_tasks\n')  # (PI columns are not in the csv)
            for r in rows: fh.write(','.join(str(x) for x in r) + '\n')
    # summary per 30 frames
    import math
    print('# per 30 frames: mean period / mean go->done / mean cpu gap [VI]')
    for b in range(0, len(rows), 30):
        blk = [r for r in rows[b:b + 30] if not math.isnan(r[7])]
        if not blk: continue
        print('  frames %3d-%3d: %.3f / %.3f / %.3f' % (b, b + len(blk) - 1, sum(r[7] for r in blk) / len(blk), sum(r[2] for r in blk if not math.isnan(r[2])) / max(1, len(blk)), sum(r[8] for r in blk if not math.isnan(r[8])) / max(1, len(blk))))

if __name__ == '__main__':
    main()
