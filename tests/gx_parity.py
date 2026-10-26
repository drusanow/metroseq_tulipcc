#!/usr/bin/env python3
"""GinTonix: compare the C pattern player (gx_seq.c) with the Python's own
layout() (reference/gintonix/gintonix.py, run without AMY) on random
patterns: rates, swing, odd lengths, fills, accents, choke, mutes.
   python3 gx_parity.py      (needs build/gx_layout)"""
import os
import random
import re
import subprocess
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'reference', 'gintonix'))
sys.modules['amy'] = types.ModuleType('amy')   # the Python only needs AMY to make sound
import gintonix as G  # noqa: E402

UNITS_PER_TICK = 140
HIT = re.compile(r'i(\d+)v0n60l([\d.]+)Z')


def python_hits(eng, pi):
    lay = eng.layout(pi)
    out = []
    for tick, wire in lay['des'].items():
        for syn, vel in HIT.findall(wire):
            out.append((tick, int(syn) - G.SYNTH0, float(vel)))
    # layout() covers one loop: two passes when the length is odd and swung
    reps = lay['N'] // lay['n']
    if reps == 1:
        out += [(t + lay['P'], c, v) for t, c, v in out]
    return sorted(out), lay['P'] * (2 // reps)


def run(seed):
    rnd = random.Random(seed)
    eng = G.GinTonic(send_raw=lambda s: None, ticks_fn=lambda: 0)
    n = rnd.choice([16, 16, 12, 8, 15, 7, 5, 3, 1])
    pat = G.random_pattern(n, rnd.random)
    for c in range(G.NCH):   # more fills than random_pattern gives
        for i in range(n):
            if pat['trig'][c] & (1 << i) and rnd.random() < 0.15:
                pat['fill'][c] |= 1 << i
    eng.pats[0] = pat
    eng.rate = rnd.randrange(len(G.RATES))
    eng.swing = rnd.choice([0.0, 0.0, 0.25, 0.5, 0.8, 1.0, rnd.random()])
    eng.fill_rate = rnd.randrange(len(G.FILL_RATES))
    choke = 0
    for c in range(G.NCH):
        if rnd.random() < 0.3:
            eng.a[c]['choke'] = eng.b[c]['choke'] = 1
            choke |= 1 << c
        if rnd.random() < 0.1:
            eng.mute[c] = True
    eng._eff = [None] * G.NCH
    eng._hcache = {}
    mute = sum(1 << c for c in range(G.NCH) if eng.mute[c])
    py, span_ticks = python_hits(eng, 0)
    inp = '%d %d %r %d %d %d\n' % (n, eng.rate, eng.swing, eng.fill_rate, choke, mute)
    for c in range(G.NCH):
        inp += '%d %d %d\n' % (pat['trig'][c], pat['acc'][c], pat['fill'][c])
    res = subprocess.run([os.path.join(HERE, 'build', 'gx_tool'), 'layout'], input=inp, text=True,
                         capture_output=True, check=True)
    cc = []
    for line in res.stdout.split():
        pass
    vals = res.stdout.split()
    for i in range(0, len(vals), 3):
        cc.append((int(vals[i]), int(vals[i + 1]), float(vals[i + 2])))
    cc.sort()
    if eng._warned:
        return 'skip'   # the Python ran out of AMY sequencer tags and dropped hits
    # the Python rounds the swing and each fill hit to whole 48 PPQ ticks
    # (each up to half a tick); C is exact
    # When a fill has more hits than its (swung) step has ticks, the Python
    # merges hits that round onto the same tick; C plays them all. Then the
    # Python's hits must all be there, and the rest only from fills.
    L = G.RATES[eng.rate][1]
    merged = G.FILL_RATES[eng.fill_rate] > L - int(eng.swing * 0.5 * L + 0.5)
    if len(py) != len(cc) and not (merged and len(cc) > len(py)):
        return 'count %d vs %d' % (len(py), len(cc))
    used = [False] * len(cc)
    for t, ch, vel in py:
        best = None
        for j, (pos, c2, v2) in enumerate(cc):
            d = abs(pos - t * UNITS_PER_TICK)
            if not used[j] and c2 == ch and d <= UNITS_PER_TICK + 1 \
                    and abs(max(0.01, v2) - vel) < 0.002 and (best is None or d < best[0]):
                best = (d, j)
        ok = best is not None
        if ok:
            used[best[1]] = True
        if not ok:
            return 'python hit tick %d ch %d vel %.4f not in C output' % (t, ch, vel)
    return None


def check_fmt():
    """every knob's display text, for random patches"""
    rnd = random.Random(1)
    rows, exp = [], []
    for _ in range(400):
        p = G.default_patch()
        for k in G.KNOB_KEYS:
            p[k] = rnd.choice([0.0, 1.0, 0.5, 0.004, 0.002, rnd.random(), rnd.random()])
        for k, _, opts, _ in G.SWITCHES:
            p[k] = rnd.choice([v for v, _ in opts])
        rows.append(' '.join(repr(p[k]) for k in G.KNOB_KEYS) + ' ' +
                    ' '.join(str(p[k]) for k in G.SWITCH_KEYS))
        exp.append([G.fmt(k, p) for k in G.KNOB_KEYS])
    out = subprocess.run([os.path.join(HERE, 'build', 'gx_tool'), 'fmt'], input='\n'.join(rows),
                         text=True, capture_output=True, check=True).stdout.strip().split('\n')
    bad = 0
    for r, (e, line) in enumerate(zip(exp, out)):
        got = line.split('\t')
        for k, a, b in zip(G.KNOB_KEYS, e, got):
            if a != b:
                # float32 vs float64 can round a value sitting on .5 the other way
                try:
                    if abs(float(a.rstrip('dBstHzm%k')) - float(b.rstrip('dBstHzm%k'))) <= 0.11:
                        continue
                except ValueError:
                    pass
                bad += 1
                if bad <= 5:
                    print('fmt %s: python %r, C %r' % (k, a, b))
    print('knob display text vs Python fmt(): %d patches x %d knobs, %d differ%s' %
          (len(exp), len(G.KNOB_KEYS), bad, '  <-- FAIL' if bad else ''))
    return bad


def close(a, b, path=''):
    if isinstance(a, dict):
        return all(close(a[k], b.get(k), path + '.' + k) for k in a if k not in ('est', 'n_stereo'))
    if isinstance(a, list):
        return len(a) == len(b) and all(close(x, y, path) for x, y in zip(a, b))
    if isinstance(a, bool) or isinstance(b, bool):
        return bool(a) == bool(b)
    if isinstance(a, (int, float)):
        return b is not None and abs(a - b) < 2e-4
    return a == b


def check_kits():
    """factory kits: kit_state() vs the generated C tables"""
    bad = 0
    drums = {n: d for n, d in G.DRUMS}
    for i, kit in enumerate(G.KITS):
        py = G.kit_state(kit, drums)
        py['pats'] = [G.pat_copy(p) for p in py['pats']]
        out = subprocess.run([os.path.join(HERE, 'build', 'gx_tool'), 'kit', str(i)], text=True,
                             capture_output=True, check=True).stdout
        import json
        c = json.loads(out)
        if not close(py, c):
            bad += 1
            print('kit %d (%s) differs' % (i, kit[0]))
    print('factory kits vs Python kit_state(): %d/%d identical%s' %
          (len(G.KITS) - bad, len(G.KITS), '  <-- FAIL' if bad else ''))
    return bad


def check_microtonic():
    """converted Microtonic kits (kits/*.json) vs mt_kit_state()"""
    import json
    rnd = random.Random(5)
    bad = 0
    for trial in range(60):
        drums = []
        for c in range(8):
            d = {'Name': 'D%d' % c, 'OscWave': rnd.choice(['Sine', 'Triangle', 'Saw']),
                 'OscFreq': rnd.uniform(20, 15000), 'ModMode': rnd.choice(['Decay', 'Sine', 'Noise']),
                 'ModAmt': rnd.uniform(-96, 96), 'ModRate': rnd.choice([None, 0, rnd.uniform(1, 20000)]),
                 'OscAtk': rnd.uniform(0, 3000), 'OscDcy': rnd.uniform(5, 9000),
                 'NFilMod': rnd.choice(['LP', 'BP', 'HP']), 'NFilFrq': rnd.uniform(20, 20000),
                 'NFilQ': rnd.uniform(0.1, 9000), 'NEnvMod': rnd.choice(['Exp', 'Linear', 'Mod']),
                 'NEnvAtk': rnd.uniform(0, 1000), 'NEnvDcy': rnd.uniform(10, 5000),
                 'Mix': rnd.uniform(0, 100), 'EQFreq': rnd.uniform(20, 20000),
                 'EQGain': rnd.uniform(-40, 40), 'DistAmt': rnd.uniform(0, 100),
                 'Level': rnd.choice([-60, rnd.uniform(-30, 10)]), 'Pan': rnd.uniform(-100, 100),
                 'OscVel': rnd.uniform(0, 200), 'NVel': rnd.uniform(0, 200),
                 'ModVel': rnd.uniform(0, 200), 'Choke': rnd.choice(['On', 'Off'])}
            for k in list(d):
                if k != 'Name' and rnd.random() < 0.05:
                    del d[k]   # missing keys take the defaults
            drums.append(d)
        pats = []
        for i in range(12):
            pats.append({'len': rnd.randint(1, 16), 'chain': rnd.random() < 0.3,
                         't': [rnd.randrange(65536) for _ in range(8)],
                         'a': [rnd.randrange(65536) for _ in range(8)],
                         'f': [rnd.randrange(65536) for _ in range(8)]})
        k = {'drums': drums, 'pats': pats, 'rate': rnd.choice(['1/8', '1/16', '1/32', '1/8T', 'x']),
             'fill': rnd.uniform(0, 10), 'master': rnd.uniform(-20, 6), 'trim_db': rnd.uniform(-6, 6),
             'pattern': rnd.choice(['a', 'C', 'l', 'zz']), 'swing': rnd.uniform(0, 100),
             'tempo': rnd.uniform(10, 400)}
        py = G.mt_kit_state(k)
        py['pats'] = [G.pat_copy(p) for p in py['pats']]
        out = subprocess.run([os.path.join(HERE, 'build', 'gx_tool'), 'mt'], input=json.dumps(k),
                             text=True, capture_output=True, check=True).stdout
        c = json.loads(out)
        if not close(py, c):
            bad += 1
            if bad <= 3:
                for key in py:
                    if not close(py[key], c.get(key)):
                        print('microtonic kit %d: %s differs' % (trial, key))
    print('Microtonic kit conversion vs Python mt_kit_state(): %d/60 identical%s' %
          (60 - bad, '  <-- FAIL' if bad else ''))
    return bad


def main():
    bad = skipped = 0
    total = 300
    for seed in range(total):
        err = run(seed)
        if err == 'skip':
            skipped += 1
            continue
        if err:
            bad += 1
            if bad <= 5:
                print('seed %d: %s' % (seed, err))
    print('gintonix pattern player vs Python layout(): %d/%d random patterns match%s%s' %
          (total - bad - skipped, total - skipped, '' if not bad else '  <-- FAIL',
           ' (%d skipped: too many fill hits for the Python\'s AMY sequencer)' % skipped
           if skipped else ''))
    bad += check_fmt() + check_kits() + check_microtonic()
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
