# gintonix.py -- GinTonix (GinTonic with the touch-friendly UI): a Sonic Charge Microtonic style drum machine for the
# Tulip Creative Computer (AMY synth engine).
#
# 8 drum channels, each one AMY synth with one voice:
#
#   HEAD (SILENT: distortion -> EQ, pan)  <- chain sum of:
#     TONE  : sine / triangle / saw, pitch modulation (decaying / sine / random),
#             exponential attack + decay           (sine mod > 60 Hz: 2-op FM)
#     NOISE : white noise -> LP / BP / HP filter, exponential / linear / clap EG
#   (no extra stereo chains: the channel sits where its PAN puts it - light on AMY)
#   channel LEVEL = AMY synth_level (after distortion, so LEVEL does not drive it)
#
# Pattern engine ("AMY loop", as in DAFQXTMOD): while playing, one pass of the
# current pattern lives in AMY's own sequencer as repeating events (one event
# per tick that has hits; period = one pass).  AMY fires them on its own task,
# so the rhythm never depends on Python: GC, screen redraws, preset loads, file
# writes cannot make it stumble.  Python only rewrites the ticks an edit really
# changed, and switches to the next pattern (chain / pattern select) at the end
# of a pass.  If Python is late, the pattern simply repeats once more.
#
# Install: copy the 'gintonix' folder to Tulip (/user/gintonix/gintonix.py), then
#   cd('/user')  ;  run('gintonix')
# The Microtonic kits are read from gintonix/kits, or else from gintonic/kits.

import amy
import math
import random

# ----------------------------------------------------------------------------
# User config
# ----------------------------------------------------------------------------
DIAG = False                 # True: print a status line every 2 s to the REPL (freeze hunting)
SHOW_CPU = False             # True: AMY load (CPU %) in the header
MUTE_OTHER_SYNTHS = True     # silence Tulip's own MIDI synths (Juno, drums) while GinTonic runs
SYNTH0 = 40                  # AMY synths 40..47 = drum channels 1..8 (AMY allows < 64)
MIDI_OUT_SYNTH = 48          # voiceless AMY synth that sends the pattern out as MIDI notes
TAG_FIRST = 32               # AMY sequencer tags TAG_FIRST..TAG_LAST belong to this app
TAG_LAST = 127               # (Tulip's AMYSequences count up from 0, DAFQXTMOD uses 128..255)
P_GUARD = 2                  # ticks: an event this close to firing is not rewritten
START_MS = 60                # PLAY: the first step sounds at a step boundary at least this far ahead
TPC = 2                      # AMY ticks per MIDI clock (48 PPQ / 24)

NCH = 8
NSTEP = 16
NPAT = 12
PAT_NAMES = 'ABCDEFGHIJKL'

# ----------------------------------------------------------------------------
# AMY constants / wire helpers
# ----------------------------------------------------------------------------
SILENT = getattr(amy, 'SILENT', 20)
W_SINE, W_SAW, W_TRI, W_NOISE, W_ALGO = 0, 2, 4, 5, 8
F_NONE, F_LPF, F_BPF, F_HPF, F_NOTCH = 0, 1, 2, 3, 5

# oscillators of one channel voice
O_HEAD, O_TONE, O_NOISE, O_LFO, O_OPM, O_OPC = range(6)
OSCS_PER_VOICE = 6     # one mono chain per channel (STEREO = PAN only, see README)

try:
    _KW = amy._KW_MAP
except Exception:
    _KW = {}
HAS_DIST = 'dist_drive' in _KW
HAS_LEVEL = 'synth_level' in _KW

# Calibration (measured in AMY): one osc at amp 1 = internal full scale (the
# distortion clips at +-1); the output stage is ~26 dB below internal.
G_TONE = 0.55          # tone amp into the head at mix 100/0
G_ALGO = 2.2           # AMY's FM (ALGO) output is ~12 dB quieter
G_NOISE = 0.6          # noise amp at mix 0/100
G_HEAD = 1.0
POST_GAIN = 9.0        # synth_level at LEVEL 0 dB, MASTER 0 dB
FM_IDX = 2.5           # AMY: FM index = 2.5 x modulator amp
LFO_MAX_HZ = 60.0      # above this, sine pitch mod is done with FM (AMY reads
                       # modulators only once per block, ~172 Hz)
RND_PY_MAX_HZ = 25.0   # random pitch mod: per hit (0 Hz), Python S&H (< this), AMY noise mod

# CPU saver (ECO): long tails are cut after ECO_CAP_MS[level] ms (and fade out
# within ECO_REL_MS).  Chosen automatically from an estimate of the kit's AMY load
# (and AMY's own load meter, if the firmware has it).
ECO_CAP_MS = (None, 1500.0, 900.0, 500.0)
ECO_REL_MS = 120.0
ECO_EST = (700.0, 950.0, 1200.0)  # est_load() -> ECO 1 / 2 / 3 (mono engine: factory < 400, Microtonic kits 280..650)

RATES = (('1/8', 24), ('1/8T', 16), ('1/16', 12), ('1/16T', 8), ('1/32', 6))   # 48 PPQ
FILL_RATES = (2, 3, 4, 5, 6, 7, 8)

try:
    from tulip import ticks_ms
except ImportError:
    import time

    def ticks_ms():
        return int(time.time() * 1000)


def clamp(x, lo=0.0, hi=1.0):
    return lo if x < lo else (hi if x > hi else x)


def bip(k):
    return (k - 0.5) * 2.0


def num(x):
    """compact number for AMY wire strings"""
    if x == int(x) and -1e6 < x < 1e6:
        return str(int(x))
    s = '%.4f' % x
    s = s.rstrip('0').rstrip('.')
    return '0' if s in ('', '-0') else s


def coefs(c, vel=0.0, eg0=0.0, eg1=0.0, mod=0.0):
    # const, note, vel, eg0, eg1, mod
    return '%s,0,%s,%s,%s,%s' % (num(c), num(vel), num(eg0), num(eg1), num(mod))


def amp(c, vel=0.0, eg0=1.0):
    # An osc that should be silent must have ALL coefficients 0 (AMY skips
    # zero coefficients, so const 0 with an EG coefficient still renders).
    if c <= 0.00001:
        return '0,0,0,0,0,0'
    return '%s,0,%s,%s,0,0' % (num(c), num(vel), num(eg0))


def expmap(x, lo, hi):
    return lo * math.pow(hi / lo, x)


# ----------------------------------------------------------------------------
# Drum patch parameters (knob positions 0..1, like the Microtonic)
# ----------------------------------------------------------------------------
LEVEL_0DB = 0.5623     # LEVEL knob position of 0 dB (max = +10 dB)
MASTER_MAX = (1.0 / LEVEL_0DB) ** 2      # MASTER knob at full: +10 dB

KNOBS = [
    # key, label, default
    ('mix', 'OSC/NOISE', 0.5),
    ('eq_freq', 'EQ FREQ', 0.5),
    ('eq_gain', 'EQ GAIN', 0.5),
    ('dist', 'DISTORT', 0.0),
    ('level', 'LEVEL', LEVEL_0DB),
    ('pan', 'PAN', 0.5),
    ('osc_freq', 'OSC FREQ', 0.5),
    ('mod_amt', 'MOD AMT', 0.5),
    ('mod_rate', 'MOD RATE', 0.484),
    ('osc_att', 'OSC ATK', 0.0),
    ('osc_dec', 'OSC DEC', 0.5),
    ('n_freq', 'NOISE FREQ', 1.0),
    ('n_q', 'NOISE Q', 0.17),
    ('n_att', 'NOISE ATK', 0.0),
    ('n_dec', 'NOISE DEC', 0.5),
    ('vel_osc', 'VEL OSC', 0.0),
    ('vel_noise', 'VEL NOISE', 0.0),
    ('vel_mod', 'VEL MOD', 0.0),
]
KNOB_KEYS = [k for k, _, _ in KNOBS]
KNOB_LABEL = {k: l for k, l, _ in KNOBS}
BIPOLAR = ('eq_gain', 'pan', 'mod_amt')
SWITCHES = [
    # key, label, choices (value, text), default
    ('wave', 'WAVE', ((0, 'SIN'), (1, 'TRI'), (2, 'SAW')), 0),
    ('mod_mode', 'MOD', ((0, 'DECAY'), (1, 'SINE'), (2, 'RAND')), 0),
    ('n_mode', 'FILTER', ((0, 'LP'), (1, 'BP'), (2, 'HP')), 0),
    ('n_env', 'NOISE EG', ((0, 'EXP'), (1, 'LIN'), (2, 'CLAP')), 0),
    ('choke', 'CHOKE', ((0, 'OFF'), (1, 'ON')), 0),
]
SWITCH_KEYS = [k for k, _, _, _ in SWITCHES]


def default_patch():
    d = {k: v for k, _, v in KNOBS}
    for k, _, _, v in SWITCHES:
        d[k] = v
    return d


# --- value mappings (Microtonic ranges) ----------------------------------------
def freq_hz(k):
    return expmap(clamp(k), 20.0, 20000.0)        # 20 Hz .. 20 kHz, 0.5 = 632 Hz


def decay_ms(k):
    return expmap(clamp(k), 10.0, 10000.0)        # 10 ms .. 10 s, 0.5 = 316 ms


def attack_ms(k):
    return 10000.0 * k * k * k                    # 0 .. 10 s


def q_val(k):
    return 0.1 * math.pow(100000.0, clamp(k))     # 0.1 .. 10000, 0.17 = 0.707


def eq_db(k):
    return 80.0 * (k - 0.5)                       # -40 .. +40 dB


def level_gain(k):
    return (k / LEVEL_0DB) ** 2                   # 0 .. +10 dB


def mod_semis(p):
    rng = 96.0 if p['mod_mode'] == 0 else 48.0
    return bip(p['mod_amt']) * rng


def mod_rate(p):
    """decaying: ms (None = infinite), sine / random: Hz"""
    k = p['mod_rate']
    m = p['mod_mode']
    if m == 0:
        if k < 0.005:
            return None
        return 10.0 * math.pow(1248.0, 1.0 - k)   # inf .. 10 ms, 0.484 = 353 ms
    if k < 0.005:
        return 0.0
    if m == 1:
        return 0.1 * math.pow(20000.0, k)         # 0 .. 2 kHz
    return math.pow(20000.0, k)                   # 0 .. 20 kHz


def vel_k(s):
    """Microtonic velocity sensitivity s (0..2): gain = 1 - s*(1-v).  AMY's amp
    'vel' coefficient k gives gain = v^k: exact at accent (v=1) and normal (v=0.5)."""
    g = 1.0 - s * 0.5
    if s <= 0.0001:
        return 0.0
    if g <= 0.001:
        return 10.0
    return -math.log(g) / math.log(2.0)


def ms_txt(ms):
    if ms is None:
        return 'INF'
    return ('%dms' % ms) if ms < 1000 else ('%.2fs' % (ms / 1000.0))


def hz_txt(f):
    return ('%dHz' % f) if f < 1000 else ('%.1fk' % (f / 1000.0))


def fmt(key, p):
    v = p[key]
    if key in ('eq_freq', 'osc_freq', 'n_freq'):
        return hz_txt(freq_hz(v))
    if key == 'eq_gain':
        return '%+.1fdB' % eq_db(v)
    if key == 'mix':
        o = int((1.0 - v) * 100 + 0.5)
        return '%d/%d' % (o, 100 - o)
    if key == 'dist':
        return '%d' % int(v * 100 + 0.5)
    if key == 'level':
        if v < 0.003:
            return '-inf'
        return '%+.1fdB' % (20.0 * math.log(level_gain(v)) / math.log(10.0))
    if key == 'pan':
        b = int(bip(v) * 100 + (0.5 if v >= 0.5 else -0.5))
        return 'C' if b == 0 else ('L%d' % -b if b < 0 else 'R%d' % b)
    if key == 'mod_amt':
        return '%+.1fst' % mod_semis(p)
    if key == 'mod_rate':
        r = mod_rate(p)
        if p['mod_mode'] == 0:
            return ms_txt(r)
        return hz_txt(r) if r >= 1 else '%.2fHz' % r
    if key in ('osc_att', 'n_att'):
        return ms_txt(attack_ms(v))
    if key == 'osc_dec':
        return ms_txt(decay_ms(v))
    if key == 'n_dec':
        d = decay_ms(v)
        if p['n_env'] == 1:
            d = d * 2.0 / 3.0
        return ms_txt(d)
    if key == 'n_q':
        q = q_val(v)
        return ('%.2f' % q) if q < 10 else ('%d' % q)
    if key in ('vel_osc', 'vel_noise', 'vel_mod'):
        return '%d%%' % int(v * 200 + 0.5)
    return '%d' % int(v * 100 + 0.5)


# ----------------------------------------------------------------------------
# One drum channel -> AMY wire fragments
# ----------------------------------------------------------------------------
def ring_ms(p):
    """how long a hit of this patch can be heard (for the automatic note-off)"""
    t1 = attack_ms(p['osc_att']) + decay_ms(p['osc_dec'])
    nd = decay_ms(p['n_dec'])
    if p['n_env'] == 1:
        nd *= 2.0 / 3.0
    t2 = attack_ms(p['n_att']) + nd
    if p['mix'] < 0.01:
        t2 = 0
    if p['mix'] > 0.99:
        t1 = 0
    return max(t1, t2) + 30.0


def comp_ms(p):
    """(tone, noise) audible time in ms, None for a silent part"""
    t1 = attack_ms(p['osc_att']) + decay_ms(p['osc_dec']) + 20.0
    nd = decay_ms(p['n_dec'])
    if p['n_env'] == 1:
        nd *= 2.0 / 3.0
    t2 = attack_ms(p['n_att']) + nd + 20.0
    return (None if p['mix'] > 0.998 else t1), (None if p['mix'] < 0.002 else t2)


def env_exp(att, dec, rel=None):
    """attack to 1, decay to 0; release (note-off) continues with the decay
    (or fades within rel ms: CPU saver)"""
    r = dec if rel is None else min(dec, rel)
    return '%d,1,%d,0,%d,0' % (int(att + 0.5), max(1, int(dec + 0.5)), max(1, int(r + 0.5)))


def env_clap(att, dec, rel=None):
    """Microtonic 'modulated' noise EG: a few fast re-triggered bursts during
    the attack time, then the decay.  (AMY: 7 breakpoints + release max.)"""
    n = 1 if att < 6 else (2 if att < 25 else 3)
    out = []
    if n > 1:
        gap = att / float(n - 1)
        for _ in range(n - 1):
            out.append('1,1,%d,0.08' % max(2, int(gap - 1)))
    d = max(1, int(dec + 0.5))
    r = d if rel is None else max(1, int(min(dec, rel) + 0.5))
    out.append('1,1,%d,0,%d,0' % (d, r))
    return ','.join(out)


def eq_params(p):
    """EQ as a head filter -> (filter type, freq, Q, gain compensation)
    AMY has no per-osc peaking EQ: boost = resonant HP (below 1 kHz, keeps the
    highs) or LP (above, keeps the lows) peaking at EQ FREQ; cut = notch
    (wider for more cut)."""
    g = eq_db(p['eq_gain'])
    f = freq_hz(p['eq_freq'])
    if abs(g) < 1.5:              # (inaudible: no filter, saves AMY time)
        return F_NONE, f, 0.707, 1.0
    if g < 0:
        q = 6.0 * math.pow(0.06, -g / 40.0)
        return F_NOTCH, f, q, 1.0
    q = clamp(0.707 * math.pow(10.0, g / 20.0), 0.707, 40.0)
    comp = math.pow(0.707 / q, 0.5)
    return (F_HPF if f < 1000.0 else F_LPF), clamp(f, 25.0, 18000.0), q, comp


class Voice:
    """Builds the AMY messages of one drum channel from its (effective) patch.
    Everything is split into per-osc parameter fragments so that only what
    changed is sent (AMY keeps the rest)."""

    def __init__(self, ch):
        self.ch = ch
        self.synth = SYNTH0 + ch
        self.sent = {}          # (osc, code) -> text last sent

    def static_parts(self, p, master=1.0, rnd_off=0.0, eco=0):
        """{(osc, code): text}  - everything that is not per hit"""
        out = {}
        # --- head: distortion -> EQ, pan
        ft, ef, eq_q, comp = eq_params(p)
        head_amp = G_HEAD * comp
        if not HAS_LEVEL:
            head_amp *= level_gain(p['level']) * master * POST_GAIN
        dist = p['dist']
        pan = clamp(0.5 + 0.5 * bip(p['pan']))
        h = O_HEAD
        out[(h, 'w')] = 'w%d' % SILENT
        out[(h, 'c')] = 'c%d' % O_TONE
        out[(h, 'a')] = 'a' + amp(head_amp, 0.0, 0.0)
        out[(h, 'Q')] = 'Q' + num(pan)
        out[(h, 'G')] = 'G%d' % ft
        if ft != F_NONE:
            out[(h, 'F')] = 'F' + coefs(ef)
            out[(h, 'R')] = 'R' + num(eq_q)
        if HAS_DIST:
            if dist > 0.03:
                drive = expmap(dist, 1.0, 24.0)
                out[(h, 'GC')] = 'GC1'
                out[(h, 'GD')] = 'GD' + num(drive)
                out[(h, 'GM')] = 'GM1'
            else:
                out[(h, 'GC')] = 'GC0'
        # --- mix (equal power)
        g_osc = math.cos(p['mix'] * math.pi * 0.5)
        g_noise = math.sin(p['mix'] * math.pi * 0.5)
        if g_osc < 0.002:
            g_osc = 0.0
        if g_noise < 0.002:
            g_noise = 0.0
        # --- tone
        f0 = freq_hz(p['osc_freq'])
        mode = p['mod_mode']
        semis = mod_semis(p)
        rate = mod_rate(p)
        oct_amt = semis / 12.0
        wave = (W_SINE, W_TRI, W_SAW)[p['wave']]
        fm = mode == 1 and rate > LFO_MAX_HZ and abs(oct_amt) > 0.001
        att = attack_ms(p['osc_att'])
        dec = decay_ms(p['osc_dec'])
        k_osc = vel_k(p['vel_osc'] * 2.0)
        rel = ECO_REL_MS if eco >= 1 else None
        tone_env = env_exp(att, dec, rel)
        eg1 = 0.0
        mod = 0.0
        fconst = f0
        lfo = None
        if mode == 0:
            if rate is None:
                fconst = f0 * math.pow(2.0, oct_amt)       # stuck at the start pitch
            elif abs(oct_amt) > 0.001:
                eg1 = oct_amt
                r = max(1, int(rate + 0.5))
                out[(O_TONE, 'B')] = 'B0,1,%d,0,%d,0' % (r, r)
        elif mode == 1:
            if not fm and abs(oct_amt) > 0.001 and rate > 0:
                lfo = 'w%df%sa1' % (W_SINE, num(rate))
                mod = oct_amt
            elif rate <= 0:
                fconst = f0          # rate 0: stuck at the centre
        else:
            if rate >= RND_PY_MAX_HZ and abs(oct_amt) > 0.001:
                lfo = 'w%da1' % W_NOISE
                mod = oct_amt
            else:
                fconst = f0 * math.pow(2.0, rnd_off * oct_amt)
        fconst = clamp(fconst, 1.0, 20000.0)
        if fm:
            # carrier = sine at the tone frequency, modulator at RATE Hz
            dev = f0 * (math.pow(2.0, abs(oct_amt)) - math.pow(2.0, -abs(oct_amt))) * 0.5
            idx = min(dev / rate, 12.0)
            out[(O_TONE, 'w')] = 'w%d' % W_ALGO
            out[(O_TONE, 'o')] = 'o1O,,,,%d,%d' % (O_OPM, O_OPC)
            out[(O_TONE, 'f')] = 'f' + coefs(fconst)
            out[(O_TONE, 'a')] = 'a' + amp(g_osc * G_TONE * G_ALGO, k_osc)
            out[(O_TONE, 'A')] = 'A' + tone_env
            out[(O_OPM, 'w')] = 'w%d' % W_SINE
            out[(O_OPM, 'f')] = 'f' + coefs(rate)
            out[(O_OPM, 'a')] = 'a' + amp(max(idx / FM_IDX, 0.0005), 0.0, 0.0)
            out[(O_OPC, 'w')] = 'w%dI1' % W_SINE
            out[(O_OPC, 'a')] = 'a' + amp(1.0, 0.0, 0.0)
        else:
            out[(O_TONE, 'w')] = 'w%d' % wave
            out[(O_TONE, 'f')] = 'f' + coefs(fconst, 0.0, 0.0, eg1, mod)
            out[(O_TONE, 'a')] = 'a' + amp(g_osc * G_TONE, k_osc)
            out[(O_TONE, 'A')] = 'A' + tone_env
        if mode != 0:
            out[(O_TONE, 'B')] = 'B0,0,1,0'
        out[(O_TONE, 'c')] = 'c%d' % O_NOISE
        if lfo:
            out[(O_LFO, 'w')] = lfo
            out[(O_TONE, 'L')] = 'L%d' % O_LFO
        # --- noise
        nm = p['n_mode']
        ntype = (F_LPF, F_BPF, F_HPF)[nm]
        nf = clamp(freq_hz(p['n_freq']), 20.0, 19000.0)
        q = clamp(q_val(p['n_q']), 0.1, 100.0)
        if nm == 1:
            ncomp = 1.0 / math.sqrt(max(q, 1.0))
        else:
            ncomp = math.pow(0.707 / q, 0.6) if q > 0.707 else 1.0
        natt = attack_ms(p['n_att'])
        ndec = decay_ms(p['n_dec'])
        k_noise = vel_k(p['vel_noise'] * 2.0)
        if p['n_env'] == 1:
            ndec *= 2.0 / 3.0
            nenv = env_exp(natt, ndec, rel)
            etype = 1
        elif p['n_env'] == 2:
            nenv = env_clap(natt, ndec, rel)
            etype = 1
        else:
            nenv = env_exp(natt, ndec, rel)
            etype = 0
        ng = g_noise * G_NOISE * ncomp
        o = O_NOISE
        if ng > 0:
            out[(o, 'w')] = 'w%d' % W_NOISE
            out[(o, 'a')] = 'a' + amp(ng, k_noise)
            out[(o, 'A')] = 'A' + nenv
            out[(o, 'T')] = 'T%d' % etype
            if (nm == 0 and nf >= 18000.0 and q <= 1.0) or (nm == 2 and nf <= 25.0 and q <= 1.0):
                out[(o, 'G')] = 'G%d' % F_NONE     # wide open: the filter would do nothing
            else:
                out[(o, 'G')] = 'G%d' % ntype
                out[(o, 'F')] = 'F' + coefs(nf)
                out[(o, 'R')] = 'R' + num(q)
        else:
            out[(o, 'a')] = 'a' + amp(0)
        return out

    def level(self, p, master):
        return level_gain(p['level']) * master * POST_GAIN

    def send(self, raw, parts, force=False):
        """send what differs from what AMY has (by osc, one message each)"""
        by_osc = {}
        for k, v in parts.items():
            if force or self.sent.get(k) != v:
                by_osc[k[0]] = by_osc.get(k[0], '') + v
                self.sent[k] = v
        msgs = []
        for o in sorted(by_osc):
            msgs.append('i%dv%d%sZ' % (self.synth, o, by_osc[o]))
        if msgs:
            raw(''.join(msgs))
        return len(msgs)

    def hit_msg(self, p, vel, choke_kill=None, head_amp=None):
        """note-on message(s) for one hit at velocity vel (0..1)"""
        s = self.synth
        m = ''
        if choke_kill:
            m += choke_kill
        if head_amp is not None:
            m += 'i%dv%da%sZ' % (s, O_HEAD, head_amp)
        sv = p['vel_mod'] * 2.0
        if sv > 0.0001 and abs(bip(p['mod_amt'])) > 0.001:
            g = clamp(1.0 - sv * (1.0 - vel))
            oct_amt = mod_semis(p) / 12.0 * g
            mode = p['mod_mode']
            rate = mod_rate(p)
            if mode == 0 and rate is not None:
                m += 'i%dv%df,,,,%sZ' % (s, O_TONE, num(oct_amt))
            elif mode == 1 and rate is not None and rate > LFO_MAX_HZ:
                f0 = freq_hz(p['osc_freq'])
                a = abs(mod_semis(p) / 12.0) * g
                dev = f0 * (math.pow(2.0, a) - math.pow(2.0, -a)) * 0.5
                m += 'i%dv%da%sZ' % (s, O_OPM, amp(max(min(dev / rate, 12.0) / FM_IDX, 0.0005), 0, 0))
            elif (mode == 1 and rate is not None and 0 < rate <= LFO_MAX_HZ) or \
                    (mode == 2 and rate is not None and rate >= RND_PY_MAX_HZ):
                m += 'i%dv%df,,,,,%sZ' % (s, O_TONE, num(oct_amt))
        v = num(max(0.01, vel))
        m += 'i%dv%dn60l%sZ' % (s, O_HEAD, v)
        return m

    def off_msg(self):
        return 'i%dv%dl0Z' % (self.synth, O_HEAD)


# ----------------------------------------------------------------------------
# Patterns
# ----------------------------------------------------------------------------
def new_pattern():
    # per channel 16-bit masks: trig, accent, fill
    return {'len': 16, 'trig': [0] * NCH, 'acc': [0] * NCH, 'fill': [0] * NCH, 'chain': False}


def pat_copy(p):
    return {'len': p['len'], 'trig': list(p['trig']), 'acc': list(p['acc']),
            'fill': list(p['fill']), 'chain': bool(p.get('chain', False))}


def pat_from_strings(rows, length=16, chain=False):
    """rows: up to 8 strings of 16 chars: '.' off, 'x' hit, 'X' accent,
    'f' fill, 'F' accented fill"""
    p = new_pattern()
    p['len'] = length
    p['chain'] = chain
    for c, r in enumerate(rows[:NCH]):
        for i, ch in enumerate(r[:NSTEP]):
            b = 1 << i
            if ch in 'xXfF':
                p['trig'][c] |= b
            if ch in 'XF':
                p['acc'][c] |= b
            if ch in 'fF':
                p['fill'][c] |= b
    return p


def chain_of(pats, i):
    """the patterns of the chain that pattern i belongs to (in order)"""
    a = i
    while a > 0 and pats[a - 1]['chain']:
        a -= 1
    b = i
    while b < NPAT - 1 and pats[b]['chain']:
        b += 1
    return list(range(a, b + 1))


# ----------------------------------------------------------------------------
# Engine: 8 channels + morph + AMY loop pattern player
# ----------------------------------------------------------------------------
class GinTonic:
    def __init__(self, send_raw=None, ticks_fn=None):
        self._raw = send_raw or getattr(amy, 'send_raw', None) or getattr(amy, 'send_wire', None)
        if ticks_fn is None:
            try:
                import tulip
                ticks_fn = tulip.seq_ticks
            except Exception:
                ticks_fn = None
        self._ticks_fn = ticks_fn
        # sound
        self.a = [default_patch() for _ in range(NCH)]     # morph left end-points
        self.b = [default_patch() for _ in range(NCH)]     # morph right end-points
        self.morph = 0.0
        self.master = 1.0
        self.mute = [False] * NCH          # mute buttons (not saved with the preset)
        self.hold_mute = [False] * NCH     # MIDI keys C2..G2 held
        self.names = ['CH %d' % (c + 1) for c in range(NCH)]   # drum patch names
        self.voices = [Voice(c) for c in range(NCH)]
        self._eff = [None] * NCH
        self._rnd = [0.0] * NCH            # random pitch offset (-1..1) per channel
        self._rnd_due = [0] * NCH
        self._hamp = [None] * NCH          # head amp text (choke restores it)
        self._sig = [None] * NCH           # hit message signature per channel
        self._choke_seen = set()           # channels a choke may have silenced (head amp 0)
        self._hcache = {}                  # (ch, vel, chokers) -> hit message
        self.eco = 0                       # CPU saver level (not saved)
        self._silenced = set()             # silence(): heads restored by the next hit
        self._unsil = 0
        self._rnd_hit = {}                 # (tick offset, ch) -> random pitch (per-hit random)
        self._pass_no = None
        # patterns
        self.pats = [new_pattern() for _ in range(NPAT)]
        self.cur = 0               # pattern being edited / selected
        self.playing_pat = 0       # pattern sounding now
        self.next_pat = None       # pattern to switch to at the end of the pass
        self.rate = 2              # index into RATES (1/16)
        self.swing = 0.0           # 0..1 -> 0..50 % of a step
        self.fill_rate = 0         # index into FILL_RATES
        self.tempo = 120.0
        self.clock_ext = False
        self.clock_out = False     # Tulip sends MIDI clock (UI sets it)
        self.ext_origin = None     # EXT clock: AMY tick of song position 0 (last Start / Continue)
        self.ext_playing = False   # EXT clock: the master is playing (between Start and Stop)
        self._held_at = None       # tick count when we held AMY's transport (zY0)
        self.sync_ms = 30          # SYNC OFFSET: play this much earlier than the MIDI clock grid
        self._first_tag = None     # EXT: the downbeat right after Start (the loop sits early)
        self.midi_out_fn = None    # fn(bytes): raw MIDI out (Song Position before Start)
        # MIDI out of the pattern
        self.midi_out_ch = 0       # 0 = off
        self.midi_out_base = 36    # C1..G1 = channels 1..8 (as the Microtonic)
        self._midi_amy = hasattr(amy, 'NOTE_OUTPUT_MIDI_OUT')
        self._midi_cfg = None
        # transport
        self.running = False
        self.led_step = -1
        self.last_cb_tick = None
        self.last_cb_ms = 0
        self.last_hit_ms = [0] * NCH
        self.voice_on = [False] * NCH
        # loop engine state
        self._lay = None           # current loop layout
        self._prev = None          # (layout, until_tick) of the loop being replaced (LEDs)
        self._ev = {}              # offset -> [tag, abs_offset, wire, kind, fire_tick]
        self._free = list(range(TAG_LAST, TAG_FIRST - 1, -1))
        self._old = []             # [(tag, free_after_tick)]
        self._pend = set()
        self._conv = {}            # offset -> tick: one-shot first hit, make periodic after it
        self._warned = False
        self._warned_tags = False
        self._starting = False      # inside the first _apply of a (re)started loop
        self._bpm_t = None          # EXT clock: (tick, ms) for the tempo estimate
        self._ring_ticks = [0] * NCH
        self._comp_ticks = [(None, None)] * NCH   # (tone, noise) ring in ticks
        self._setup()

    # --- low level -------------------------------------------------------------
    def _setup(self):
        try:
            self._clear_all()
        except Exception:
            pass
        for c in range(NCH):
            v = self.voices[c]
            self._raw('i%div1in%dZ' % (v.synth, OSCS_PER_VOICE))
            v.sent = {}
        self.refresh_all(force=True)

    def eff(self, c):
        """effective patch of channel c at the current morph position"""
        e = self._eff[c]
        if e is not None:
            return e
        a = self.a[c]
        b = self.b[c]
        m = self.morph
        if m <= 0.0:
            e = dict(a)
        elif m >= 1.0:
            e = dict(b)
        else:
            e = {}
            for k in KNOB_KEYS:
                e[k] = a[k] + (b[k] - a[k]) * m
            for k in SWITCH_KEYS:
                e[k] = a[k] if m < 0.5 else b[k]
        self._eff[c] = e
        return e

    def refresh(self, c, force=False):
        """send channel c's sound to AMY (only what changed)"""
        self._eff[c] = None
        self._hcache = {}
        p = self.eff(c)
        v = self.voices[c]
        parts = v.static_parts(p, self.master, self._rnd[c], self.eco)
        self._hamp[c] = parts[(O_HEAD, 'a')][1:]
        if c in self._choke_seen and not self._unsil:
            v.sent.pop((O_HEAD, 'a'), None)          # AMY may hold a choked (0) head amp
        if self._unsil:
            parts = dict(parts)                       # silence(): heads stay down for now
            parts.pop((O_HEAD, 'a'), None)
        v.send(self._raw, parts, force)
        if HAS_LEVEL:
            lv = num(v.level(p, self.master))
            if force or v.sent.get('lvl') != lv:
                v.sent['lvl'] = lv
                self._raw('i%diV%sZ' % (v.synth, lv))
        return self._calc_ring(c)

    def _calc_ring(self, c):
        """ring times in ticks (channel, tone part, noise part) for the automatic note-offs"""
        p = self.eff(c)
        k = self.tempo * 48 / 60000.0
        cap = ECO_CAP_MS[self.eco]
        rm = ring_ms(p)
        t1, t2 = comp_ms(p)
        if cap is not None:
            rm = min(rm, cap)
            t1 = None if t1 is None else min(t1, cap)
            t2 = None if t2 is None else min(t2, cap)
        rt = int(rm * k) + 2
        comp = (None if t1 is None else int(t1 * k) + 1, None if t2 is None else int(t2 * k) + 1)
        if rt != self._ring_ticks[c] or comp != self._comp_ticks[c]:
            self._ring_ticks[c] = rt
            self._comp_ticks[c] = comp
            return True
        return False

    def refresh_all(self, force=False):
        for c in range(NCH):
            self.refresh(c, force)
        if self._lay is not None:
            self.sync_loop()

    def set_knob(self, c, key, val):
        """turn a drum patch knob of channel c at the current morph position:
        both end-points move so that the sounding value follows the knob"""
        val = clamp(val)
        m = self.morph
        a = self.a[c]
        b = self.b[c]
        if m <= 0.0:
            a[key] = val
        elif m >= 1.0:
            b[key] = val
        else:
            cur = a[key] + (b[key] - a[key]) * m
            d = val - cur
            n = (1 - m) * (1 - m) + m * m
            a[key] = clamp(a[key] + d * (1 - m) / n)
            b[key] = clamp(b[key] + d * m / n)
        self._eff[c] = None

    def set_switch(self, c, key, val):
        """switch of channel c: at MORPH = A only A, at B only B, between: both
        (the sounding switch flips half way)"""
        old = self.eff(c)[key]
        if self.morph < 1.0:
            self.a[c][key] = val
        if self.morph > 0.0:
            self.b[c][key] = val
        self._eff[c] = None
        if key == 'choke' and old != val:
            # a choked head may sit at amp 0: make the next refresh resend them all
            for v in self.voices:
                v.sent.pop((O_HEAD, 'a'), None)

    def set_eco(self, level, apply=True):
        level = max(0, min(3, int(level)))
        if level == self.eco:
            return
        if not apply:
            # (kit load: set before set_state, which refreshes everything anyway)
            self.eco = level
            self._hcache = {}
            return
        self.eco = level
        self._hcache = {}
        for c in range(NCH):
            self.refresh(c)
        if self._lay is not None:
            self.sync_loop()

    def est_load(self, pi=None):
        """rough AMY load of a pattern with the current sounds, in the units the
        GinTonic tests measured (factory kits peak ~230)"""
        if pi is None:
            pi = self.playing_pat if self.running else self.cur
        pat = self.pats[pi]
        n = max(1, pat['len'])
        L = self.step_ticks()
        P = n * L
        k = self.tempo * 48 / 60000.0
        tot = 0.0
        for c in range(NCH):
            if self.mute[c]:
                continue
            p = self.eff(c)
            hits = [i * L for i in range(n) if pat['trig'][c] & (1 << i)]
            if not hits:
                continue
            ft, nt, eq = 2.0, 5.0, 0.0
            if p['mod_mode'] == 1 and (mod_rate(p) or 0) > LFO_MAX_HZ:
                ft = 6.0
            if abs(eq_db(p['eq_gain'])) >= 1.5:
                eq += 3.0
            if p['dist'] > 0.03:
                eq += 2.0
            t1, t2 = comp_ms(p)
            act = 0.0
            for j, t in enumerate(hits):
                gap = (hits[j + 1] if j + 1 < len(hits) else hits[0] + P) - t
                a1 = 0.0 if t1 is None else min(gap, t1 * k) / P
                a2 = 0.0 if t2 is None else min(gap, t2 * k) / P
                act += ft * a1 + nt * a2 + eq * max(a1, a2)
            tot += act * 8.0
        return tot

    def silence(self):
        """cut every sounding drum at once (kit change): heads to ~0 and released,
        so AMY stops rendering their tails; the next refresh restores the heads"""
        m = ''
        for c in range(NCH):
            v = self.voices[c]
            m += 'i%dv%da0.0001,0,0,0,0,0Zi%dv%dl0Z' % (v.synth, O_HEAD, v.synth, O_HEAD)
            self.voice_on[c] = False
        self._raw(m)
        # the heads stay down until AMY has dropped the tails (a few blocks);
        # a hit before that brings its own head back
        self._silenced = set(range(NCH))
        self._hcache = {}
        self._unsil = ticks_ms() + 80

    def tick_ms(self, now):
        """UI tick: restore the heads after silence(); MIDI Continue check"""
        if self._unsil and now >= self._unsil:
            self._unsil = 0
            for c in range(NCH):
                self.voices[c].sent.pop((O_HEAD, 'a'), None)
                self.refresh(c)
            # the heads are back: hits no longer need to restore them
            self._silenced = set()
            self._hcache = {}
            if self._lay is not None:
                self.sync_loop()

    def set_tempo(self, bpm):
        self.tempo = clamp(bpm, 20.0, 300.0)
        ch = False
        for c in range(NCH):
            if self._calc_ring(c):
                ch = True
        if ch and self._lay is not None:
            self.sync_loop()

    def set_morph(self, m):
        self.morph = clamp(m)
        self._eff = [None] * NCH

    def morph_copy(self, to_b=True):
        for c in range(NCH):
            if to_b:
                self.b[c] = dict(self.a[c])
            else:
                self.a[c] = dict(self.b[c])
        self._eff = [None] * NCH

    def hit_sig_changed(self, c):
        """after a knob change: do the loop's hit messages for channel c differ?"""
        p = self.eff(c)
        v = self.voices[c]
        sig = (v.hit_msg(p, 1.0), v.hit_msg(p, 0.5), self._ring_ticks[c], self._comp_ticks[c],
               self._head_amp_txt(c) if (p['choke'] == 1 or c in self._choke_seen or c in self._silenced) else '',
               self.per_hit_dynamic(c),
               p['choke'])
        if sig != self._sig[c]:
            self._sig[c] = sig
            return True
        return False

    def per_hit_dynamic(self, c):
        """does the hit message depend on something that changes per hit?"""
        p = self.eff(c)
        return p['mod_mode'] == 2 and abs(bip(p['mod_amt'])) > 0.001 and (mod_rate(p) or 0) < 0.4

    def py_random(self, now_ms):
        """random pitch mod between 0.4 and RND_PY_MAX_HZ: Python sample & hold"""
        for c in range(NCH):
            p = self.eff(c)
            if p['mod_mode'] != 2 or abs(bip(p['mod_amt'])) < 0.001:
                continue
            r = mod_rate(p)
            if r < 0.4 or r >= RND_PY_MAX_HZ:
                continue
            if now_ms >= self._rnd_due[c]:
                self._rnd_due[c] = now_ms + int(1000.0 / r)
                self._rnd[c] = random.random() * 2.0 - 1.0
                v = self.voices[c]
                f0 = freq_hz(p['osc_freq'])
                f = clamp(f0 * math.pow(2.0, self._rnd[c] * mod_semis(p) / 12.0), 1.0, 20000.0)
                t = 'f' + coefs(f)
                v.sent[(O_TONE, 'f')] = t
                self._raw('i%dv%d%sZ' % (v.synth, O_TONE, t))

    # --- hits ------------------------------------------------------------------------
    def _choke_list(self):
        return [c for c in range(NCH) if self.eff(c)['choke'] == 1]

    def _head_amp_txt(self, c):
        if self._hamp[c] is None:
            p = self.eff(c)
            self._hamp[c] = self.voices[c].static_parts(p, self.master, self._rnd[c])[(O_HEAD, 'a')][1:]
        return self._hamp[c]

    def _hit(self, c, vel, chokers=None, key=None):
        if chokers is None:
            chokers = self._choke_list()
        dyn = self.per_hit_dynamic(c)
        if not dyn:
            ck = (c, vel, tuple(chokers))
            m = self._hcache.get(ck)
            if m is not None:
                return m
        p = self.eff(c)
        v = self.voices[c]
        kill = None
        ha = None
        if p['choke'] == 1 and len(chokers) > 1:
            kill = ''
            for j in chokers:
                if j != c:
                    if j not in self._choke_seen:
                        self._choke_seen.add(j)
                        self._hcache = {}
                    vj = self.voices[j]
                    kill += 'i%dv%da0.0001,0,0,0,0,0Z' % (vj.synth, O_HEAD)
            ha = self._head_amp_txt(c)
        elif c in self._choke_seen or c in self._silenced:
            ha = self._head_amp_txt(c)          # may have been choked / silenced: restore
        msg = v.hit_msg(p, vel, kill, ha)
        if not dyn:
            if len(self._hcache) > 200:
                self._hcache = {}
            self._hcache[ck] = msg
        else:
            # random pitch per hit: the offset rides with the note-on
            f0 = freq_hz(p['osc_freq'])
            r = None
            if key is not None:
                r = self._rnd_hit.get(key)
            if r is None:
                r = random.random() * 2.0 - 1.0
                if key is not None:
                    self._rnd_hit[key] = r
            sv = p['vel_mod'] * 2.0
            g = clamp(1.0 - sv * (1.0 - vel)) if sv > 0.0001 else 1.0      # VEL MOD
            f = clamp(f0 * math.pow(2.0, r * mod_semis(p) * g / 12.0), 1.0, 20000.0)
            msg = 'i%dv%df%sZ' % (v.synth, O_TONE, num(f)) + msg
        return msg

    def trigger(self, c, vel=0.5):
        """play channel c now (UI / MIDI note)"""
        if self.mute[c] or self.hold_mute[c]:
            return
        self._raw(self._hit(c, vel))
        self.last_hit_ms[c] = ticks_ms()
        self.voice_on[c] = True

    def note_off(self, c):
        self._raw(self.voices[c].off_msg())

    # --- MIDI out -----------------------------------------------------------------
    def midi_all_off(self):
        """note-off for the 8 pattern-out notes (AMY's all-notes-off sends no MIDI)"""
        if self._midi_amy and self._midi_cfg:
            self._raw(self._midi_off_wire())

    def midi_setup(self):
        if not self._midi_amy:
            return
        cfg = self.midi_out_ch
        if cfg == self._midi_cfg:
            return
        self.midi_all_off()                   # on the old channel
        self._midi_cfg = cfg
        try:
            if cfg:
                self._raw('i%diG%d,%dZ' % (MIDI_OUT_SYNTH, amy.NOTE_OUTPUT_MIDI_OUT, cfg))
            else:
                self._raw('i%dl0Z' % MIDI_OUT_SYNTH)
                self._raw('i%diG%dZ' % (MIDI_OUT_SYNTH, amy.NOTE_OUTPUT_OFF))
        except Exception as e:
            print('gintonix: MIDI out setup', e)
            self._midi_amy = False

    # --- loop layout ------------------------------------------------------------------
    def step_ticks(self):
        return RATES[self.rate][1]

    def layout(self, pi):
        """one pass of pattern pi -> {'pi', 'n', 'L', 'P', 'starts', 'des': {offset: wire}}"""
        pat = self.pats[pi]
        n = max(1, min(NSTEP, pat['len']))
        L = self.step_ticks()
        sw = self.swing
        reps = 2 if (n & 1 and sw > 0.004) else 1      # swing needs an even number of steps
        N = n * reps
        starts = []
        for k in range(N):
            t = k * L
            if k & 1:
                t += int(sw * 0.5 * L + 0.5)
            starts.append(t)
        P = N * L
        groups = {}                       # tick offset -> [(ch, vel)]
        fr = FILL_RATES[self.fill_rate]
        for k in range(N):
            i = k % n
            b = 1 << i
            t = starts[k]
            Ls = (starts[k + 1] if k + 1 < N else P) - t
            for c in range(NCH):
                if not pat['trig'][c] & b or self.mute[c] or self.hold_mute[c]:
                    continue
                acc = pat['acc'][c] & b
                v0 = 1.0 if acc else 0.5
                if pat['fill'][c] & b:
                    lastt = -1
                    for h in range(fr):
                        tt = t + int(h * Ls / float(fr) + 0.5)
                        if tt <= lastt:
                            continue
                        lastt = tt
                        vel = v0 * (1.0 - 0.5 * h / fr) if acc else v0 * (1.0 - float(h) / fr)
                        groups.setdefault(tt, []).append((c, vel))
                else:
                    groups.setdefault(t, []).append((c, v0))
        # choke: at one tick only the lowest choke channel plays
        chokers = self._choke_list()
        if len(chokers) > 1:
            for t, hl in groups.items():
                cs = [c for c, _ in hl if c in chokers]
                if len(cs) > 1:
                    keep = min(cs)
                    groups[t] = [(c, v) for c, v in hl if c not in chokers or c == keep]
        ticks = sorted(groups)
        # budget (AMY sequencer tags)
        budget = TAG_LAST - TAG_FIRST + 1 - 8
        if len(ticks) > budget:
            if not self._warned:
                print('gintonix: too many fill hits for the AMY sequencer, some are dropped')
                self._warned = True
            keep = set(starts)
            ticks = [t for t in ticks if t in keep] + [t for t in ticks if t not in keep]
            ticks = sorted(ticks[:budget])
            groups = {t: groups[t] for t in ticks}
        # automatic note-offs (lets AMY stop rendering silent channels): put
        # them on a tick that has an event anyway, after the sound has died
        # and before the channel's next hit.
        msgs = {t: '' for t in ticks}
        offs = {t: '' for t in ticks}
        if ticks:
            for c in range(NCH):
                ht = [t for t in ticks if any(cc == c for cc, _ in groups[t])]
                if not ht:
                    continue
                rt = self._ring_ticks[c]
                vc = self.voices[c]
                ct = self._comp_ticks[c]
                # a part (tone / noise) that has died away gets its own note-off:
                # AMY then stops rendering it (and its filter) while the other
                # part still rings.
                parts = []
                if ct[0] is not None and ct[1] is not None:
                    parts.append((ct[0], 'i%dv%dl0Z' % (vc.synth, O_TONE)))
                    parts.append((ct[1], 'i%dv%dl0Z' % (vc.synth, O_NOISE)))
                all_t = ticks + [x + P for x in ticks]
                for j, t in enumerate(ht):
                    nxt = ht[j + 1] if j + 1 < len(ht) else ht[0] + P
                    th = None
                    if nxt - t > rt + 1:
                        for t2 in all_t:
                            if t + rt < t2 < nxt:
                                offs[t2 % P] += vc.off_msg()
                                th = t2
                                break
                    for r, msg in parts:
                        if r >= rt - 1 or nxt - t <= r + 1:
                            continue
                        for t2 in all_t:
                            if t + r < t2 < nxt:
                                if th is None or t2 < th:
                                    offs[t2 % P] += msg
                                break
        midi = self.midi_out_ch and self._midi_amy
        mmsg = {}
        last = ()
        for t in ticks:
            m = offs[t]
            mm = ''
            if midi:
                for nt in last:
                    mm += 'i%dn%dl0Z' % (MIDI_OUT_SYNTH, nt)
            notes = []
            for c, vel in sorted(groups[t]):
                m += self._hit(c, vel, chokers, (t, c))
                if midi:
                    nt = self.midi_out_base + c
                    mv = 1.0 if vel >= 0.99 else max(0.01, vel * 1.008)   # 127 / 64
                    mm += 'i%dn%dl%sZ' % (MIDI_OUT_SYNTH, nt, num(mv))
                    notes.append(nt)
            last = notes
            msgs[t] = m
            mmsg[t] = mm
        comp = self.comp_ticks()
        if midi and ticks:
            # pass start: every pattern note off first (also the previous
            # pattern's, after a switch), then this tick's notes
            mmsg[ticks[0]] = self._midi_off_wire() + mmsg[ticks[0]]
            # MIDI leaves at once: it stays on the clock grid, while the sound
            # goes 'comp' ticks early (SYNC OFFSET, Tulip's audio buffer)
            for t in ticks:
                if mmsg[t]:
                    o = (t + comp) % P
                    msgs[o] = msgs.get(o, '') + mmsg[t]
        chs = set(c for t in ticks for c, _ in groups[t])
        return {'pi': pi, 'n': n, 'N': N, 'L': L, 'P': P, 'starts': starts, 'des': msgs,
                'chs': chs, 'comp': comp, 'key': (pi, n, L, reps, tuple(starts), comp)}

    # --- AMY event bookkeeping ------------------------------------------------------
    def _now_tick(self):
        if self._ticks_fn is not None:
            try:
                return int(self._ticks_fn())
            except Exception:
                self._ticks_fn = None
        t = self.last_cb_tick or 0
        return t + int((ticks_ms() - self.last_cb_ms) * self.tempo * 48 / 60000.0)

    def _tag(self):
        if not self._free:
            if not self._warned_tags:
                print('gintonix: out of AMY sequencer tags')
                self._warned_tags = True
            return None
        return self._free.pop()

    def _add(self, tag, tick, period, wire):
        self._raw('H%d,%d,%d' % (tick, period, tag) + wire)

    def _clear(self, tag):
        self._raw('H0,0,%dZ' % tag)

    def _next_fire(self, absoff, P, now):
        return now + (absoff - now) % P

    def _place(self, off, wire, now, lay, start_b):
        """schedule wire at loop offset off (layout lay starts at start_b)"""
        P = lay['P']
        absoff = (lay['T0'] + off) % P
        tn = self._next_fire(absoff, P, now)
        ev = self._ev.get(off)
        if ev is None:
            tag = self._tag()
            if tag is None:
                return
        else:
            tag = ev[0]
            self._clear(tag)
        first = start_b + (absoff - start_b) % P      # its first hit in this loop
        if self._starting and first - now <= P_GUARD and now - first < self.step_ticks():
            # the loop's very first hits are due right now (Python was slow
            # at PLAY): play them a moment late instead of a whole pass later
            ft = max(first, now + 1)
            self._add(tag, ft, 0, wire)
            self._ev[off] = [tag, absoff, wire, 'once', ft]
            self._conv[off] = ft
            return
        if tn < start_b or tn - now <= P_GUARD:
            # would fire before the loop starts (or too soon to be safe):
            # a one-shot on its first real hit, made periodic after that
            ft = tn
            while ft < start_b or ft - now <= P_GUARD:
                ft += P
            self._add(tag, ft, 0, wire)
            self._ev[off] = [tag, absoff, wire, 'once', ft]
            self._conv[off] = ft
        else:
            self._add(tag, absoff, P, wire)
            self._ev[off] = [tag, absoff, wire, 'loop', tn]
            self._conv.pop(off, None)

    def _apply(self, keys=None):
        """bring AMY's events in line with the wanted ones (rewrite only changes)"""
        lay = self._lay
        if lay is None:
            return
        now = self._now_tick()
        P = lay['P']
        des = lay['des']
        if keys is None:
            keys = set(des)
            keys.update(self._ev)
        pend = set()
        for off in keys:
            new = des.get(off)
            ev = self._ev.get(off)
            if ev is not None and new is not None and ev[2] == new:
                continue
            if ev is not None:
                tn = ev[4] if ev[3] == 'once' else self._next_fire(ev[1], P, now)
                if tn - now <= P_GUARD and tn >= now:
                    pend.add(off)             # about to fire: leave it until it has
                    continue
            if new is None:
                if ev is None:
                    continue
                self._clear(ev[0])
                self._free.append(ev[0])
                del self._ev[off]
                self._conv.pop(off, None)
                continue
            self._place(off, new, now, lay, lay['B'])
        self._pend = pend

    def _retire(self, b, now):
        """the current loop ends at tick b: hits due before b stay (as
        one-shots, so they cannot repeat if Python stalls), the rest go now"""
        lay = self._lay
        if lay is None:
            return
        P = lay['P']
        for off, ev in self._ev.items():
            tag, absoff, wire, kind, ft = ev
            tn = ft if kind == 'once' else self._next_fire(absoff, P, now)
            if kind == 'once' and tn < now:
                tn = now - 1
            if now <= tn < b:
                if kind == 'loop' and tn - now > P_GUARD:
                    self._clear(tag)
                    self._add(tag, tn, 0, wire)
                    self._old.append((tag, tn + 1))
                elif kind == 'loop':
                    self._old.append((tag, tn + 1))   # fires in a moment; cleared after
                else:
                    self._old.append((tag, tn + 1))
            else:
                self._clear(tag)
                self._free.append(tag)
        self._ev = {}
        self._pend = set()
        self._conv = {}
        self._prev = (lay, b)

    def _start(self, pi, b, k_first=0, lay=None):
        """(re)start the loop with pattern pi so that its step k_first sounds at tick b"""
        if lay is None:
            lay = self.layout(pi)
        if k_first >= lay['N']:
            k_first = 0
        lay['T0'] = (b - lay['starts'][k_first]) % lay['P']
        self._go(pi, lay, b)

    def _go(self, pi, lay, b, starting=True):
        old = self._lay if self._lay is not None else (self._prev[0] if self._prev else None)
        lay['B'] = b
        self._lay = lay
        self.playing_pat = pi
        self._starting = starting
        try:
            self._apply()
        finally:
            self._starting = False
        if old is not None:
            self._dropped(old['chs'] - lay['chs'], b)
        if not lay['des']:
            self._midi_off_at(b)

    def _oneshot(self, tick, wire):
        """a one-off AMY event at tick (its tag is freed after it fired)"""
        tag = self._tag()
        if tag is not None:
            self._add(tag, tick, 0, wire)
            self._old.append((tag, tick + 1))

    def _midi_off_wire(self, chs=None):
        m = ''
        for c in (range(NCH) if chs is None else chs):
            m += 'i%dn%dl0Z' % (MIDI_OUT_SYNTH, self.midi_out_base + c)
        return m

    def _midi_off_at(self, tick, chs=None):
        """pattern-out note-offs at tick (no later hit carries them)"""
        if self._midi_amy and self._midi_cfg:
            if tick is None:
                self._raw(self._midi_off_wire(chs))           # now
            else:
                self._oneshot(tick, self._midi_off_wire(chs))

    def _dropped(self, chs, b):
        """channels without hits in the new loop (muted / empty): their pending
        automatic note-off is gone, so housekeeping sends one once they died
        away; their pattern-out note gets its note-off at the switch"""
        if not chs:
            return
        ms = ticks_ms()
        for c in chs:
            self.voice_on[c] = True
            self.last_hit_ms[c] = ms
        self._midi_off_at(b, sorted(chs))

    def _grid_start(self, pi, anchor, now, lay=None, retire=False, late=True):
        """(re)start pattern pi on the grid whose passes start at tick anchor
        (+ n passes).  late=True (PLAY / MIDI Start): at the step that began a
        moment ago (played a little late) or the next one; late=False (LENGTH /
        RATE / SWING changed while playing): at the first step safely ahead, the
        old loop plays up to there."""
        if lay is None:
            lay = self.layout(pi)
            now = self._now_tick()
        P = lay['P']
        lay['T0'] = anchor % P
        L = lay['L']
        t = now - L // 4 if late else now + P_GUARD + 1
        x = (t - lay['T0']) % P
        base = t - x
        b = base + P
        for st in lay['starts']:
            if st >= x:
                b = base + st
                break
        if retire and self._lay is not None:
            self._retire(b, now)
        self._go(pi, lay, b, late)

    def _old_tick(self, tick):
        if not self._old:
            return
        keep = []
        for tag, after in self._old:
            if tick >= after:
                self._clear(tag)
                self._free.append(tag)
            else:
                keep.append((tag, after))
        self._old = keep

    def _conv_tick(self, tick):
        """one-shot first hits that have played: from now on they repeat"""
        if not self._conv:
            return
        lay = self._lay
        for off, ft in list(self._conv.items()):
            if tick > ft:
                ev = self._ev.get(off)
                del self._conv[off]
                if ev is None or ev[3] != 'once':
                    continue
                tag, absoff, wire = ev[0], ev[1], ev[2]
                tn = self._next_fire(absoff, lay['P'], tick)
                if tn - tick <= P_GUARD:
                    self._conv[off] = ft    # try again in a moment
                    continue
                self._clear(tag)
                self._add(tag, absoff, lay['P'], wire)
                self._ev[off] = [tag, absoff, wire, 'loop', tn]

    def pos(self, tick, lay=None):
        """(pass start tick, step index k) sounding at tick"""
        lay = lay or self._lay
        if lay is None:
            return None
        P = lay['P']
        x = (tick - lay['T0']) % P
        st = lay['starts']
        k = len(st) - 1
        while k > 0 and st[k] > x:
            k -= 1
        return tick - x, k

    def pass_end(self, now):
        """first end of a pattern pass at or after now + guard"""
        lay = self._lay
        P = lay['P']
        pl = lay['n'] * lay['L']              # one pattern pass (P may hold two)
        t = now + P_GUARD + 1
        x = (t - lay['T0']) % P
        base = t - x
        e = 0
        while base + e < t:
            e += pl
        return base + e

    # --- public: transport --------------------------------------------------------
    def set_running(self, on, now=None):
        if on == self.running:
            return
        self.running = on
        if now is None:
            now = self._now_tick()
        if on:
            if self.clock_ext:
                # EXT = arm: the loop waits in AMY on the master's grid (or joins
                # it at once if the master is playing)
                self._ext_begin()
                return
            pi = self.next_pat if self.next_pat is not None else self.cur
            self.next_pat = None
            lay = self.layout(pi)           # (slow in MicroPython: before the clock is read)
            now = self._now_tick()
            L = self.step_ticks()
            g = max(P_GUARD + 2, int(START_MS * self.tempo * 48 / 60000.0) + 1)
            b = ((now + g + L - 1) // L) * L
            if self.clock_out:
                # clock master: Song Position 0, then MIDI Start from a scheduled
                # 'zY1' (AMY sends Start and restarts its clock phase), so the first
                # clock after Start falls on b + comp - our step 1 sounds at b,
                # 'comp' ticks early for the audio buffer (SYNC OFFSET)
                if self.midi_out_fn is not None:
                    try:
                        self.midi_out_fn(bytes([0xF2, 0, 0]))
                    except Exception:
                        pass
                self._oneshot(b + lay['comp'] - 1, 'zY1Z')
            self._start(pi, b, 0, lay)
            return
        # stop
        if self._first_tag is not None:
            self._clear(self._first_tag)
            self._free.append(self._first_tag)
            self._first_tag = None
        if self._lay is not None:
            self._retire(now + P_GUARD + 1, now)
            self._lay = None
        self.midi_all_off()
        self._midi_off_at(now + P_GUARD + 2)  # also after the hits kept above
        self.led_step = -1
        ms = ticks_ms()
        for c in range(NCH):
            self.voice_on[c] = True          # housekeeping sends the note-offs
            self.last_hit_ms[c] = ms

    def select_pattern(self, i):
        """pattern buttons: edit pattern i; while playing it starts at the end of the pass"""
        self.cur = i
        if self.running and self.clock_ext and not self.ext_playing:
            self.next_pat = None
            self._ext_begin()                # armed: re-arm with the new pattern
        elif self.running:
            if i != self.playing_pat:
                self.next_pat = i
            else:
                self.next_pat = None         # stay on the pattern that plays
        else:
            self.playing_pat = i

    def sync_loop(self):
        """patterns / mutes / sounds changed while playing: rewrite what changed"""
        if self._lay is None:
            return
        if self.clock_ext and not self.ext_playing:
            self._ext_begin()                # armed (master stopped): re-arm
            return
        pi = self._lay['pi']
        new = self.layout(pi)
        if new['key'] != self._lay['key'] and self.clock_ext:
            # EXT: the new loop stays anchored to the master's song position
            now = self._now_tick()
            new['T0'] = (self.ext_origin - new['comp']) % new['P']
            t = now + P_GUARD + 1
            x = (t - new['T0']) % new['P']
            b = t - x + new['P']
            for st in new['starts']:
                if st >= x:
                    b = t - x + st
                    break
            self._retire(b, now)
            self._go(pi, new, b, False)
            return
        if new['key'] != self._lay['key']:
            # LENGTH / RATE / SWING changed: carry on at the next step boundary of
            # the same bar grid (the pass start does not move)
            now = self._now_tick()
            base, _ = self.pos(now)
            self._grid_start(pi, base, now, new, retire=True, late=False)
            return
        old = self._lay
        new['T0'] = old['T0']
        new['B'] = old['B']
        self._lay = new
        self._apply()
        gone = old['chs'] - new['chs']
        if gone:
            # muted (or emptied) while playing: pattern-out note-offs now
            self._dropped(gone, None)

    def _next_in_chain(self):
        if self.next_pat is not None:
            return self.next_pat
        ch = chain_of(self.pats, self.playing_pat)
        if len(ch) > 1:
            j = ch.index(self.playing_pat)
            return ch[(j + 1) % len(ch)]
        return None

    # --- sequencer callback ---------------------------------------------------------
    def seq_cb(self, tick):
        now_ms = ticks_ms()
        # the count jumped back (AMY restarted it on MIDI Start); a callback a few
        # ticks older than the last one is just late, not a restart
        restarted = self.last_cb_tick is not None and tick + 24 < self.last_cb_tick
        self.last_cb_tick = tick
        self.last_cb_ms = now_ms
        if self.clock_ext:
            self._ext_tempo(tick, now_ms, restarted)
        if self._first_tag is not None and self.ext_playing and 2 <= tick < 48:
            self._clear(self._first_tag)     # the Start downbeat has played
            self._free.append(self._first_tag)
            self._first_tag = None
        if restarted and self.clock_ext:
            restarted = False                # (MIDI Start: AMY began again; ext_start anchors)
        if restarted:
            # the tick count began again (external MIDI Start): nothing old
            # may fire any more; the MIDI Start handler restarts the loop
            run = self._lay is not None
            pi = self.playing_pat
            self._clear_all()
            self._prev = None
            if run:
                self._grid_start(pi, 1, tick)
            return
        self._old_tick(tick)
        if self._lay is None:
            return
        self._conv_tick(tick)
        lay = self._lay
        # LEDs / hit lights
        if self._prev is not None and tick < self._prev[1]:
            pl = self._prev[0]
            _, k = self.pos(tick, pl)
            self.led_step = k % pl['n']
        else:
            self._prev = None
            if tick >= lay['B']:
                base, k = self.pos(tick)
                self.led_step = k % lay['n']
                if base != self._pass_no:
                    self._pass_no = base
                    if self._rnd_hit:
                        # per-hit random pitch: new dice for the next pass
                        self._rnd_hit = {}
                        self.sync_loop()
                        lay = self._lay
        # pattern switch at the end of the pass (chain / pattern select)
        nxt = self._next_in_chain()
        if nxt is not None and tick >= lay['B']:
            b = self.pass_end(tick)
            lead = max(8, int(self.tempo * 48 / 60.0 * 0.3))
            if b - tick <= lead:
                self._retire(b, tick)
                self.next_pat = None
                self._start(nxt, b, 0)
                return
        if self._pend:
            self._apply(self._pend)

    def _ext_tempo(self, tick, now_ms, restarted):
        """EXT clock: estimate the master's tempo (note-off timing, display)"""
        if restarted or self._bpm_t is None:
            self._bpm_t = (tick, now_ms)
            return
        t0, m0 = self._bpm_t
        if now_ms - m0 >= 1000:
            if tick > t0:
                bpm = (tick - t0) / 48.0 / ((now_ms - m0) / 60000.0)
                if 20.0 <= bpm <= 300.0 and abs(bpm - self.tempo) > 0.5:
                    self.set_tempo(self.tempo + (bpm - self.tempo) * (1.0 if abs(bpm - self.tempo) > 8 else 0.5))
            self._bpm_t = (tick, now_ms)

    # --- external MIDI clock (slave) --------------------------------------------------
    def comp_ticks(self):
        """SYNC OFFSET in ticks.  Everything AMY plays leaves the Tulip ~30 ms
        later (DMA audio buffer), while MIDI clock goes out / comes in at once:
        to sound together with a clocked device the loop fires this much earlier
        than the clock grid.  EXT: whole clocks (AMY counts both ticks of a clock
        at once, 20.8 ms at 120 BPM); master: ticks (10.4 ms at 120 BPM)."""
        if not (self.clock_ext or self.clock_out) or self.sync_ms <= 0:
            return 0
        tick_ms = 60000.0 / (max(self.tempo, 20.0) * 48)
        if self.clock_ext:
            return int(self.sync_ms / (tick_ms * TPC) + 0.5) * TPC
        return int(self.sync_ms / tick_ms + 0.5)

    def resync_offset(self):
        """SYNC OFFSET changed: move the loop against the clock"""
        if not self.running or self._lay is None:
            return
        if self.clock_ext:
            if not self.ext_playing:
                self._ext_begin()
                return
            self.sync_loop()                 # (the layout key holds comp)
            return
        lay = self._lay
        new = self.layout(lay['pi'])
        if new['comp'] == lay['comp']:
            return
        now = self._now_tick()
        base, _ = self.pos(now)
        self._grid_start(lay['pi'], base + lay['comp'] - new['comp'], now, new, retire=True, late=False)

    def ext_start(self, pos16=0, cont=False):
        """MIDI Start (FA) / Continue (FB) / MMC PLAY from the master.
        The loop is anchored to the master's song position: step 1 falls on the
        first clock after Start (tick 1 of AMY's count, which AMY itself restarts
        on Start).  While armed, the loop already sits in AMY before Start: AMY
        plays the downbeat on its own, Python only keeps the display going."""
        c = self._now_tick()
        if cont:
            # Continue: AMY does not restart for it - restart now, the next
            # clock is song position pos16 (in 16ths)
            self._raw('zY1Z')
            c = self._now_tick()
            origin = c + 1 - pos16 * 12
        else:
            h = self._held_at
            restarted = (h is not None and c < h) or c <= 4 * TPC
            if restarted and h is not None and c == h:
                # held at a small count: did AMY restart on this Start (its count
                # moves with the next clock) or is it still held?
                restarted = self._wait_tick_change(c)
            if not restarted:
                # AMY did not restart on this Start (older firmware / channel 11
                # not active): restart it now.  The master's first clock normally
                # follows Start at once, so it is already gone.
                self._raw('zY1Z')
                c = self._now_tick()
                origin = c + 1 - TPC
            else:
                origin = 1
        self._held_at = None
        self.ext_playing = True
        self._bpm_t = None
        self.last_cb_tick = None
        if self.running and self.ext_origin == origin:
            return                           # armed on this grid already: AMY plays it
        self.ext_origin = origin
        self.running = True
        self._ext_begin()

    def ext_stop(self):
        """MIDI Stop (FC) / MMC STOP from the master: AMY's clock stops; the loop
        stays armed for the next Start (which plays from step 1)"""
        self.hold()                          # (AMY may have done so itself)
        self.ext_playing = False
        self.ext_origin = 1
        self.midi_all_off()
        if self.running:
            self._ext_begin()                # same loop, anchored to the next Start
        else:
            self._clear_used()
        ms = ticks_ms()
        for c in range(NCH):
            self.voice_on[c] = True          # housekeeping sends the note-offs
            self.last_hit_ms[c] = ms

    def hold(self):
        """stop AMY's transport (ticks stand still) and remember where"""
        self._raw('zY0Z')
        self._held_at = self._now_tick()

    def _wait_tick_change(self, c):
        try:
            import time
            sl = time.sleep_ms
        except Exception:
            import time
            sl = lambda ms: time.sleep(ms / 1000.0)
        waited = 0
        limit = min(90, int(1.5 * 60000.0 / (max(self.tempo, 20.0) * 24)) + 2)
        while waited < limit:
            sl(2)
            waited += 2
            if self._now_tick() != c:
                return True
        return False

    def _ext_begin(self):
        """arm / play the loop on the master's grid that begins at ext_origin"""
        origin = self.ext_origin if self.ext_origin is not None else 1
        if self.next_pat is not None:
            pi = self.next_pat
            self.next_pat = None
        elif self._lay is not None:
            pi = self.cur if not self.ext_playing else self._lay['pi']
        else:
            pi = self.cur
        lay = self.layout(pi)
        P = lay['P']
        comp = lay['comp']
        lay['T0'] = (origin - comp) % P
        self._clear_used()
        if self._first_tag is not None:
            self._first_tag = None           # (cleared with the rest)
        if self.ext_playing:
            # the master plays: join its grid from the next safe tick
            now = self._now_tick()
            self._go(pi, lay, now + P_GUARD + 1, False)
            return
        # armed (AMY held): every event periodic on the grid, so AMY plays it
        # by itself after Start; the loop sits 'comp' ticks early, so its own
        # step 1 comes only a pass later - the downbeat right after Start gets
        # an extra event on tick 1
        lay['B'] = 0
        for off, wire in lay['des'].items():
            tag = self._tag()
            if tag is None:
                break
            absoff = (lay['T0'] + off) % P
            self._add(tag, absoff, P, wire)
            self._ev[off] = [tag, absoff, wire, 'loop', absoff]
        if comp > 0 and origin == 1 and 0 in lay['des']:
            tag = self._tag()
            if tag is not None:
                self._add(tag, 1 % P, P, lay['des'][0])
                self._first_tag = tag
        self._lay = lay
        self.playing_pat = pi
        self.led_step = -1

    def _clear_used(self):
        """clear only the tags this app has in use (faster than all 96)"""
        for ev in self._ev.values():
            self._clear(ev[0])
        for tag, _ in self._old:
            self._clear(tag)
        if self._first_tag is not None:
            self._clear(self._first_tag)
            self._first_tag = None
        self._lay = None
        self._ev = {}
        self._old = []
        self._pend = set()
        self._conv = {}
        self._prev = None
        self._free = list(range(TAG_LAST, TAG_FIRST - 1, -1))
        self.led_step = -1

    def housekeeping(self):
        """note-off once a channel has died away (AMY stops rendering it): when
        stopped, and while playing for the channels the loop does not play"""
        playing = self.running and (self.ext_playing or not self.clock_ext)
        chs = self._lay['chs'] if (self._lay is not None and playing) else ()
        now = ticks_ms()
        for c in range(NCH):
            if c in chs:
                continue
            if self.voice_on[c] and now - self.last_hit_ms[c] > ring_ms(self.eff(c)) + 200:
                self.voice_on[c] = False
                self.note_off(c)

    # --- cleanup ----------------------------------------------------------------------
    def _clear_all(self):
        for t in range(TAG_FIRST, TAG_LAST + 1):
            self._raw('H0,0,%dZ' % t)
        self._lay = None
        self._ev = {}
        self._old = []
        self._pend = set()
        self._conv = {}
        self._free = list(range(TAG_LAST, TAG_FIRST - 1, -1))
        self.led_step = -1

    def release(self):
        try:
            self._clear_all()
        except Exception:
            pass
        try:
            if self._midi_amy and self._midi_cfg:
                self.midi_all_off()
                self._raw('i%diG%dZ' % (MIDI_OUT_SYNTH, amy.NOTE_OUTPUT_OFF))
        except Exception:
            pass
        for c in range(NCH):
            try:
                self._raw('i%div0Z' % (SYNTH0 + c))
            except Exception:
                pass

    # --- state (presets) --------------------------------------------------------------
    def get_state(self):
        return {
            'v': 1,
            'a': [dict(x) for x in self.a],
            'b': [dict(x) for x in self.b],
            'names': list(self.names),
            'morph': self.morph,
            'master': self.master,
            'pats': [pat_copy(p) for p in self.pats],
            'rate': self.rate, 'swing': self.swing, 'fill_rate': self.fill_rate,
            'tempo': self.tempo, 'cur': self.cur,
        }

    def set_state(self, s, keep_tempo=False):
        def patch(d):
            p = default_patch()
            if isinstance(d, dict):
                for k in p:
                    if k in d:
                        p[k] = d[k]
            return p
        a = s.get('a') or []
        b = s.get('b') or a
        self.a = [patch(a[c] if c < len(a) else None) for c in range(NCH)]
        self.b = [patch(b[c] if c < len(b) else None) for c in range(NCH)]
        nm = s.get('names') or []
        self.names = [str(nm[c]) if c < len(nm) else 'CH %d' % (c + 1) for c in range(NCH)]
        self.morph = clamp(s.get('morph', 0.0))
        self.master = clamp(s.get('master', 1.0), 0.0, MASTER_MAX)
        pats = s.get('pats') or []
        self.pats = []
        for i in range(NPAT):
            if i < len(pats) and isinstance(pats[i], dict):
                q = pats[i]
                p = new_pattern()
                p['len'] = max(1, min(NSTEP, int(q.get('len', 16))))
                for key in ('trig', 'acc', 'fill'):
                    row = list(q.get(key, [0] * NCH))[:NCH]
                    p[key] = [int(x) & 0xFFFF for x in row] + [0] * (NCH - len(row))
                p['chain'] = bool(q.get('chain', False))
                self.pats.append(p)
            else:
                self.pats.append(new_pattern())
        self.rate = int(s.get('rate', 2)) % len(RATES)
        self.swing = clamp(s.get('swing', 0.0))
        self.fill_rate = int(s.get('fill_rate', 0)) % len(FILL_RATES)
        if not keep_tempo and 'tempo' in s:
            self.tempo = clamp(float(s['tempo']), 20.0, 300.0)
        self.cur = int(s.get('cur', 0)) % NPAT
        if not self.running:
            self.playing_pat = self.cur
            self.next_pat = None
        else:
            self.next_pat = self.cur if self.cur != self.playing_pat else None
        self._eff = [None] * NCH
        self.refresh_all()


# ----------------------------------------------------------------------------
# Randomizer / pattern tools (Microtonic "Randomize", "Alter", shift, reverse)
# ----------------------------------------------------------------------------
def random_patch(rnd=random.random):
    """a musically useful random drum patch"""
    p = default_patch()
    kind = rnd()
    p['wave'] = 0 if rnd() < 0.6 else (1 if rnd() < 0.6 else 2)
    p['mod_mode'] = 0 if rnd() < 0.65 else (1 if rnd() < 0.6 else 2)
    if kind < 0.3:          # low drum
        p['osc_freq'] = 0.05 + rnd() * 0.2
        p['mix'] = rnd() * 0.3
        p['mod_amt'] = 0.55 + rnd() * 0.2
        p['mod_rate'] = 0.5 + rnd() * 0.3
        p['osc_dec'] = 0.35 + rnd() * 0.3
    elif kind < 0.6:        # snare / clap / tom
        p['osc_freq'] = 0.2 + rnd() * 0.3
        p['mix'] = 0.4 + rnd() * 0.5
        p['n_mode'] = 1 if rnd() < 0.5 else (0 if rnd() < 0.5 else 2)
        p['n_freq'] = 0.4 + rnd() * 0.45
        p['n_env'] = 2 if rnd() < 0.25 else (1 if rnd() < 0.4 else 0)
        p['n_att'] = rnd() * 0.15 if p['n_env'] == 2 else 0.0
        p['mod_amt'] = 0.5 + rnd() * 0.15
        p['osc_dec'] = 0.2 + rnd() * 0.3
        p['n_dec'] = 0.2 + rnd() * 0.35
    else:                   # metal / hats / perc
        p['osc_freq'] = 0.4 + rnd() * 0.5
        p['mix'] = 0.3 + rnd() * 0.7
        p['n_mode'] = 2 if rnd() < 0.6 else 1
        p['n_freq'] = 0.6 + rnd() * 0.35
        p['n_q'] = 0.1 + rnd() * 0.35
        p['mod_amt'] = rnd()
        p['mod_rate'] = 0.3 + rnd() * 0.6
        p['osc_dec'] = 0.05 + rnd() * 0.35
        p['n_dec'] = 0.05 + rnd() * 0.4
    p['eq_freq'] = rnd()
    p['eq_gain'] = 0.5 + (rnd() - 0.5) * 0.4
    p['dist'] = rnd() * 0.4 if rnd() < 0.4 else 0.0
    p['n_q'] = clamp(p['n_q'] + (rnd() - 0.5) * 0.2)
    p['pan'] = 0.5 + (rnd() - 0.5) * 0.4
    return p


def alter_patch(p, amount=0.08, rnd=random.random):
    q = dict(p)
    for k in KNOB_KEYS:
        if k in ('level',):
            continue
        q[k] = clamp(q[k] + (rnd() * 2.0 - 1.0) * amount)
    return q


def random_pattern(length=16, rnd=random.random):
    """roughly: low drums on the beat, snares on 2 and 4, busy hats"""
    p = new_pattern()
    p['len'] = length
    dens = (0.35, 0.18, 0.55, 0.15, 0.12, 0.12, 0.08, 0.08)
    for c in range(NCH):
        m = 0
        a = 0
        f = 0
        for i in range(length):
            pr = dens[c]
            if c == 0 and i % 4 == 0:
                pr = 0.8
            if c == 1 and i % 8 == 4:
                pr = 0.85
            if c == 2 and i % 2 == 0:
                pr = 0.7
            if rnd() < pr:
                m |= 1 << i
                if rnd() < 0.25:
                    a |= 1 << i
                if rnd() < 0.05:
                    f |= 1 << i
        p['trig'][c] = m
        p['acc'][c] = a & m
        p['fill'][c] = f & m
    return p


def _rot(mask, n, d):
    full = (1 << n) - 1
    low = mask & full
    if d > 0:
        low = ((low << 1) | (low >> (n - 1))) & full
    else:
        low = ((low >> 1) | ((low & 1) << (n - 1))) & full
    return (mask & ~full & 0xFFFF) | low


def _rev(mask, n):
    out = mask & ~((1 << n) - 1) & 0xFFFF
    for i in range(n):
        if mask & (1 << i):
            out |= 1 << (n - 1 - i)
    return out


def pat_shift(p, d, chans=None):
    n = p['len']
    for c in (chans if chans is not None else range(NCH)):
        for key in ('trig', 'acc', 'fill'):
            p[key][c] = _rot(p[key][c], n, d)


def pat_reverse(p, chans=None):
    n = p['len']
    for c in (chans if chans is not None else range(NCH)):
        for key in ('trig', 'acc', 'fill'):
            p[key][c] = _rev(p[key][c], n)


def pat_alter(p, rnd=random.random, chans=None):
    """shuffle a few existing hits around (works on existing material)"""
    n = p['len']
    for c in (chans if chans is not None else range(NCH)):
        hits = [i for i in range(n) if p['trig'][c] & (1 << i)]
        if not hits:
            continue
        for _ in range(1 + len(hits) // 4):
            i = hits[int(rnd() * len(hits)) % len(hits)]
            j = int(rnd() * n) % n
            for key in ('trig', 'acc', 'fill'):
                bi = (p[key][c] >> i) & 1
                bj = (p[key][c] >> j) & 1
                p[key][c] &= ~((1 << i) | (1 << j)) & 0xFFFF
                p[key][c] |= (bj << i) | (bi << j)


# ----------------------------------------------------------------------------
# Factory content (gintonic_presets.py)
# ----------------------------------------------------------------------------
def kit_state(kit, drums):
    """K(...) entry of gintonic_presets.KITS -> engine state"""
    name, k = kit
    a = []
    names = []
    for dn in list(k.get('drums', []))[:NCH]:
        p = default_patch()
        p.update(drums.get(dn, {}))
        a.append(p)
        names.append(dn)
    while len(a) < NCH:
        a.append(default_patch())
        names.append('Init')
    pats = []
    for i in range(NPAT):
        rows = k.get('patterns', {}).get(PAT_NAMES[i])
        if rows:
            n = max(len(r) for r in rows) or 16
            pats.append(pat_from_strings(rows, max(1, min(NSTEP, n))))
        else:
            pats.append(new_pattern())
    ch = k.get('chain', '')
    for j in range(len(ch) - 1):
        i = PAT_NAMES.find(ch[j])
        if 0 <= i < NPAT - 1 and ch[j + 1] == PAT_NAMES[i + 1]:
            pats[i]['chain'] = True
    rate = [r for r, _ in RATES].index(k.get('rate', '1/16')) if k.get('rate', '1/16') in [r for r, _ in RATES] else 2
    fr = int(k.get('fill', 2))
    fi = FILL_RATES.index(fr) if fr in FILL_RATES else 0
    return {'v': 1, 'a': a, 'b': [dict(x) for x in a], 'names': names, 'morph': 0.0, 'master': 1.0,
            'pats': pats, 'rate': rate, 'swing': clamp(k.get('swing', 0) / 100.0), 'fill_rate': fi,
            'tempo': float(k.get('tempo', 120)), 'cur': 0}


DRUMS = [('Init', default_patch())]
KITS = []
try:
    import sys as _sys
    if 'gintonic_presets' in _sys.modules:       # Tulip keeps it after quit: reload edits
        del _sys.modules['gintonic_presets']
    del _sys
    import gintonic_presets as _fp
    DRUMS = [(n, d) for n, d in _fp.DRUMS]
    KITS = list(_fp.KITS)
    del _fp
except ImportError:
    pass
except Exception as _e:
    print('gintonic_presets.py error:', _e)
DRUM_BY_NAME = {n: d for n, d in DRUMS}


# ----------------------------------------------------------------------------
# Microtonic kits (gintonic/kits/*.json, converted from .mtpreset files):
# the values are kept in Microtonic units and mapped onto the knobs here.
# ----------------------------------------------------------------------------
KIT_DIR = 'kits'
_LN10 = math.log(10.0)


def _kf(hz):
    return clamp(math.log(max(hz, 20.0) / 20.0) / math.log(1000.0))


def _kd(ms):
    return clamp(math.log(max(ms, 10.0) / 10.0) / math.log(1000.0))


def _ka(ms):
    return clamp(math.pow(max(ms, 0.0) / 10000.0, 1.0 / 3.0))


def mt_patch(d):
    """Microtonic drum patch (as in a .mtpreset) -> GinTonic knob positions"""
    def g(k, dv):
        v = d.get(k, dv)
        return dv if v is None else v
    p = default_patch()
    p['wave'] = {'Sine': 0, 'Triangle': 1, 'Saw': 2}.get(d.get('OscWave'), 0)
    p['osc_freq'] = _kf(g('OscFreq', 632.46))
    mode = {'Decay': 0, 'Sine': 1, 'Noise': 2}.get(d.get('ModMode'), 0)
    p['mod_mode'] = mode
    st = g('ModAmt', 0.0)
    r = d.get('ModRate', 353.33)
    if mode == 0:
        p['mod_amt'] = clamp(0.5 + st / 192.0)
        if r is None or r > 12000:
            p['mod_rate'] = 0.0
        else:
            p['mod_rate'] = clamp(1.0 - math.log(max(r, 10.0) / 10.0) / math.log(1248.0))
    else:
        p['mod_amt'] = clamp(0.5 + st / 96.0)
        if not r or r <= 0:
            p['mod_rate'] = 0.0
        elif mode == 1:
            p['mod_rate'] = clamp(math.log(max(r, 0.1) / 0.1) / math.log(20000.0))
        else:
            p['mod_rate'] = clamp(math.log(max(r, 1.0)) / math.log(20000.0))
    p['osc_att'] = _ka(g('OscAtk', 0.0))
    p['osc_dec'] = _kd(g('OscDcy', 316.23))
    p['n_mode'] = {'LP': 0, 'BP': 1, 'HP': 2}.get(d.get('NFilMod'), 0)
    p['n_freq'] = _kf(g('NFilFrq', 20000.0))
    q = max(g('NFilQ', 0.707), 0.1)
    p['n_q'] = clamp(math.log(q / 0.1) / _LN10 / 5.0)
    env = {'Exp': 0, 'Linear': 1, 'Mod': 2}.get(d.get('NEnvMod'), 0)
    p['n_env'] = env
    p['n_att'] = _ka(g('NEnvAtk', 0.0))
    nd = g('NEnvDcy', 316.23)
    p['n_dec'] = _kd(nd * 1.5 if env == 1 else nd)
    p['mix'] = clamp(1.0 - g('Mix', 50.0) / 100.0)          # Microtonic: oscillator share in %
    p['eq_freq'] = _kf(g('EQFreq', 632.46))
    p['eq_gain'] = clamp(0.5 + g('EQGain', 0.0) / 80.0)
    p['dist'] = clamp(g('DistAmt', 0.0) / 100.0)
    lv = g('Level', 0.0)
    p['level'] = 0.0 if lv <= -59.0 else clamp(LEVEL_0DB * math.pow(10.0, lv / 40.0))
    p['pan'] = clamp(0.5 + g('Pan', 0.0) / 200.0)
    p['vel_osc'] = clamp(g('OscVel', 0.0) / 200.0)
    p['vel_noise'] = clamp(g('NVel', 0.0) / 200.0)
    p['vel_mod'] = clamp(g('ModVel', 0.0) / 200.0)
    # (Microtonic NStereo: no extra noise chain here, the channel's PAN places it)
    p['choke'] = 1 if d.get('Choke') == 'On' else 0
    return p


def mt_kit_state(k):
    """converted Microtonic preset (dict from kits/*.json) -> engine state"""
    a = []
    names = []
    for d in list(k.get('drums', []))[:NCH]:
        a.append(mt_patch(d))
        names.append(str(d.get('Name', 'Drum'))[:24])
    while len(a) < NCH:
        a.append(default_patch())
        names.append('Init')
    pats = []
    for q in list(k.get('pats', []))[:NPAT]:
        p = new_pattern()
        p['len'] = max(1, min(NSTEP, int(q.get('len', 16))))
        p['chain'] = bool(q.get('chain', False))
        for key, src in (('trig', 't'), ('acc', 'a'), ('fill', 'f')):
            row = list(q.get(src, []))[:NCH]
            p[key] = [int(x) & 0xFFFF for x in row] + [0] * (NCH - len(row))
        pats.append(p)
    while len(pats) < NPAT:
        pats.append(new_pattern())
    pats[NPAT - 1]['chain'] = False
    rl = [r for r, _ in RATES]
    rate = rl.index(k.get('rate')) if k.get('rate') in rl else 2
    fr = int(float(k.get('fill', 2)) + 0.5)
    fr = max(FILL_RATES[0], min(FILL_RATES[-1], fr))
    # MastVol + the loudness trim worked out when the kit was converted
    mv = float(k.get('master', 0.0) or 0.0) + float(k.get('trim_db', 0.0) or 0.0)
    pn = str(k.get('pattern', 'a')).upper()
    cur = PAT_NAMES.find(pn) if (len(pn) == 1 and pn in PAT_NAMES) else 0
    return {'v': 1, 'a': a, 'b': [dict(x) for x in a], 'names': names, 'morph': 0.0,
            'master': clamp(math.pow(10.0, mv / 20.0), 0.0, MASTER_MAX),
            'pats': pats, 'rate': rate, 'swing': clamp(float(k.get('swing', 0.0)) / 100.0),
            'fill_rate': FILL_RATES.index(fr), 'tempo': clamp(float(k.get('tempo', 120.0)), 20.0, 300.0),
            'cur': max(0, cur)}


def list_kit_files(folder):
    """names of the converted Microtonic kits (no file is read)"""
    try:
        import os
        fs = [f[:-5] for f in os.listdir(folder) if f.endswith('.json')]
    except Exception:
        return []
    fs.sort()
    return fs


def load_kit_file(folder, name):
    """-> engine state (kits/*.json hold it ready made, plus 'est', the
    estimated AMY load; a raw Microtonic conversion is mapped here)"""
    import json
    with open('%s/%s.json' % (folder, name)) as f:
        d = json.load(f)
    if 'state' in d:
        st = d['state']
        if 'est' in d:
            st['est'] = d['est']
        return st
    return mt_kit_state(d)


# ----------------------------------------------------------------------------
# Tulip UI (only loaded on Tulip)
#
#   1024 x 600 touchscreen.  Header: pages, kits, preset bank, status.
#   MAIN  : channel pads + mute / solo, drum patch of the selected channel, focus
#           fader (last touched pot), PLAY, patterns, chain, morph + ALTER A/B, master
#   SEQ   : all 8 channels x 16 steps, edit mode, PLAY, patterns, RATE, chain,
#           TEMPO / SWING / FILL rate with -/+
#   MIDI  : channel, note map, clock, CC map + learn, pattern MIDI out
#   TOOLS : randomize / alter / copy / paste / shift / reverse, morph tools
# ----------------------------------------------------------------------------
try:
    import tulip
    import lvgl as lv
    import midi
    import gc
    _ON_TULIP = True
except ImportError:
    _ON_TULIP = False

app = None
eng = None

if _ON_TULIP:
    F_SMALL = lv.font_unscii_8
    F_TINY = lv.font_unscii_8
    F_LABEL = lv.font_montserrat_12
    F_MID = lv.font_montserrat_18
    F_BIG = lv.font_montserrat_24

    # Tulip draws LVGL in RGB332: only colours that survive that are used.
    C_BG = 0x000000
    C_LINE = 0x494955
    C_BTN_EDGE = 0x6d6d55
    C_TRACK = 0x494955
    C_TEXT = 0xffffff
    C_DIM = 0x9292aa
    ACC = 0xff9200           # drum patch accent (amber)
    ACC_DIM = 0x492400
    SEQ_ACC = 0x24b6ff       # pattern / transport (cyan)
    SEQ_DIM = 0x004955
    TRIG_C = 0x24b6ff        # step colours
    ACC_C = 0xffdb00
    FILL_C = 0xff6dff
    STEP_OFF = 0x242424
    STEP_OUT = 0x000024      # beyond the pattern length
    STEP_DIM = 0x244955      # a set step beyond the pattern length
    LED_ON = 0xff2400
    LED_OFF = 0x490000
    PLAY_C = 0x6dff00
    MUTE_C = 0xff2400
    LEARN_C = 0x6dff00
    CHAIN_C = 0x6db6ff

    PAGE_Y = 44
    CELL_H = 118
    KNOB = 64
    KN_W = 76
    SW_W = 70
    KNOB_Y = 18
    DRAG_PX = 260.0
    ARC_W = 3                # pot: value ring width
    CAP = 42                 # pot: cap diameter
    CAP_C = 0x242400         # pot: cap colour / rim
    CAP_EDGE = 0x6d6d55
    PTR_DEG = 5              # pot: pointer half width in degrees
    RADIUS_CIRCLE = 0x7fff
    SOLO_C = 0x6dff00
    # synth sections: (colour, panel tint, dim colour for the focus fader)
    SEC_OSC = (ACC, 0x242400, ACC_DIM)
    SEC_NOISE = (0x24dbaa, 0x002400, 0x004955)
    SEC_MIX = (0xdbdbff, 0x240000, 0x494955)
    SEC_VEL = (0x92b6ff, 0x242455, 0x244955)
    SEC_OF = {}
    for _k in ('wave', 'osc_freq', 'mod_mode', 'mod_amt', 'mod_rate', 'osc_att', 'osc_dec'):
        SEC_OF[_k] = SEC_OSC
    for _k in ('n_mode', 'n_freq', 'n_q', 'n_env', 'n_att', 'n_dec'):
        SEC_OF[_k] = SEC_NOISE
    for _k in ('mix', 'eq_freq', 'eq_gain', 'dist', 'level', 'pan', 'choke'):
        SEC_OF[_k] = SEC_MIX
    for _k in ('vel_osc', 'vel_noise', 'vel_mod'):
        SEC_OF[_k] = SEC_VEL
    TICK_MS = 25
    REFRESH_MS = 35
    VIS_MS = 80
    KNOBS_PER_TICK = 5
    GC_MS = 2000
    SETTINGS = 'gintonic_settings.json'

    MIDI_CH_VALS = [0] + list(range(1, 17)) + [-1]
    MIDI_CH_OPTS = ['MIDI OMNI'] + ['MIDI CH %d' % c for c in range(1, 17)] + ['MIDI OFF']
    NOTE_TRIG = 36           # C1..G1: play channel 1..8
    NOTE_MUTE = 48           # C2..G2: mute channel 1..8 while held
    NOTE_PAT = 60            # C3..B3: pattern A..L

    def _c(h):
        return lv.color_hex(h)

    def _plain(o, bg=None, radius=0):
        o.set_style_pad_all(0, 0)
        o.set_style_border_width(0, 0)
        o.set_style_radius(radius, 0)
        o.set_style_shadow_width(0, 0)
        if bg is None:
            o.set_style_bg_opa(lv.OPA.TRANSP, 0)
        else:
            o.set_style_bg_opa(lv.OPA.COVER, 0)
            o.set_style_bg_color(_c(bg), 0)
        o.remove_flag(lv.obj.FLAG.SCROLLABLE)
        return o

    def _label(parent, text, font, color, x, y, w=None, center=False):
        l = lv.label(parent)
        l.set_text(text)
        l.set_style_text_font(font, 0)
        l.set_style_text_color(_c(color), 0)
        if w is not None:
            l.set_width(w)
        if center:
            l.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
        l.set_pos(x, y)
        return l

    def _line(parent, x, y, w, h=1):
        o = _plain(lv.obj(parent), C_LINE)
        o.set_size(w, h)
        o.set_pos(x, y)
        return o

    def _clip(lbl):
        """one-line label cut at its width"""
        try:
            lm = getattr(lv.label, 'LONG_MODE', None) or lv.label.LONG
            lbl.set_long_mode(lm.CLIP)
        except Exception:
            pass

    def _dropdown(parent, opts, x, y, w, h=30, accent=ACC, title=''):
        """a button that opens a full screen list (PickBtn: the lv.dropdown calls the
        app uses: set_options / get_selected / set_selected / set_text / add_event_cb)"""
        return PickBtn(parent, opts, x, y, w, h, accent, title)

    class PickBtn:
        def __init__(self, parent, opts, x, y, w, h, accent, title):
            self.accent = accent
            self.title = title
            self.opts = []
            self.sel = 0
            self.fixed = None
            self.cbs = []
            self.btn = Btn(parent, '', x, y, w, h, accent, self.open, font=F_LABEL)
            self.btn.b.set_style_border_color(_c(C_BTN_EDGE), 0)
            self.btn.l.set_style_text_color(_c(C_TEXT), 0)
            try:
                self.btn.l.set_width(w - 10)
                self.btn.l.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
                self.btn.l.center()
                _clip(self.btn.l)
            except Exception:
                pass
            self.set_options(opts)

        def _show(self):
            if self.fixed is not None:
                t = self.fixed
            elif self.opts:
                t = self.opts[self.sel]
            else:
                t = ''
            self.btn.text(t)

        def set_options(self, opts):
            self.opts = opts.split('\n') if opts else []
            if self.sel >= len(self.opts):
                self.sel = 0
            self._show()

        def get_selected(self):
            return self.sel

        def set_selected(self, i, *a):
            if 0 <= i < len(self.opts):
                self.sel = i
                self._show()

        def set_text(self, t):
            self.fixed = t
            self._show()

        def add_event_cb(self, cb, ev, data):
            self.cbs.append(cb)

        def open(self):
            t = self.title() if callable(self.title) else self.title
            open_picker(t, self.opts, self.sel, self.pick, self.accent)

        def pick(self, i):
            self.sel = i
            self._show()
            for cb in self.cbs:
                cb(None)

    # --- full screen list picker -----------------------------------------------------------
    PICK_COLS = 4
    PICK_ROWS = 9

    def open_picker(title, opts, sel, on_pick, accent=ACC):
        if getattr(app, 'picker', None) is not None or not opts:
            return
        hdr = app.header
        dlg = getattr(hdr, 'dlg', None)
        for pg in app.pages:
            pg.group.add_flag(lv.obj.FLAG.HIDDEN)
        hdr.group.add_flag(lv.obj.FLAG.HIDDEN)
        if dlg is not None:
            dlg.add_flag(lv.obj.FLAG.HIDDEN)
        d = _plain(lv.obj(app.group), C_BG)
        d.set_size(1024, 600)
        d.set_pos(0, 0)
        d.add_flag(lv.obj.FLAG.CLICKABLE)
        pk = {'d': d, 'opts': opts, 'sel': sel, 'cb': on_pick, 'dlg': dlg, 'page': 0, 'btns': []}
        app.picker = pk
        per = PICK_COLS * PICK_ROWS
        pk['pages'] = (len(opts) + per - 1) // per
        pk['page'] = min(max(sel, 0) // per, pk['pages'] - 1)
        _label(d, title, F_BIG, accent, 12, 14, w=560)
        pk['pg_lbl'] = _label(d, '', F_MID, C_DIM, 590, 18, w=90)
        pk['prev'] = Btn(d, '<', 680, 6, 70, 48, accent, lambda: _picker_page(-1), font=F_BIG)
        pk['next'] = Btn(d, '>', 756, 6, 70, 48, accent, lambda: _picker_page(1), font=F_BIG)
        Btn(d, 'CANCEL', 846, 6, 170, 48, ACC, close_picker, font=F_MID)
        bw = (1024 - 8 - (PICK_COLS - 1) * 6) // PICK_COLS
        bh = 54
        for j in range(per):
            r, c = j // PICK_COLS, j % PICK_COLS
            b = Btn(d, '', 4 + c * (bw + 6), 64 + r * (bh + 4), bw, bh, accent,
                    lambda j=j: _picker_tap(j), font=F_LABEL)
            try:
                b.l.set_width(bw - 10)
                b.l.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
                b.l.center()
                _clip(b.l)
            except Exception:
                pass
            pk['btns'].append(b)
        _picker_fill()

    def _picker_fill():
        pk = app.picker
        per = PICK_COLS * PICK_ROWS
        base = pk['page'] * per
        for j, b in enumerate(pk['btns']):
            i = base + j
            if i < len(pk['opts']):
                b.b.remove_flag(lv.obj.FLAG.HIDDEN)
                b.text(pk['opts'][i])
                b.set(i == pk['sel'])
                if i != pk['sel']:
                    b.l.set_style_text_color(_c(C_TEXT), 0)
            else:
                b.b.add_flag(lv.obj.FLAG.HIDDEN)
        n = pk['pages']
        pk['pg_lbl'].set_text('%d / %d' % (pk['page'] + 1, n) if n > 1 else '')
        for k in ('prev', 'next'):
            if n > 1:
                pk[k].b.remove_flag(lv.obj.FLAG.HIDDEN)
            else:
                pk[k].b.add_flag(lv.obj.FLAG.HIDDEN)

    def _picker_page(d):
        pk = app.picker
        if pk is None:
            return
        pk['page'] = (pk['page'] + d) % pk['pages']
        _picker_fill()

    def _picker_tap(j):
        pk = app.picker
        if pk is None:
            return
        i = pk['page'] * PICK_COLS * PICK_ROWS + j
        if i >= len(pk['opts']):
            return
        cb = pk['cb']
        close_picker()
        cb(i)

    def close_picker():
        pk = getattr(app, 'picker', None)
        if pk is None:
            return
        app.picker = None
        try:
            lv.indev_active().wait_release()   # the touch must not land on the page below
        except Exception:
            pass
        pk['d'].delete()
        if pk['dlg'] is not None and app.header.dlg is pk['dlg']:
            pk['dlg'].remove_flag(lv.obj.FLAG.HIDDEN)      # back to the dialog
        else:
            app.header.group.remove_flag(lv.obj.FLAG.HIDDEN)
            app.show_page(app.page)

    class Btn:
        """Flat latching-style button."""

        def __init__(self, parent, text, x, y, w, h, accent, cb, font=None):
            self.accent = accent
            self.b = _plain(lv.button(parent), 0x000000)
            self.b.set_style_border_width(1, 0)
            self.b.set_size(w, h)
            self.b.set_pos(x, y)
            self.l = lv.label(self.b)
            self.l.set_text(text)
            self.l.set_style_text_font(font or F_LABEL, 0)
            self.l.center()
            self.b.add_event_cb(lambda e: cb(), lv.EVENT.PRESSED, None)
            self.on = None
            self.set(False)

        def set(self, on):
            if on == self.on:
                return
            self.on = on
            self.b.set_style_bg_color(_c(self.accent if on else 0x000000), 0)
            self.b.set_style_border_color(_c(self.accent if on else C_BTN_EDGE), 0)
            self.l.set_style_text_color(_c(0x000000 if on else C_DIM), 0)

        def text(self, t):
            self.l.set_text(t)

    # --- parameter access ------------------------------------------------------
    #   'k:<param>'  = drum patch knob of the selected channel
    #   'c<n>:<param>' = drum patch knob of channel n (MIDI CC map)
    #   'g:<name>'   = global: tempo, swing, fill, master, morph
    G_KEYS = ('g:tempo', 'g:swing', 'g:fill', 'g:master', 'g:morph')
    G_LABEL = {'g:tempo': 'TEMPO', 'g:swing': 'SWING', 'g:fill': 'FILL RATE',
               'g:master': 'MASTER', 'g:morph': 'MORPH'}

    def _split(key):
        if key.startswith('k:'):
            return app.sel, key[2:]
        if key[0] == 'c' and ':' in key:
            i = key.index(':')
            return int(key[1:i]), key[i + 1:]
        return None, key

    def tempo_k(bpm):
        return clamp(math.log(bpm / 20.0) / math.log(15.0))

    def k_tempo(k):
        return 20.0 * math.pow(15.0, clamp(k))

    def get_val(key):
        c, k = _split(key)
        if c is not None:
            return eng.eff(c)[k]
        if key == 'g:tempo':
            return tempo_k(eng.tempo)
        if key == 'g:swing':
            return eng.swing
        if key == 'g:fill':
            return eng.fill_rate / float(len(FILL_RATES) - 1)
        if key == 'g:master':
            return LEVEL_0DB * math.sqrt(eng.master)
        if key == 'g:morph':
            return eng.morph
        return 0.0

    def set_val(key, v):
        v = clamp(v)
        c, k = _split(key)
        if c is not None:
            eng.set_knob(c, k, v)
            app.dirty_ch[c] = 1
            _edited()
            return
        if key == 'g:tempo':
            if eng.clock_ext:
                return                       # the master sets the tempo
            apply_tempo(k_tempo(v))
        elif key == 'g:swing':
            eng.swing = v
            app.pat_dirty = True
        elif key == 'g:fill':
            eng.fill_rate = int(v * (len(FILL_RATES) - 1) + 0.5)
            app.pat_dirty = True
        elif key == 'g:master':
            eng.master = (v / LEVEL_0DB) ** 2
            for i in range(NCH):
                app.dirty_ch[i] = 1
        elif key == 'g:morph':
            eng.set_morph(v)
            for i in range(NCH):
                app.dirty_ch[i] = 1
            app.knobs_dirty = True
        _edited()

    def val_text(key):
        c, k = _split(key)
        if c is not None:
            return fmt(k, eng.eff(c))
        if key == 'g:tempo':
            return ('EXT %d' if eng.clock_ext else '%d BPM') % int(eng.tempo + 0.5)
        if key == 'g:swing':
            return '%d%%' % int(eng.swing * 100 + 0.5)
        if key == 'g:fill':
            return '%dx' % FILL_RATES[eng.fill_rate]
        if key == 'g:master':
            if eng.master < 0.0001:
                return '-inf'
            return '%+.1fdB' % (20.0 * math.log(eng.master) / math.log(10.0))
        if key == 'g:morph':
            return 'A %d B' % int(eng.morph * 100 + 0.5)
        return ''

    # short pot titles (the focus fader shows the full name)
    KNOB_TITLE = {'osc_freq': 'FREQ', 'osc_att': 'ATTACK', 'osc_dec': 'DECAY',
                  'n_freq': 'FREQ', 'n_q': 'Q', 'n_att': 'ATTACK', 'n_dec': 'DECAY',
                  'vel_noise': 'VEL NOIS', 'g:fill': 'FILL'}
    KNOB_DEFAULT = {k: v for k, _, v in KNOBS}

    def knob_title(key):
        c, k = _split(key)
        if c is not None and key.startswith('k:'):
            return KNOB_TITLE.get(k, KNOB_LABEL.get(k, k))
        return KNOB_TITLE.get(key, key_label(key))

    def key_default(key):
        c, k = _split(key)
        if c is not None:
            return KNOB_DEFAULT.get(k, 0.0)
        if key == 'g:tempo':
            return tempo_k(120.0)
        if key == 'g:master':
            return LEVEL_0DB
        return 0.0

    def _knob_dirty(key):
        kn = app.knob_by_key.get(key)
        if kn is not None:
            app.dirty_knobs[key] = kn

    def nudge(key, d):
        """focus fader -/+ : one fine step (tempo: 1 BPM, fill rate: one rate)"""
        if key == 'g:tempo':
            if not eng.clock_ext:
                apply_tempo(max(20.0, min(300.0, int(eng.tempo + 0.5) + d)))
                _edited()
        else:
            if key == 'g:fill':
                step = 1.0 / (len(FILL_RATES) - 1)
            elif key == 'g:swing':
                step = 0.01
            else:
                step = 0.002
            v = get_val(key) + d * step
            if key == 'g:swing':
                v = int(v * 100 + 0.5) / 100.0          # whole percents
            set_val(key, v)
        _knob_dirty(key)

    def set_focus(key):
        old = getattr(app, 'focus_key', None)
        if old == key:
            return
        app.focus_key = key
        for k in (old, key):
            kn = app.knob_by_key.get(k) if k is not None else None
            if kn is not None:
                kn.set_focus(k == key)

    def key_label(key):
        c, k = _split(key)
        if c is not None:
            lab = KNOB_LABEL.get(k, k)
            if not key.startswith('k:'):
                lab = '%d %s' % (c + 1, lab)
            return lab
        return G_LABEL.get(key, key)

    def apply_tempo(bpm=None):
        if bpm is not None:
            eng.set_tempo(bpm)
        if eng.clock_ext:
            return
        try:
            if abs(tulip.seq_bpm() - eng.tempo) > 0.05:
                tulip.seq_bpm(eng.tempo)
        except Exception:
            pass

    def _touch_y(cx, cy):
        p = _touch_xy(cx, cy)
        return None if p is None else p[1]

    def _touch_x(cx, cy):
        p = _touch_xy(cx, cy)
        return None if p is None else p[0]

    def _touch_xy(cx, cy):
        """the touch point nearest to (cx, cy), screen coordinates"""
        try:
            t = tulip.touch()
        except Exception:
            return None
        best = None
        bd = 1e9
        for k in (0, 2, 4):
            if k + 1 >= len(t):
                break
            tx, ty = t[k], t[k + 1]
            if tx == 0 and ty == 0:
                continue
            d = abs(tx - cx) + abs(ty - cy)
            if d < bd:
                bd = d
                best = (tx, ty)
        return best

    class Knob:
        """Rotary pot: touch anywhere in the cell and drag up/down (relative)."""

        def __init__(self, parent, key, x, y, ax, ay, accent=ACC, w=KN_W, bipolar=None, top=0, big=False):
            self.key = key
            self.accent = accent
            self.cell = _plain(lv.obj(parent))
            self.cell.set_size(w, CELL_H)
            self.cell.set_pos(x, y)
            self.cell.add_flag(lv.obj.FLAG.CLICKABLE)
            self.title = _label(self.cell, knob_title(key), F_TINY, C_TEXT, 0, top + 4, w=w, center=True)
            kx = (w - KNOB) // 2
            a = lv.arc(self.cell)
            self.arc = a
            a.set_size(KNOB, KNOB)
            a.set_pos(kx, top + KNOB_Y)
            a.set_bg_angles(135, 45)
            a.set_range(0, 100)
            # thin value ring (the arc's own knob is hidden) ...
            a.set_style_arc_width(ARC_W, lv.PART.MAIN)
            a.set_style_arc_width(ARC_W, lv.PART.INDICATOR)
            a.set_style_arc_rounded(False, lv.PART.MAIN)
            a.set_style_arc_rounded(False, lv.PART.INDICATOR)
            a.set_style_arc_color(_c(C_TRACK), lv.PART.MAIN)
            a.set_style_arc_color(_c(accent), lv.PART.INDICATOR)
            a.set_style_bg_opa(lv.OPA.TRANSP, lv.PART.KNOB)
            a.set_style_pad_all(0, lv.PART.KNOB)
            a.set_style_shadow_width(0, lv.PART.KNOB)
            a.set_style_pad_all(0, lv.PART.MAIN)
            a.set_style_bg_opa(lv.OPA.TRANSP, lv.PART.MAIN)
            a.remove_flag(lv.obj.FLAG.CLICKABLE)
            # ... a dark cap inside it ...
            cap = _plain(lv.obj(self.cell), CAP_C, RADIUS_CIRCLE)
            cap.set_size(CAP, CAP)
            cap.set_pos((w - CAP) // 2, top + KNOB_Y + (KNOB - CAP) // 2)
            cap.set_style_border_width(1, 0)
            cap.set_style_border_color(_c(CAP_EDGE), 0)
            cap.remove_flag(lv.obj.FLAG.CLICKABLE)
            # ... and a white pointer: a narrow wedge of a second arc, centre to rim
            pr = lv.arc(self.cell)
            ps = CAP - 6
            pr.set_size(ps, ps)
            pr.set_pos((w - ps) // 2, top + KNOB_Y + (KNOB - ps) // 2)
            pr.set_bg_angles(0, 360)
            pr.set_style_arc_opa(lv.OPA.TRANSP, lv.PART.MAIN)
            pr.set_style_arc_width(ps // 2 - 5, lv.PART.INDICATOR)
            pr.set_style_arc_rounded(False, lv.PART.INDICATOR)
            pr.set_style_arc_color(_c(C_TEXT), lv.PART.INDICATOR)
            pr.set_style_bg_opa(lv.OPA.TRANSP, lv.PART.KNOB)
            pr.set_style_pad_all(0, lv.PART.KNOB)
            pr.set_style_pad_all(0, lv.PART.MAIN)
            pr.set_style_bg_opa(lv.OPA.TRANSP, lv.PART.MAIN)
            pr.remove_flag(lv.obj.FLAG.CLICKABLE)
            self.ptr = pr
            if bipolar is None:
                bipolar = key[2:] in BIPOLAR
            if bipolar:
                try:
                    a.set_mode(lv.arc.MODE.SYMMETRICAL)
                except Exception:
                    try:
                        a.set_mode(lv.ARC_MODE.SYMMETRICAL)
                    except Exception:
                        pass
            if big:      # important value: large, in the accent colour
                self.val = _label(self.cell, '', F_MID, accent, 0, top + KNOB_Y + KNOB + 2, w=w, center=True)
            else:
                self.val = _label(self.cell, '', F_SMALL, C_DIM, 0, top + KNOB_Y + KNOB + 6, w=w, center=True)
            self.cx = ax + w // 2
            self.cy = ay + top + KNOB_Y + KNOB // 2
            self.y0 = None
            self.v0 = 0
            self.cell.add_event_cb(self.press_cb, lv.EVENT.PRESSED, None)
            self.cell.add_event_cb(self.drag_cb, lv.EVENT.PRESSING, None)
            self.cell.add_event_cb(self.release_cb, lv.EVENT.RELEASED, None)
            self.shown = None
            self.shown_txt = None
            self.learn_mark = False
            self.focus_mark = False
            self.refresh()

        def refresh(self):
            v = get_val(self.key)
            iv = int(v * 100 + 0.5)
            if iv != self.shown:
                self.shown = iv
                self.arc.set_value(iv)
                av = 135 + 270 * iv // 100
                self.ptr.set_angles((av - PTR_DEG) % 360, (av + PTR_DEG) % 360)
            t = val_text(self.key)
            if t != self.shown_txt:
                self.shown_txt = t
                self.val.set_text(t)

        def set_learn(self, on):
            if on != self.learn_mark:
                self.learn_mark = on
                self.title.set_style_text_color(_c(LEARN_C if on else C_TEXT), 0)

        def set_focus(self, on):
            if on != self.focus_mark:
                self.focus_mark = on
                if on:
                    self.cell.set_style_border_color(_c(self.accent), 0)
                    self.cell.set_style_border_width(2, 0)
                    self.cell.set_style_radius(6, 0)
                else:
                    self.cell.set_style_border_width(0, 0)

        def press_cb(self, e):
            self.y0 = _touch_y(self.cx, self.cy)
            self.v0 = get_val(self.key)
            set_focus(self.key)
            if app.learn_on:
                app.set_learn_target(self.key)

        def drag_cb(self, e):
            if self.y0 is None:
                self.y0 = _touch_y(self.cx, self.cy)
                self.v0 = get_val(self.key)
                return
            y = _touch_y(self.cx, self.cy)
            if y is None:
                return
            v = clamp(self.v0 + (self.y0 - y) / DRAG_PX)
            if abs(v - get_val(self.key)) > 0.0005:
                set_val(self.key, v)
                app.dirty_knobs[self.key] = self

        def release_cb(self, e):
            self.y0 = None

    class Switch:
        """A labelled group of latching buttons (a drum patch switch of the selected channel)."""

        def __init__(self, parent, key, title, x, y, opts, w=SW_W, accent=ACC, top=0):
            self.key = key
            _label(parent, title, F_TINY, C_TEXT, x, y + top + 3, w=w, center=True)
            n = len(opts)
            if top:
                bh = 24 if n == 3 else 32
                gap = 4 if n == 3 else 8
                top = y + top + 18 if n == 3 else y + top + 22
            else:
                bh = 26 if n == 3 else 34
                gap = 5 if n == 3 else 10
                top = y + 20 if n == 3 else y + 24
            self.btns = []
            for j, (val, text) in enumerate(opts):
                b = Btn(parent, text, x + 6, top + j * (bh + gap), w - 12, bh, accent,
                        lambda val=val: self.pick(val), font=F_TINY)
                self.btns.append((val, b))
            self.refresh()

        def pick(self, val):
            eng.set_switch(app.sel, self.key, val)
            app.dirty_ch[app.sel] = 1
            if self.key in ('mod_mode', 'n_env'):
                app.knobs_dirty = True          # MOD RATE / NOISE DEC units change
            _edited()
            self.refresh()

        def refresh(self):
            cur = eng.eff(app.sel)[self.key]
            for val, b in self.btns:
                b.set(val == cur)

    # --- the channel strip (MAIN) ----------------------------------------------------
    def _ch_name(c):
        return '%d %s' % (c + 1, _ascii(eng.names[c])[:9])

    def select_channel(c, play=True):
        """Microtonic: tap a channel to select it, tap the selected one to hear it"""
        if c == app.sel:
            if play:
                eng.trigger(c, 0.5)
                app.flash[c] = ticks_ms()
            return
        app.sel = c
        app.refresh_channel()

    def toggle_mute(c, solo=False):
        if solo:
            others = [i for i in range(NCH) if i != c]
            if all(eng.mute[i] for i in others) and not eng.mute[c]:
                for i in range(NCH):
                    eng.mute[i] = False
            else:
                for i in range(NCH):
                    eng.mute[i] = i != c
        else:
            eng.mute[c] = not eng.mute[c]
        app.pat_dirty = True
        app.strip_dirty = True

    def is_solo(c):
        return not eng.mute[c] and all(eng.mute[i] for i in range(NCH) if i != c)

    class ChannelStrip:
        """MAIN: 8 big channel pads (tap = select, tap the selected one = play it) and,
        apart from them, a row of M (mute) and S (solo) buttons"""

        def __init__(self, g, y, h=52, mh=32):
            self.sel_b = []
            self.mute_b = []
            self.solo_b = []
            self.leds = []
            w = 128
            for c in range(NCH):
                x = c * w
                b = Btn(g, _ch_name(c), x + 3, y, w - 6, h, ACC, lambda c=c: select_channel(c), font=F_LABEL)
                led = _plain(lv.obj(g), LED_OFF, 5)
                led.set_size(10, 10)
                led.set_pos(x + 9, y + 6)
                led.remove_flag(lv.obj.FLAG.CLICKABLE)
                hw = (w - 14) // 2
                m = Btn(g, 'M', x + 3, y + h + 8, hw, mh, MUTE_C, lambda c=c: toggle_mute(c), font=F_LABEL)
                so = Btn(g, 'S', x + 11 + hw, y + h + 8, hw, mh, SOLO_C, lambda c=c: toggle_mute(c, solo=True),
                         font=F_LABEL)
                self.sel_b.append(b)
                self.mute_b.append(m)
                self.solo_b.append(so)
                self.leds.append(led)
            self.led_on = [False] * NCH
            self.names_shown = [None] * NCH

        def refresh(self):
            for c in range(NCH):
                self.sel_b[c].set(c == app.sel)
                self.mute_b[c].set(eng.mute[c] or eng.hold_mute[c])
                self.solo_b[c].set(is_solo(c))
                n = _ch_name(c)
                if n != self.names_shown[c]:
                    self.names_shown[c] = n
                    self.sel_b[c].text(n)

        def tick(self, now):
            for c in range(NCH):
                on = now - app.flash[c] < 90
                if on != self.led_on[c]:
                    self.led_on[c] = on
                    self.leds[c].set_style_bg_color(_c(PLAY_C if on else LED_OFF), 0)

    class FatBar:
        """wide touch fader: drag left / right anywhere on it (relative, so a touch
        never makes the value jump); value 0..1 via get() / set()"""

        def __init__(self, g, x, y, w, h, ax, ay, color, dim, on_change, thumb=28):
            self.w = w
            self.thumb = thumb
            self.cb = on_change
            self.box = _plain(lv.obj(g), STEP_OFF, 4)
            self.box.set_size(w, h)
            self.box.set_pos(x, y)
            self.box.add_flag(lv.obj.FLAG.CLICKABLE)
            self.box.set_ext_click_area(8)
            self.fill = _plain(lv.obj(self.box), dim)
            self.fill.set_size(1, h)
            self.fill.set_pos(0, 0)
            self.fill.remove_flag(lv.obj.FLAG.CLICKABLE)
            self.knob = _plain(lv.obj(self.box), color, 4)
            self.knob.set_size(thumb, h - 8)
            self.knob.set_pos(0, 4)
            self.knob.remove_flag(lv.obj.FLAG.CLICKABLE)
            self.cx = ax + w // 2
            self.cy = ay + h // 2
            self.x0 = None
            self.v0 = 0.0
            self.v = -1.0
            self.get = None
            self.key = None
            self.box.add_event_cb(self.press_cb, lv.EVENT.PRESSED, None)
            self.box.add_event_cb(self.drag_cb, lv.EVENT.PRESSING, None)
            self.box.add_event_cb(self.release_cb, lv.EVENT.RELEASED, None)

        def set_color(self, color, dim):
            self.knob.set_style_bg_color(_c(color), 0)
            self.fill.set_style_bg_color(_c(dim), 0)

        def show(self, v):
            v = clamp(v)
            px = int(v * (self.w - self.thumb) + 0.5)
            if px != int(self.v * (self.w - self.thumb) + 0.5) or self.v < 0:
                self.fill.set_width(max(1, px + self.thumb // 2))
                self.knob.set_x(px)
            self.v = v

        def press_cb(self, e):
            self.x0 = _touch_x(self.cx, self.cy)
            self.v0 = self.get() if self.get else self.v
            if self.key is not None:          # MORPH: focus fader + MIDI CC learn
                set_focus(self.key)
                if app.learn_on:
                    app.set_learn_target(self.key)

        def drag_cb(self, e):
            if self.x0 is None:
                self.press_cb(e)
                return
            x = _touch_x(self.cx, self.cy)
            if x is None:
                return
            v = clamp(self.v0 + (x - self.x0) / float(self.w - self.thumb))
            if abs(v - self.v) > 0.0005:
                self.show(v)
                self.cb(v)

        def release_cb(self, e):
            self.x0 = None

    class FocusFader:
        """MAIN: the last touched pot, big: name + value, a wide fader,
        -/+ fine steps (hold to repeat) and DEFAULT"""

        def __init__(self, g, y, h=56):
            self.key = None
            self.name = _label(g, '', F_SMALL, C_DIM, 8, y + 6, w=150)
            self.val = _label(g, '', F_MID, ACC, 8, y + 24, w=150)
            bw = 52
            self._btn(g, '-', 166, y + 2, bw, h - 4, -1)
            x1 = 1016 - 76 - 8 - bw
            self._btn(g, '+', x1, y + 2, bw, h - 4, 1)
            bx = 166 + bw + 8
            self.bar = FatBar(g, bx, y + 2, x1 - 8 - bx, h - 4, bx, PAGE_Y + y + 2, ACC, ACC_DIM, self.moved)
            self.bar.get = lambda: get_val(self.key) if self.key else 0.0
            Btn(g, 'DEFAULT', 1016 - 76, y + 2, 76, h - 4, C_DIM, self.default, font=F_LABEL)
            self.shown = None
            self.shown_txt = None

        def _btn(self, g, t, x, y, w, h, d):
            b = Btn(g, t, x, y, w, h, ACC, lambda: self.step(d), font=F_BIG)
            ev = getattr(lv.EVENT, 'LONG_PRESSED_REPEAT', None)
            if ev is not None:
                b.b.add_event_cb(lambda e: self.step(d), ev, None)

        def step(self, d):
            if self.key:
                nudge(self.key, d)
                self.shown = None

        def default(self):
            if self.key:
                if self.key == 'g:tempo':
                    if not eng.clock_ext:
                        apply_tempo(120.0)
                        _edited()
                else:
                    set_val(self.key, key_default(self.key))
                _knob_dirty(self.key)
                self.shown = None

        def moved(self, v):
            if self.key:
                set_val(self.key, v)
                _knob_dirty(self.key)

        def tick(self):
            key = getattr(app, 'focus_key', None)
            if key is None:
                return
            if key != self.key:
                self.key = key
                c, k = _split(key)
                nm = KNOB_LABEL.get(k, k) if c is not None else key_label(key)
                if c is not None:
                    nm = 'CH%d  %s' % (app.sel + 1, nm)
                self.name.set_text(nm)
                if c is not None:
                    col, _, dim = SEC_OF.get(k, SEC_OSC)
                elif key in ('g:master', 'g:morph'):
                    col, dim = ACC, ACC_DIM
                else:
                    col, dim = SEQ_ACC, SEQ_DIM
                self.val.set_style_text_color(_c(col), 0)
                self.bar.set_color(col, dim)
                self.shown = None
                self.sel_shown = app.sel
            if getattr(self, 'sel_shown', None) != app.sel:
                self.key = None          # channel changed: retitle next tick
                return
            v = get_val(key)
            iv = int(v * 2000 + 0.5)
            if iv != self.shown and self.bar.x0 is None:
                self.bar.show(v)
            self.shown = iv
            t = val_text(key)
            if t != self.shown_txt:
                self.shown_txt = t
                self.val.set_text(t)

    # --- pattern bar (MAIN + MATRIX) ----------------------------------------------------
    class PatternBar:
        """A-L buttons (selected = filled, playing = green frame, chained = blue
        text, empty = dim), CHAIN >, PLAY / STOP."""

        def __init__(self, g, x, y, cols, bw, bh, gap, chain, play):
            """chain / play: (x, y, w, h) of the CHAIN > and PLAY buttons"""
            self.btns = []
            for i in range(NPAT):
                b = _plain(lv.button(g), 0x000000)
                b.set_style_border_width(2, 0)
                b.set_size(bw, bh)
                b.set_pos(x + (i % cols) * (bw + gap), y + (i // cols) * (bh + gap))
                l = lv.label(b)
                l.set_text(PAT_NAMES[i])
                l.set_style_text_font(F_MID, 0)
                l.center()
                b.add_event_cb(lambda e, i=i: self.tap(i), lv.EVENT.PRESSED, None)
                self.btns.append((b, l))
            cx, cy, cw, ch = chain
            self.chain_b = Btn(g, 'CHAIN >', cx, cy, cw, ch, CHAIN_C, self.chain, font=F_LABEL)
            px, py, pw, ph = play
            self.play_b = Btn(g, 'PLAY', px, py, pw, ph, PLAY_C, self.play, font=F_BIG)
            self.shown = None

        def tap(self, i):
            eng.select_pattern(i)
            app.grid_dirty = True
            _edited()

        def chain(self):
            p = eng.pats[eng.cur]
            if eng.cur < NPAT - 1:
                p['chain'] = not p['chain']
                app.pat_dirty = True
                _edited()

        def play(self):
            transport(not eng.running)

        def tick(self):
            ch = chain_of(eng.pats, eng.cur)
            st = (eng.cur, eng.playing_pat if eng.running else -1, eng.next_pat, tuple(ch),
                  tuple(1 if any(p['trig']) else 0 for p in eng.pats), eng.running,
                  eng.pats[eng.cur]['chain'], eng.ext_playing, eng.clock_ext)
            if st == self.shown:
                return
            self.shown = st
            for i, (b, l) in enumerate(self.btns):
                sel = i == eng.cur
                playing = _audible() and i == eng.playing_pat
                b.set_style_bg_color(_c(SEQ_ACC if sel else 0x000000), 0)
                b.set_style_border_color(_c(PLAY_C if playing else (SEQ_ACC if i == eng.next_pat else C_BTN_EDGE)), 0)
                if sel:
                    tc = 0x000000
                elif len(ch) > 1 and i in ch:
                    tc = CHAIN_C
                elif any(eng.pats[i]['trig']):
                    tc = C_TEXT
                else:
                    tc = C_LINE
                l.set_style_text_color(_c(tc), 0)
            self.chain_b.set(eng.pats[eng.cur]['chain'])
            self.play_b.set(eng.running)
            if eng.running and eng.clock_ext and not eng.ext_playing:
                self.play_b.text('ARMED')
            else:
                self.play_b.text('STOP' if eng.running else 'PLAY')

    # --- step editing ---------------------------------------------------------------------
    def step_state(pi, c, i):
        p = eng.pats[pi]
        b = 1 << i
        if not p['trig'][c] & b:
            return 0
        return 1 | (2 if p['acc'][c] & b else 0) | (4 if p['fill'][c] & b else 0)

    def step_color(s, inside=True):
        if not s:
            return STEP_OFF if inside else STEP_OUT
        if not inside:
            return STEP_DIM          # set, but past the pattern LENGTH
        if s & 4:
            return FILL_C
        if s & 2:
            return ACC_C
        return TRIG_C

    def edit_step(c, i, mode):
        """mode: 't' trigger, 'a' accent, 'f' fill (accent / fill also set the trigger)"""
        p = eng.pats[eng.cur]
        b = 1 << i
        if mode == 't':
            if p['trig'][c] & b:
                p['trig'][c] &= ~b & 0xFFFF
                p['acc'][c] &= ~b & 0xFFFF
                p['fill'][c] &= ~b & 0xFFFF
            else:
                p['trig'][c] |= b
        else:
            key = 'acc' if mode == 'a' else 'fill'
            if p[key][c] & b and p['trig'][c] & b:
                p[key][c] &= ~b & 0xFFFF
            else:
                p[key][c] |= b
                p['trig'][c] |= b
        app.pat_dirty = True
        app.grid_dirty = True
        _edited()

    # --- pages ------------------------------------------------------------------------------
    class MainPage(tulip.UIElement):
        def __init__(self):
            super().__init__()
            g = _plain(self.group, C_BG)
            # channel pads + M / S row
            self.strip = ChannelStrip(g, 4)
            self.knobs = []
            self.switches = []
            y1 = 100
            sw = {k: (l, o) for k, l, o, _ in SWITCHES}
            # row 1: the sound sources, row 2: mix / output, velocity, drum library.
            # Each synth section sits on its own tinted panel in its own colour;
            # '|' = a short divider between the sub-groups of a section.
            y2 = y1 + CELL_H + 2
            x = self.section(g, 'OSC', SEC_OSC, 2, y1, ('S:wave', 'osc_freq', '|', 'S:mod_mode', 'mod_amt',
                                                       'mod_rate', '|', 'osc_att', 'osc_dec'), sw)
            self.section(g, 'NOISE', SEC_NOISE, x + 4, y1, ('S:n_mode', 'n_freq', 'n_q', '|', 'S:n_env',
                                                             'n_att', 'n_dec'), sw)
            x = self.section(g, 'MIX / OUT', SEC_MIX, 2, y2, ('mix', 'eq_freq', 'eq_gain', 'dist', 'level',
                                                              'pan', '|', 'S:choke'), sw)
            x = self.section(g, 'VELOCITY', SEC_VEL, x + 4, y2, ('vel_osc', 'vel_noise', 'vel_mod'), sw)
            # drum sound library (factory + saved sounds) showing the channel's sound,
            # SAVE = store the selected channel's sound in the list, DEL = remove it
            x -= 6
            dw = 1020 - x - 10
            self.name_lbl = _label(g, 'DRUM', F_TINY, C_TEXT, x + 8, y2 + 6, w=dw)
            dd = _dropdown(g, drum_options(), x + 8, y2 + 22, dw, 40,
                           title=lambda: 'DRUM SOUND  ->  CH %d' % (app.sel + 1))
            dd.add_event_cb(self.drum_cb, lv.EVENT.VALUE_CHANGED, None)
            self.drum_dd = dd
            bw = (dw - 6) * 3 // 5
            Btn(g, 'SAVE SOUND', x + 8, y2 + 70, bw, 40, ACC, self.save_drum, font=F_LABEL)
            self.del_b = Btn(g, 'DEL', x + 14 + bw, y2 + 70, dw - 6 - bw, 40, MUTE_C, self.del_drum, font=F_LABEL)
            self.del_armed = 0
            # focus fader: the last touched pot
            y3 = y2 + CELL_H + 2
            self.focus = FocusFader(g, y3)
            _line(g, 2, y3 + 60, 1020)
            # bottom: PLAY (left, square), patterns, chain, morph, MASTER (right)
            y4 = y3 + 66
            H = 120
            yo = y4 + (556 - y4 - H) // 2
            bh = (H - 6) // 2
            px = 4 + H + 10
            rx = px + 6 * 52 + 2
            self.pbar = PatternBar(g, px, yo, cols=6, bw=46, bh=bh, gap=6,
                                   chain=(rx, yo, 84, H), play=(4, yo, H, H))
            # MASTER at the right edge (TEMPO / SWING / FILL are on the SEQ page)
            kn = Knob(g, 'g:master', 1020 - 72, yo - 4, 1020 - 72, PAGE_Y + yo - 4, ACC, w=72, bipolar=False)
            self.knobs.append(kn)
            # MORPH: a wide fat fader, % and ALTER A / ALTER B
            mx = rx + 84 + 12
            mw = 1020 - 72 - 10 - mx
            _label(g, 'MORPH', F_TINY, C_TEXT, mx, yo + 2)
            self.morph_lbl = _label(g, '', F_SMALL, ACC, mx + mw - 40, yo + 2, w=40)
            self.morph = FatBar(g, mx, yo + 16, mw, 48, mx, PAGE_Y + yo + 16, ACC, ACC_DIM,
                                lambda v: set_val('g:morph', v), thumb=24)
            self.morph.get = lambda: eng.morph
            self.morph.key = 'g:morph'
            for t, xx in (('A', mx + 6), ('B', mx + mw - 20)):
                _label(g, t, F_MID, ACC, xx, yo + 30)
            hw = (mw - 6) // 2
            # ALTER A / ALTER B: randomize the morph end-points of all 8 sounds (TOOLS, UNDO there)
            Btn(g, 'ALTER A', mx, yo + 72, hw, H - 72, ACC, lambda: app.tools.alter_a(), font=F_LABEL)
            Btn(g, 'ALTER B', mx + hw + 6, yo + 72, hw, H - 72, ACC, lambda: app.tools.alter_b(), font=F_LABEL)
            self.mshown = None

        def section(self, g, name, sec, x, y, items, sw):
            """a tinted panel with a title; returns the x right after it"""
            col, tint, _ = sec
            w = 6
            for it in items:
                w += 8 if it == '|' else (SW_W if it.startswith('S:') else KN_W)
            pnl = _plain(lv.obj(g), tint, 6)
            pnl.set_size(w, CELL_H - 2)
            pnl.set_pos(x, y)
            pnl.remove_flag(lv.obj.FLAG.CLICKABLE)
            _label(g, name, F_TINY, col, x + 8, y + 3)
            xx = x + 3
            for it in items:
                if it == '|':
                    _line(g, xx + 3, y + 32, 1, CELL_H - 44)
                    xx += 8
                elif it.startswith('S:'):
                    k = it[2:]
                    self.switches.append(Switch(g, k, sw[k][0], xx, y, sw[k][1], accent=col, top=12))
                    xx += SW_W
                else:
                    self.knobs.append(Knob(g, 'k:' + it, xx, y, xx, PAGE_Y + y, col, top=12))
                    xx += KN_W
            return x + w

        def drum_cb(self, e):
            i = self.drum_dd.get_selected()
            lst = drum_list()
            if i >= len(lst):
                return
            name, d, db = lst[i]
            p = default_patch()
            p.update(d)
            pb = default_patch()
            pb.update(db if db is not None else d)
            c = app.sel
            eng.a[c] = p
            eng.b[c] = pb
            eng._eff[c] = None
            eng.names[c] = name
            app.dirty_ch[c] = 1
            app.refresh_channel()
            eng.trigger(c, 1.0)
            app.flash[c] = ticks_ms()
            _edited()

        def save_drum(self):
            c = app.sel
            app.header.text_dialog('SAVE SOUND  (CH %d)' % (c + 1), _ascii(eng.names[c])[:NAME_MAX],
                                   lambda name, c=c: self.save_drum_as(c, name),
                                   lambda name: 'OVERWRITE' if user_drum_index(name) >= 0 else 'SAVE')

        def save_drum_as(self, c, name):
            if not name:
                name = 'Sound %d' % (len(app.user_drums) + 1)
            save_user_drum(name, eng.a[c], eng.b[c])
            eng.names[c] = name
            app.strip_dirty = True
            self.drum_dd.set_options(drum_options())
            self.refresh_name()
            app.header.update_status('sound saved')

        def del_drum(self):
            c = app.sel
            i = user_drum_index(eng.names[c])
            if i < 0:
                app.header.update_status('not a saved sound')
                return
            now = ticks_ms()
            if not (self.del_armed and now - self.del_armed < 3000):
                self.del_armed = now
                self.del_b.text('SURE?')
                self.del_b.set(True)
                return
            self.del_armed = 0
            self.del_b.text('DEL')
            self.del_b.set(False)
            del_user_drum(i)
            self.drum_dd.set_options(drum_options())
            self.refresh_name()
            app.header.update_status('sound deleted')

        def refresh_name(self):
            c = app.sel
            nm = eng.names[c]
            self.drum_dd.set_text(_ascii(nm))
            for i, (n, _, _) in enumerate(drum_list()):   # highlight it in the list
                if n == nm:
                    self.drum_dd.set_selected(i)
                    break
            self.name_lbl.set_text('DRUM  CH %d%s' % (c + 1, '  (saved)' if user_drum_index(nm) >= 0 else ''))
            if self.del_armed:
                self.del_armed = 0
                self.del_b.text('DEL')
                self.del_b.set(False)

        def refresh(self, knobs=True):
            if knobs:
                for k in self.knobs:
                    k.refresh()
            for s in self.switches:
                s.refresh()
            self.strip.refresh()
            self.refresh_name()
            self.mshown = None

        def tick(self, now):
            self.strip.tick(now)
            self.pbar.tick()
            self.focus.tick()
            m = int(eng.morph * 1000 + 0.5)
            if m != self.mshown:
                self.mshown = m
                if self.morph.x0 is None:
                    self.morph.show(eng.morph)
                self.morph_lbl.set_text('%d%%' % int(eng.morph * 100 + 0.5))

    class MatrixPage(tulip.UIElement):
        """all 8 channels x 16 steps; tap = toggle in the chosen mode"""

        def __init__(self):
            super().__init__()
            g = _plain(self.group, C_BG)
            self.mode = 't'
            x0 = 150
            sw = 54
            rh = 46
            y0 = 22
            self.leds = []
            for i in range(NSTEP):
                led = _plain(lv.obj(g), STEP_OUT)
                led.set_size(sw - 8, 10)
                led.set_pos(x0 + i * sw, 4)
                led.add_flag(lv.obj.FLAG.CLICKABLE)
                led.set_ext_click_area(6)
                led.add_event_cb(lambda e, i=i: self.set_len(i), lv.EVENT.PRESSED, None)
                self.leds.append(led)
            self.cells = []
            self.sel_b = []
            self.mute_b = []
            for c in range(NCH):
                y = y0 + c * rh
                b = Btn(g, _ch_name(c), 2, y, 98, rh - 6, ACC, lambda c=c: select_channel(c), font=F_LABEL)
                m = Btn(g, 'M', 110, y, 34, rh - 6, MUTE_C, lambda c=c: toggle_mute(c), font=F_LABEL)
                self.sel_b.append(b)
                self.mute_b.append(m)
                row = []
                for i in range(NSTEP):
                    o = _plain(lv.obj(g), STEP_OFF)
                    o.set_size(sw - 8, rh - 8)
                    o.set_pos(x0 + i * sw + (2 if i % 4 == 0 else 0), y)
                    o.add_flag(lv.obj.FLAG.CLICKABLE)
                    o.add_event_cb(lambda e, c=c, i=i: self.tap(c, i), lv.EVENT.PRESSED, None)
                    row.append(o)
                self.cells.append(row)
            yb = y0 + NCH * rh + 4
            H = 120
            yo = yb + 8
            x1 = 4 + H + 10
            self.mode_b = []
            for j, (m, name, col) in enumerate((('t', 'TRIGGER', TRIG_C), ('a', 'ACCENT', ACC_C), ('f', 'FILL', FILL_C))):
                b = Btn(g, name, x1 + j * 98, yo, 92, 50, col, lambda m=m: self.set_mode(m), font=F_LABEL)
                self.mode_b.append((m, b))
            self.pbar = PatternBar(g, x1, yo + 62, cols=NPAT, bw=42, bh=H - 62, gap=3,
                                   chain=(x1 + NPAT * 45 + 4, yo + 62, 84, H - 62), play=(4, yo, H, H))
            tx = x1 + 3 * 98 + 8
            cx = x1 + NPAT * 45 + 4              # CHAIN > column; RATE above it
            kx = cx + 84 + 8
            kw = (1020 - kx) // 3
            _label(g, 'Tap: TRIGGER / ACCENT / FILL\n(accent, fill set the trigger)\n'
                      'Top bar = pattern LENGTH', F_LABEL, C_DIM, tx, yo, w=cx - tx - 6)
            self.rate_b = Btn(g, 'RATE', cx, yo, 84, 50, SEQ_ACC, self.next_rate, font=F_LABEL)
            self.rate_shown = None
            # TEMPO / SWING / FILL rate in the row of the A-L buttons, - / + above
            # each: one step (1 BPM, 1 %, one fill rate), hold to repeat
            self.knobs = []
            for j, key in enumerate(('g:tempo', 'g:swing', 'g:fill')):
                bw = (kw - 10) // 2
                for t, d, xx in (('-', -1, kx + j * kw + 2), ('+', 1, kx + j * kw + 8 + bw)):
                    b = Btn(g, t, xx, yo, bw, 40, SEQ_ACC, lambda key=key, d=d: nudge(key, d), font=F_BIG)
                    ev = getattr(lv.EVENT, 'LONG_PRESSED_REPEAT', None)
                    if ev is not None:
                        b.b.add_event_cb(lambda e, key=key, d=d: nudge(key, d), ev, None)
            ky = yo + 62 + (H - 62) // 2 - (KNOB_Y + KNOB // 2)
            for j, key in enumerate(('g:tempo', 'g:swing', 'g:fill')):
                x = kx + j * kw
                self.knobs.append(Knob(g, key, x, ky, x, PAGE_Y + ky, SEQ_ACC, w=kw, bipolar=False, big=True))
            self.shown = {}
            self.led_shown = None
            self.names_shown = [None] * NCH
            self.set_mode('t')

        def set_mode(self, m):
            self.mode = m
            for mm, b in self.mode_b:
                b.set(mm == m)

        def set_len(self, i):
            eng.pats[eng.cur]['len'] = i + 1
            app.pat_dirty = True
            app.grid_dirty = True
            _edited()

        def tap(self, c, i):
            edit_step(c, i, self.mode)

        def next_rate(self):
            eng.rate = (eng.rate + 1) % len(RATES)
            app.pat_dirty = True
            self.refresh_rate()
            _edited()

        def refresh_rate(self):
            if self.rate_shown != eng.rate:
                self.rate_shown = eng.rate
                self.rate_b.text('RATE\n' + RATES[eng.rate][0])

        def refresh(self, knobs=True):
            p = eng.pats[eng.cur]
            n = p['len']
            self.refresh_rate()
            for c in range(NCH):
                self.sel_b[c].set(c == app.sel)
                self.mute_b[c].set(eng.mute[c] or eng.hold_mute[c])
                nm = _ch_name(c)
                if nm != self.names_shown[c]:
                    self.names_shown[c] = nm
                    self.sel_b[c].text(nm)
                for i in range(NSTEP):
                    col = step_color(step_state(eng.cur, c, i), i < n)
                    k = (c, i)
                    if self.shown.get(k) != col:
                        self.shown[k] = col
                        self.cells[c][i].set_style_bg_color(_c(col), 0)
            self.led_shown = None

        def tick(self, now):
            self.pbar.tick()
            n = eng.pats[eng.cur]['len']
            led = eng.led_step if (eng.running and eng.playing_pat == eng.cur) else -1
            st = (n, led)
            if st != self.led_shown:
                self.led_shown = st
                for i in range(NSTEP):
                    self.leds[i].set_style_bg_color(_c(LED_ON if i == led else (SEQ_DIM if i < n else STEP_OUT)), 0)

    class MidiPage(tulip.UIElement):
        def __init__(self):
            super().__init__()
            g = _plain(self.group, C_BG)
            _label(g, 'MIDI & CLOCK', F_MID, ACC, 12, 8)
            _label(g, 'MIDI IN', F_SMALL, C_DIM, 700, 8)
            self.mon = _label(g, '--', F_SMALL, C_TEXT, 760, 8, w=250)
            _label(g, 'NOTES / CC', F_LABEL, C_TEXT, 12, 54)
            self.dd_ch = _dropdown(g, '\n'.join(MIDI_CH_OPTS), 110, 46, 140, title='MIDI NOTES / CC CHANNEL')
            self.dd_ch.add_event_cb(self.ch_cb, lv.EVENT.VALUE_CHANGED, None)
            _label(g, 'C1-G1 (36-43) = play channel 1-8 (velocity)    C2-G2 (48-55) = mute channel 1-8 '
                      'while held\nC3-B3 (60-71) = pattern A-L (switches at the end of the pass)    '
                      'Program Change 0-127 = preset slot 1-128', F_LABEL, C_DIM, 270, 44, w=740)
            _label(g, 'CLOCK', F_LABEL, C_TEXT, 12, 104)
            self.b_int = Btn(g, 'INTERNAL', 110, 96, 100, 32, SEQ_ACC, lambda: self.set_clock(False))
            self.b_ext = Btn(g, 'EXT MIDI', 216, 96, 100, 32, SEQ_ACC, lambda: self.set_clock(True))
            self.b_out = Btn(g, 'SEND CLOCK OUT', 330, 96, 150, 32, SEQ_ACC, self.toggle_out)
            self.b_mmc = Btn(g, 'MMC IN', 488, 96, 100, 32, SEQ_ACC, self.toggle_mmc)
            _label(g, 'EXT: PLAY arms, the master\'s Start / Continue / Stop runs it.\n'
                      'MMC: IN with EXT (MPC transport), OUT with INTERNAL.',
                   F_LABEL, C_DIM, 600, 92, w=416)
            _label(g, 'SYNC', F_LABEL, C_TEXT, 12, 454)
            Btn(g, '-', 110, 446, 48, 32, SEQ_ACC, lambda: self.sync_step(-5), font=F_MID)
            self.sync_lbl = _label(g, '', F_LABEL, C_TEXT, 164, 454, w=110)
            self.sync_lbl.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
            Btn(g, '+', 280, 446, 48, 32, SEQ_ACC, lambda: self.sync_step(5), font=F_MID)
            _label(g, 'SYNC OFFSET (EXT / CLOCK OUT): the drums play this much earlier than the clock,\n'
                      'so they sound together with the clocked device (Tulip audio leaves ~30 ms late).',
                   F_LABEL, C_DIM, 340, 442, w=680)
            _label(g, 'TULIP', F_LABEL, C_TEXT, 12, 154)
            self.b_mute = Btn(g, 'MUTE TULIP SYNTHS', 110, 146, 170, 32, ACC, self.toggle_mute)
            _label(g, 'CPU', F_LABEL, C_TEXT, 12, 504)
            self.b_cpu = Btn(g, 'CPU SAVER AUTO', 110, 496, 170, 32, ACC, self.toggle_cpu)
            _label(g, 'AUTO: when AMY gets close to overload, long tails are shortened (ECO 1-3 in the '
                      'header: after 1.5 / 0.9 / 0.5 s) until the load drops.  Not saved with the kit.',
                   F_LABEL, C_DIM, 290, 494, w=720)
            _label(g, 'PATTERN OUT', F_LABEL, C_TEXT, 300, 154)
            self.dd_out = _dropdown(g, '\n'.join(['OFF'] + ['CH %d' % c for c in range(1, 17)]),
                                    400, 146, 100, 32, title="PATTERN MIDI OUT")
            self.dd_out.add_event_cb(self.out_ch_cb, lv.EVENT.VALUE_CHANGED, None)
            _label(g, 'the pattern engine sends its hits as MIDI notes C1-G1 (36-43), accent = '
                      'velocity 127, normal 64', F_LABEL, C_DIM, 512, 150, w=500)
            _label(g, 'MIDI CC', F_LABEL, C_TEXT, 12, 206)
            self.b_learn = Btn(g, 'CC LEARN', 110, 198, 100, 32, LEARN_C, self.toggle_learn)
            Btn(g, 'FORGET KNOB', 216, 198, 120, 32, ACC, self.forget)
            Btn(g, 'CLEAR ALL', 342, 198, 100, 32, ACC, self.clear_all)
            self.b_sel = Btn(g, 'CC -> SELECTED CH', 450, 198, 170, 32, ACC, self.toggle_sel)
            self.learn_lbl = _label(g, '', F_LABEL, C_DIM, 630, 206, w=390)
            _label(g, 'CC -> SELECTED CH on: a learned drum knob always moves the channel that is '
                      'selected (handy for one hardware controller).  Off: it stays on the channel it '
                      'was learned on.', F_LABEL, C_DIM, 12, 240, w=1000)
            self.cc_lbl = _label(g, '', F_SMALL, C_TEXT, 12, 280, w=1000)
            self.cc_lbl.set_style_text_line_space(4, 0)
            self.refresh()

        def ch_cb(self, e):
            app.midi_ch = MIDI_CH_VALS[self.dd_ch.get_selected()]
            if any(eng.hold_mute):            # their note-offs may not come any more
                eng.hold_mute = [False] * NCH
                app.pat_dirty = True
                app.strip_dirty = True
            _save_settings()

        def set_clock(self, ext):
            _set_clock(ext, app.clock_out)
            self.refresh()
            app.knobs_dirty = True
            _save_settings()

        def toggle_out(self):
            if eng.clock_ext:
                app.header.update_status('EXT: no clock out')
                return                   # slave: no clock out
            _set_clock(eng.clock_ext, not app.clock_out)
            self.refresh()
            _save_settings()

        def toggle_mmc(self):
            app.mmc = not app.mmc
            self.refresh()
            _save_settings()

        def sync_step(self, d):
            eng.sync_ms = max(0, min(100, eng.sync_ms + d))
            eng.resync_offset()
            self.refresh()
            _save_settings()

        def toggle_mute(self):
            global MUTE_OTHER_SYNTHS
            _mute_others(False)
            MUTE_OTHER_SYNTHS = not MUTE_OTHER_SYNTHS
            _mute_others(True)
            self.refresh()
            _save_settings()

        def out_ch_cb(self, e):
            eng.midi_out_ch = self.dd_out.get_selected()
            eng.midi_setup()
            app.pat_dirty = True
            _save_settings()

        def toggle_cpu(self):
            app.cpu_auto = not app.cpu_auto
            if not app.cpu_auto:
                app.eco_base = 0
                eng.set_eco(0)
            else:
                auto_eco()
            app.cpu_dirty = True
            self.refresh()
            _save_settings()

        def toggle_learn(self):
            app.learn_on = not app.learn_on
            app.set_learn_target(None)
            self.refresh()

        def toggle_sel(self):
            app.cc_sel = not app.cc_sel
            self.refresh()
            _save_settings()

        def forget(self):
            k = app.learn_target
            if k is None:
                self.learn_lbl.set_text('Turn CC LEARN on and touch a knob first.')
                return
            ks = (_cc_key(k), k, 'c%d:%s' % (app.sel, k[2:]) if k.startswith('k:') else k)
            for cc in [c for c, kk in app.cc_map.items() if kk in ks]:
                del app.cc_map[cc]
            _save_settings()
            self.refresh()

        def clear_all(self):
            app.cc_map = {}
            _save_settings()
            self.refresh()

        def refresh(self):
            mch = app.midi_ch
            self.dd_ch.set_selected(MIDI_CH_VALS.index(mch) if mch in MIDI_CH_VALS else 0)
            self.b_int.set(not eng.clock_ext)
            self.b_ext.set(eng.clock_ext)
            self.b_out.set(app.clock_out and not eng.clock_ext)
            self.b_mmc.set(app.mmc)
            self.b_mmc.text('MMC IN' if eng.clock_ext else 'MMC OUT')
            c = eng.comp_ticks()
            if eng.clock_ext:
                self.sync_lbl.set_text('%d ms = %d clk' % (eng.sync_ms, c // TPC))
            elif eng.clock_out:
                self.sync_lbl.set_text('%d ms = %d tick' % (eng.sync_ms, c))
            else:
                self.sync_lbl.set_text('%d ms (off)' % eng.sync_ms)
            self.b_mute.set(MUTE_OTHER_SYNTHS)
            self.b_cpu.set(app.cpu_auto)
            self.dd_out.set_selected(eng.midi_out_ch)
            self.b_learn.set(app.learn_on)
            self.b_sel.set(app.cc_sel)
            if app.learn_on:
                t = app.learn_target
                if t is None:
                    self.learn_lbl.set_text('Touch a knob or MORPH (MAIN / SEQ page), then move a controller.')
                else:
                    self.learn_lbl.set_text('%s: move a controller now.' % key_label(_cc_key(t)))
            else:
                self.learn_lbl.set_text('')
            items = sorted(app.cc_map.items())
            if not items:
                self.cc_lbl.set_text('(no CC assigned)')
            else:
                cols = []
                for cc, k in items:
                    cols.append('CC%-3d %-14s' % (cc, key_label(k)[:14]))
                lines = []
                for r in range(0, len(cols), 5):
                    lines.append('  '.join(cols[r:r + 5]))
                self.cc_lbl.set_text('\n'.join(lines))

    class ToolsPage(tulip.UIElement):
        """Microtonic menu functions: randomize, alter, copy / paste, shift, reverse, morph"""

        def __init__(self):
            super().__init__()
            g = _plain(self.group, C_BG)
            self.clip_patch = None
            self.clip_pat = None
            self.undo_state = None
            _label(g, 'DRUM PATCH  (selected channel)', F_MID, ACC, 12, 8)
            y = 40
            for j, (name, fn) in enumerate((('RANDOMIZE', self.rnd_patch), ('ALTER', self.alter_patch),
                                            ('INIT', self.init_patch), ('COPY', self.copy_patch),
                                            ('PASTE', self.paste_patch), ('EXCHANGE', self.swap_patch))):
                Btn(g, name, 12 + j * 140, y, 132, 40, ACC, fn, font=F_LABEL)
            _label(g, 'ALL 8 DRUM PATCHES', F_MID, ACC, 12, 98)
            y = 128
            for j, (name, fn) in enumerate((('RANDOMIZE ALL', self.rnd_all), ('ALTER ALL', self.alter_all),
                                            ('A > B (morph)', lambda: self.morph_copy(True)),
                                            ('B > A (morph)', lambda: self.morph_copy(False)),
                                            ('ALTER A', self.alter_a), ('ALTER B', self.alter_b))):
                Btn(g, name, 12 + j * 168, y, 160, 40, ACC, fn, font=F_LABEL)
            _label(g, 'PATTERN  (selected pattern)', F_MID, SEQ_ACC, 12, 186)
            self.only_b = Btn(g, 'SELECTED CHANNEL ONLY', 340, 182, 220, 32, SEQ_ACC, self.toggle_only,
                              font=F_LABEL)
            self.only = False
            y = 222
            for j, (name, fn) in enumerate((('RANDOMIZE', self.rnd_pat), ('ALTER', self.alter_pat),
                                            ('ACCENTS + FILLS', self.rnd_af),
                                            ('<< SHIFT', lambda: self.shift(-1)),
                                            ('SHIFT >>', lambda: self.shift(1)),
                                            ('REVERSE', self.reverse), ('CLEAR', self.clear))):
                Btn(g, name, 12 + j * 142, y, 134, 40, SEQ_ACC, fn, font=F_LABEL)
            y = 272
            for j, (name, fn) in enumerate((('COPY', self.copy_pat), ('PASTE', self.paste_pat),
                                            ('EXCHANGE', self.swap_pat))):
                Btn(g, name, 12 + j * 142, y, 134, 40, SEQ_ACC, fn, font=F_LABEL)
            _line(g, 12, 330, 1000)
            Btn(g, 'UNDO', 12, 344, 160, 48, C_TEXT, self.undo, font=F_MID)
            self.msg = _label(g, '', F_LABEL, C_DIM, 190, 356, w=820)
            _label(g, 'Morph: every drum knob has an A and a B end-point; the MORPH slider (MAIN page) '
                      'fades between them.  Turning a knob with MORPH between A and B moves both '
                      'end-points.  A > B copies A into B (then change B, e.g. at MORPH = B).  ALTER A / '
                      'ALTER B (also on MAIN) randomize one end-point of all 8 sounds; UNDO undoes it.',
                   F_LABEL, C_DIM, 12, 410, w=1000)
            self.toggle_only(False)

        def _snap(self):
            self.undo_state = eng.get_state()

        def _done(self, msg, sound=True, pattern=False):
            if sound:
                eng._eff = [None] * NCH
                for c in range(NCH):
                    app.dirty_ch[c] = 1
            if pattern:
                app.pat_dirty = True
            app.refresh_all()
            _edited()
            self.msg.set_text(msg)

        def _chans(self):
            return [app.sel] if self.only else None

        def toggle_only(self, flip=True):
            if flip:
                self.only = not self.only
            self.only_b.set(self.only)

        # drum patch
        def rnd_patch(self):
            self._snap()
            c = app.sel
            p = random_patch()
            eng.a[c] = p
            eng.b[c] = dict(p)
            eng.names[c] = 'Random'
            self._done('channel %d: random drum patch' % (c + 1))
            eng.refresh(c)
            eng.trigger(c, 1.0)

        def alter_patch(self):
            self._snap()
            c = app.sel
            eng.a[c] = alter_patch(eng.a[c])
            eng.b[c] = alter_patch(eng.b[c])
            self._done('channel %d: altered' % (c + 1))
            eng.refresh(c)
            eng.trigger(c, 1.0)

        def init_patch(self):
            self._snap()
            c = app.sel
            eng.a[c] = default_patch()
            eng.b[c] = default_patch()
            eng.names[c] = 'Init'
            self._done('channel %d: init' % (c + 1))

        def copy_patch(self):
            c = app.sel
            self.clip_patch = (dict(eng.a[c]), dict(eng.b[c]), eng.names[c])
            self.msg.set_text('channel %d copied' % (c + 1))

        def paste_patch(self):
            if self.clip_patch is None:
                self.msg.set_text('nothing copied yet')
                return
            self._snap()
            c = app.sel
            eng.a[c] = dict(self.clip_patch[0])
            eng.b[c] = dict(self.clip_patch[1])
            eng.names[c] = self.clip_patch[2]
            self._done('pasted into channel %d' % (c + 1))

        def swap_patch(self):
            if self.clip_patch is None:
                self.msg.set_text('nothing copied yet')
                return
            self._snap()
            c = app.sel
            old = (dict(eng.a[c]), dict(eng.b[c]), eng.names[c])
            eng.a[c] = dict(self.clip_patch[0])
            eng.b[c] = dict(self.clip_patch[1])
            eng.names[c] = self.clip_patch[2]
            self.clip_patch = old
            self._done('channel %d <-> clipboard' % (c + 1))

        def rnd_all(self):
            self._snap()
            for c in range(NCH):
                p = random_patch()
                eng.a[c] = p
                eng.b[c] = dict(p)
                eng.names[c] = 'Random'
            self._done('all drum patches random')

        def alter_all(self):
            self._snap()
            for c in range(NCH):
                eng.a[c] = alter_patch(eng.a[c])
                eng.b[c] = alter_patch(eng.b[c])
            self._done('all drum patches altered')

        def alter_a(self):
            self._snap()
            for c in range(NCH):
                eng.a[c] = alter_patch(eng.a[c], 0.25)
            self._done('morph A end-points altered (move MORPH to hear)')

        def alter_b(self):
            self._snap()
            for c in range(NCH):
                eng.b[c] = alter_patch(eng.b[c], 0.25)
            self._done('morph B end-points altered (move MORPH to hear)')

        def morph_copy(self, to_b):
            self._snap()
            eng.morph_copy(to_b)
            self._done('A > B' if to_b else 'B > A')

        # pattern
        def rnd_pat(self):
            self._snap()
            p = eng.pats[eng.cur]
            r = random_pattern(p['len'])
            for c in (self._chans() or range(NCH)):
                for key in ('trig', 'acc', 'fill'):
                    p[key][c] = r[key][c]
            self._done('pattern %s: random' % PAT_NAMES[eng.cur], False, True)

        def alter_pat(self):
            self._snap()
            pat_alter(eng.pats[eng.cur], chans=self._chans())
            self._done('pattern %s: altered' % PAT_NAMES[eng.cur], False, True)

        def rnd_af(self):
            self._snap()
            p = eng.pats[eng.cur]
            for c in (self._chans() or range(NCH)):
                t = p['trig'][c]
                a = 0
                f = 0
                for i in range(p['len']):
                    if t & (1 << i):
                        if random.random() < 0.3:
                            a |= 1 << i
                        if random.random() < 0.08:
                            f |= 1 << i
                p['acc'][c] = a
                p['fill'][c] = f
            self._done('pattern %s: new accents + fills' % PAT_NAMES[eng.cur], False, True)

        def shift(self, d):
            self._snap()
            pat_shift(eng.pats[eng.cur], d, self._chans())
            self._done('pattern %s: shifted' % PAT_NAMES[eng.cur], False, True)

        def reverse(self):
            self._snap()
            pat_reverse(eng.pats[eng.cur], self._chans())
            self._done('pattern %s: reversed' % PAT_NAMES[eng.cur], False, True)

        def clear(self):
            self._snap()
            p = eng.pats[eng.cur]
            for c in (self._chans() or range(NCH)):
                for key in ('trig', 'acc', 'fill'):
                    p[key][c] = 0
            self._done('pattern %s: cleared' % PAT_NAMES[eng.cur], False, True)

        def copy_pat(self):
            self.clip_pat = (self.only, app.sel, pat_copy(eng.pats[eng.cur]))
            self.msg.set_text('pattern %s copied%s' % (PAT_NAMES[eng.cur],
                                                       ' (channel %d)' % (app.sel + 1) if self.only else ''))

        def _paste(self, swap):
            if self.clip_pat is None:
                self.msg.set_text('nothing copied yet')
                return
            self._snap()
            only, src_c, clip = self.clip_pat
            p = eng.pats[eng.cur]
            old = pat_copy(p)
            if only:
                for key in ('trig', 'acc', 'fill'):
                    p[key][app.sel] = clip[key][src_c]
            else:
                chain = p['chain']
                eng.pats[eng.cur] = pat_copy(clip)
                eng.pats[eng.cur]['chain'] = chain
            if swap:
                self.clip_pat = (only, app.sel, old)
            self._done('pattern %s: %s' % (PAT_NAMES[eng.cur], 'exchanged' if swap else 'pasted'), False, True)

        def paste_pat(self):
            self._paste(False)

        def swap_pat(self):
            self._paste(True)

        def undo(self):
            if self.undo_state is None:
                self.msg.set_text('nothing to undo')
                return
            st = self.undo_state
            self.undo_state = None
            eng.set_state(st, keep_tempo=True)
            app.pat_dirty = True
            app.refresh_all()
            self.msg.set_text('undone')

    # --- user preset bank: 128 slots, one JSON file each ---------------------------------
    BANK_SLOTS = 128
    NAME_MAX = 16

    def _ascii(t):
        t = str(t)
        for a, b in (('á', 'a'), ('é', 'e'), ('í', 'i'), ('ó', 'o'), ('ö', 'o'), ('ő', 'o'),
                     ('ú', 'u'), ('ü', 'u'), ('ű', 'u'), ('Á', 'A'), ('É', 'E'), ('Í', 'I'),
                     ('Ó', 'O'), ('Ö', 'O'), ('Ő', 'O'), ('Ú', 'U'), ('Ü', 'U'), ('Ű', 'U')):
            t = t.replace(a, b)
        return ''.join(c if 32 <= ord(c) < 127 else '?' for c in t)

    class PresetBank:
        """gintonic_bank/001.json ... 128.json  ->  {"name": "...", "state": {...}}"""

        def __init__(self, folder):
            import os
            self.os = os
            self.folder = folder
            try:
                os.mkdir(folder)
            except OSError:
                pass
            self._names = None
            self._cache = {}

        def path(self, slot):
            return '%s/%03d.json' % (self.folder, slot)

        def names(self):
            if self._names is None:
                names = [None] * BANK_SLOTS
                try:
                    files = self.os.listdir(self.folder)
                except OSError:
                    files = []
                for fn in files:
                    if fn.endswith('.json') and fn[:3].isdigit():
                        s = int(fn[:3])
                        if 1 <= s <= BANK_SLOTS:
                            d = self.load(s, keep=False)
                            if d is not None:
                                names[s - 1] = str(d.get('name', 'KIT %03d' % s))
                self._names = names
            return self._names

        def load(self, slot, keep=True):
            """keep=False (listing the names): read it, do not hold it in RAM"""
            if slot in self._cache:
                return self._cache[slot]
            try:
                import json
                with open(self.path(slot)) as f:
                    d = json.load(f)
                if not isinstance(d, dict):
                    d = None
            except Exception:
                d = None
            if keep:
                self._cache = {slot: d}       # only the last one
            return d

        def save(self, slot, name, state):
            import json
            d = {'name': name, 'state': state}
            with open(self.path(slot), 'w') as f:
                json.dump(d, f)
            self._cache = {slot: d}
            if self._names is not None:
                self._names[slot - 1] = name

        def delete(self, slot):
            try:
                self.os.remove(self.path(slot))
            except OSError:
                pass
            self._cache[slot] = None
            if self._names is not None:
                self._names[slot - 1] = None

        def first_empty(self, start=1):
            names = self.names()
            for i in range(BANK_SLOTS):
                s = (start - 1 + i) % BANK_SLOTS + 1
                if names[s - 1] is None:
                    return s
            return None

    def _edited():
        if not app.dirty:
            app.dirty = True
            app.status_dirty = True

    def kit_dir():
        """gintonix/kits, or the GinTonic app's kits folder next to it"""
        if getattr(app, 'kit_dir', None) is None:
            d = _path(KIT_DIR)
            if not list_kit_files(d):
                a = (getattr(app, 'app_dir', None) or tulip.pwd()).rstrip('/')
                alt = a[:a.rfind('/')] + '/gintonic/' + KIT_DIR
                if list_kit_files(alt):
                    d = alt
            app.kit_dir = d
        return app.kit_dir

    def kit_names():
        """factory kits (gintonic_presets.py) + Microtonic kits (kits/*.json)"""
        if getattr(app, 'kit_list', None) is None:
            lst = [(n, None) for n, _ in KITS]
            for n in list_kit_files(kit_dir()):
                lst.append((n, n))
            app.kit_list = lst
        return app.kit_list

    def load_kit(i):
        lst = kit_names()
        if not lst:
            return
        name, fn = lst[i]
        try:
            if fn is None:
                st = kit_state(KITS[i], DRUM_BY_NAME)
            else:
                st = load_kit_file(kit_dir(), fn)
        except Exception as ex:
            print('gintonix: kit load error', name, ex)
            app.header.update_status('kit load error')
            return
        t0 = ticks_ms()
        eng.silence()                     # drop the old kit's tails (CPU)
        pre_eco(st.get('est'))            # CPU saver level before the sounds go out (one refresh)
        eng.set_state(st, keep_tempo=eng.clock_ext)
        if st.get('est') is None:
            auto_eco()
        if DIAG:
            print('gintonix diag: kit "%s" loaded in %d ms, est %s, eco %d' % (
                _ascii(name), ticks_ms() - t0, st.get('est'), eng.eco))
        apply_tempo()
        app.patch_name = name
        app.source_slot = None
        app.dirty = False
        app.pat_dirty = True
        app.refresh_all()
        app.status_dirty = True

    def pre_eco(est):
        """CPU saver level from a precomputed load estimate (kits/*.json)"""
        lv = 0
        if app.cpu_auto and est is not None:
            for j, th in enumerate(ECO_EST):
                if est > th:
                    lv = j + 1
        app.eco_base = lv
        eng.set_eco(lv, apply=False)
        app.cpu_dirty = True

    def auto_eco():
        """CPU saver level for the kit just loaded (from its estimated AMY load)"""
        lv = 0
        if app.cpu_auto:
            try:
                est = max(eng.est_load(i) for i in range(NPAT) if any(eng.pats[i]['trig']))
            except ValueError:
                est = 0.0
            for j, th in enumerate(ECO_EST):
                if est > th:
                    lv = j + 1
        app.eco_base = lv
        eng.set_eco(lv)
        app.cpu_dirty = True

    def cpu_watch(now):
        """AMY's own load meter (newer firmware): step the CPU saver up before
        AMY overloads (which freezes the screen), back down when it is calm"""
        if now < app.cpu_next:
            return
        app.cpu_next = now + 500
        ld = None
        if app.cpu_meter:
            try:
                ld = float(amy.render_load())
            except Exception:
                app.cpu_meter = False
        app.cpu_load = ld
        if app.cpu_auto and ld is not None:
            if ld > 0.85 and eng.eco < 3 and now >= app.cpu_hold:
                eng.set_eco(eng.eco + 1)
                app.cpu_hold = now + 1500
                app.cpu_calm = 0
            elif ld < 0.45 and eng.eco > app.eco_base:
                if not app.cpu_calm:
                    app.cpu_calm = now
                elif now - app.cpu_calm > 6000:
                    eng.set_eco(eng.eco - 1)
                    app.cpu_calm = 0
            else:
                app.cpu_calm = 0
        app.cpu_dirty = True

    def load_slot(slot):
        slot = max(1, min(BANK_SLOTS, int(slot)))
        app.slot = slot
        d = app.bank.load(slot)
        if d is not None:
            eng.silence()
            eng.set_state(d.get('state', {}), keep_tempo=eng.clock_ext)
            auto_eco()
            apply_tempo()
            app.patch_name = d.get('name', 'KIT %03d' % slot)
            app.source_slot = slot
            app.dirty = False
            app.pat_dirty = True
            app.refresh_all()
        app.header.update_slot_ui(rebuild=False)
        if d is None:
            app.header.update_status('%03d empty' % slot)
        _save_settings()
        return d is not None

    def save_slot(slot, name):
        slot = max(1, min(BANK_SLOTS, int(slot)))
        if not name:
            name = 'KIT %03d' % slot
        app.bank.save(slot, name, eng.get_state())
        app.slot = slot
        app.source_slot = slot
        app.patch_name = name
        app.dirty = False
        app.header.update_slot_ui()
        _save_settings()

    def suggest_save_slot():
        if app.source_slot is not None:
            return app.source_slot
        return app.bank.first_empty(app.slot) or app.slot

    def _slot_options():
        return '\n'.join('%03d  %s' % (i + 1, _ascii(n) if n else '---')
                         for i, n in enumerate(app.bank.names()))

    class Header(tulip.UIElement):
        def __init__(self):
            super().__init__()
            g = _plain(self.group, C_BG)
            _label(g, 'GINTONIX', F_BIG, 0xffffff, 6, 6)
            self.pages = []
            for j, name in enumerate(('MAIN', 'SEQ', 'MIDI', 'TOOLS')):
                b = Btn(g, name, 140 + j * 62, 5, 58, 32, ACC, lambda j=j: app.show_page(j))
                self.pages.append(b)
            fd = _dropdown(g, '\n'.join(_ascii(n) for n, _ in kit_names()) or 'Init', 392, 5, 74, 32, title='LOAD KIT')
            fd.set_text('KITS')
            fd.add_event_cb(self.kit_cb, lv.EVENT.VALUE_CHANGED, None)
            self.kit_dd = fd
            Btn(g, '<', 470, 5, 28, 32, ACC, lambda: self.step_slot(-1))
            self.slot_dd = _dropdown(g, _slot_options(), 500, 5, 168, 32, title='KIT SLOT')
            self.slot_dd.add_event_cb(self.slot_cb, lv.EVENT.VALUE_CHANGED, None)
            Btn(g, '>', 670, 5, 28, 32, ACC, lambda: self.step_slot(1))
            Btn(g, 'LOAD', 702, 5, 52, 32, ACC, self.load_cb)
            Btn(g, 'SAVE...', 758, 5, 66, 32, ACC, self.save_dialog)
            self.del_btn = Btn(g, 'DEL', 828, 5, 46, 32, ACC, self.del_cb)
            self.status = _label(g, '', F_SMALL, C_DIM, 880, 17, w=142)
            self.cpu = _label(g, '', F_SMALL, C_DIM, 880, 3, w=142)
            self.cpu_shown = None
            self.del_armed = 0
            self.dlg = None

        def update_slot_ui(self, rebuild=True):
            if rebuild:
                self.slot_dd.set_options(_slot_options())
            self.slot_dd.set_selected(app.slot - 1)
            self.disarm_del()
            self.update_status()

        def update_status(self, msg=None):
            if msg is None:
                msg = ('*' if app.dirty else '') + _ascii(app.patch_name)[:17]
            self.status.set_text(msg)
            self.status.set_style_text_color(_c(ACC if app.dirty else C_DIM), 0)

        def show_cpu(self):
            t = ''
            if SHOW_CPU and app.cpu_load is not None:
                t = 'CPU %d%%' % int(app.cpu_load * 100 + 0.5)
            if eng.eco:
                t += (' ' if t else '') + 'ECO %d' % eng.eco
            if t != self.cpu_shown:
                self.cpu_shown = t
                self.cpu.set_text(t)
                self.cpu.set_style_text_color(_c(ACC if eng.eco else C_DIM), 0)

        def step_slot(self, d):
            load_slot((app.slot - 1 + d) % BANK_SLOTS + 1)

        def slot_cb(self, e):
            load_slot(self.slot_dd.get_selected() + 1)

        def kit_cb(self, e):
            if kit_names():
                load_kit(self.kit_dd.get_selected())
            self.kit_dd.set_text('KITS')

        def load_cb(self):
            if not load_slot(app.slot):
                self.update_status('%03d empty' % app.slot)

        def disarm_del(self):
            self.del_armed = 0
            self.del_btn.text('DEL')
            self.del_btn.set(False)

        def del_cb(self):
            name = app.bank.names()[app.slot - 1]
            if name is None:
                self.update_status('%03d empty' % app.slot)
                return
            now = ticks_ms()
            if not (self.del_armed and now - self.del_armed < 3000):
                self.del_armed = now
                self.del_btn.text('SURE?')
                self.del_btn.set(True)
                self.update_status('delete %03d?' % app.slot)
                return
            app.bank.delete(app.slot)
            if app.source_slot == app.slot:
                app.source_slot = None
            self.update_slot_ui()

        def save_dialog(self):
            if self.dlg is not None:
                return
            self.disarm_del()
            for pg in app.pages:
                pg.group.add_flag(lv.obj.FLAG.HIDDEN)
            self.group.add_flag(lv.obj.FLAG.HIDDEN)
            d = _plain(lv.obj(app.group), C_BG)
            d.set_size(1024, 600)
            d.set_pos(0, 0)
            d.add_flag(lv.obj.FLAG.CLICKABLE)
            self.dlg = d
            _label(d, 'SAVE KIT', F_BIG, ACC, 0, 12, w=1024, center=True)
            _label(d, 'SLOT', F_MID, C_TEXT, 110, 70)
            Btn(d, '<', 200, 62, 44, 40, ACC, lambda: self.dlg_step(-1), font=F_MID)
            dd = _dropdown(d, _slot_options(), 250, 62, 300, 40, title='SAVE TO SLOT')
            dd.add_event_cb(lambda e: self.dlg_set_slot(dd.get_selected() + 1),
                            lv.EVENT.VALUE_CHANGED, None)
            self.dlg_dd = dd
            Btn(d, '>', 556, 62, 44, 40, ACC, lambda: self.dlg_step(1), font=F_MID)
            self.dlg_info = _label(d, '', F_LABEL, C_DIM, 616, 66, w=380)
            _label(d, 'NAME', F_MID, C_TEXT, 110, 128)
            ta = lv.textarea(d)
            ta.set_one_line(True)
            ta.set_max_length(NAME_MAX)
            nm = _ascii(app.patch_name)[:NAME_MAX] if app.patch_name else ''
            ta.set_text(nm)
            ta.set_style_text_font(F_MID, 0)
            ta.set_size(400, 48)
            ta.set_pos(200, 116)
            self.dlg_ta = ta
            Btn(d, 'CANCEL', 620, 118, 150, 44, ACC, self.close_dialog, font=F_MID)
            self.dlg_ok = Btn(d, 'SAVE', 780, 118, 150, 44, ACC, self.dlg_save, font=F_MID)
            kb = lv.keyboard(d)
            kb.set_size(984, 330)
            kb.align(lv.ALIGN.TOP_MID, 0, 190)
            kb.set_textarea(ta)
            kb.add_event_cb(lambda e: self.dlg_save(), lv.EVENT.READY, None)
            kb.add_event_cb(lambda e: self.close_dialog(), lv.EVENT.CANCEL, None)
            self.dlg_set_slot(suggest_save_slot())

        def text_dialog(self, title, text, on_ok, ok_label=None):
            """full screen name entry: on_ok(name) on SAVE / keyboard OK"""
            if self.dlg is not None:
                return
            self.disarm_del()
            for pg in app.pages:
                pg.group.add_flag(lv.obj.FLAG.HIDDEN)
            self.group.add_flag(lv.obj.FLAG.HIDDEN)
            d = _plain(lv.obj(app.group), C_BG)
            d.set_size(1024, 600)
            d.set_pos(0, 0)
            d.add_flag(lv.obj.FLAG.CLICKABLE)
            self.dlg = d
            _label(d, title, F_BIG, ACC, 0, 12, w=1024, center=True)
            _label(d, 'NAME', F_MID, C_TEXT, 110, 128)
            ta = lv.textarea(d)
            ta.set_one_line(True)
            ta.set_max_length(NAME_MAX)
            ta.set_text(text)
            ta.set_style_text_font(F_MID, 0)
            ta.set_size(400, 48)
            ta.set_pos(200, 116)
            self.dlg_ta = ta
            self.tdlg_ok = on_ok
            self.tdlg_label = ok_label
            Btn(d, 'CANCEL', 620, 118, 150, 44, ACC, self.close_dialog, font=F_MID)
            self.tdlg_btn = Btn(d, 'SAVE', 780, 118, 150, 44, ACC, self.tdlg_save, font=F_MID)
            self.tdlg_shown = None
            ta.add_event_cb(lambda e: self.tdlg_check(), lv.EVENT.VALUE_CHANGED, None)
            kb = lv.keyboard(d)
            kb.set_size(984, 330)
            kb.align(lv.ALIGN.TOP_MID, 0, 190)
            kb.set_textarea(ta)
            kb.add_event_cb(lambda e: self.tdlg_save(), lv.EVENT.READY, None)
            kb.add_event_cb(lambda e: self.close_dialog(), lv.EVENT.CANCEL, None)
            self.tdlg_check()

        def tdlg_check(self):
            if self.dlg is None or self.tdlg_label is None:
                return
            t = self.tdlg_label(_ascii(self.dlg_ta.get_text()).strip()[:NAME_MAX])
            if t != self.tdlg_shown:
                self.tdlg_shown = t
                self.tdlg_btn.text(t)
                self.tdlg_btn.set(t != 'SAVE')

        def tdlg_save(self):
            if self.dlg is None:
                return
            name = _ascii(self.dlg_ta.get_text()).strip()[:NAME_MAX]
            cb = self.tdlg_ok
            self.close_dialog()
            try:
                cb(name)
            except Exception as ex:
                self.update_status('save error')
                print('gintonix: save error', ex)

        def dlg_step(self, dlt):
            self.dlg_set_slot((self.dlg_slot - 1 + dlt) % BANK_SLOTS + 1)

        def dlg_set_slot(self, slot):
            self.dlg_slot = slot
            self.dlg_dd.set_selected(slot - 1)
            old = app.bank.names()[slot - 1]
            self.dlg_ta.set_placeholder_text('KIT %03d' % slot)
            if old is None:
                self.dlg_info.set_text('empty slot')
                self.dlg_info.set_style_text_color(_c(C_DIM), 0)
                self.dlg_ok.text('SAVE')
                self.dlg_ok.set(False)
            else:
                self.dlg_info.set_text('overwrites:\n%s' % _ascii(old))
                self.dlg_info.set_style_text_color(_c(ACC), 0)
                self.dlg_ok.text('OVERWRITE')
                self.dlg_ok.set(True)

        def dlg_save(self):
            if self.dlg is None:
                return
            name = _ascii(self.dlg_ta.get_text()).strip()[:NAME_MAX]
            slot = self.dlg_slot
            self.close_dialog()
            try:
                save_slot(slot, name)
                self.update_status('saved %03d' % slot)
            except Exception as ex:
                self.update_status('save error')
                print('gintonix: save error', ex)

        def close_dialog(self):
            if self.dlg is not None:
                self.dlg.delete()
                self.dlg = None
                self.group.remove_flag(lv.obj.FLAG.HIDDEN)
                app.show_page(app.page)

    # --- files ------------------------------------------------------------------------
    def _path(name):
        d = getattr(app, 'app_dir', None) or tulip.pwd()
        return d.rstrip('/') + '/' + name

    def _load_json(name, default):
        try:
            import json
            with open(_path(name)) as f:
                return json.load(f)
        except Exception:
            return default

    def _save_json(name, data):
        try:
            import json
            with open(_path(name), 'w') as f:
                json.dump(data, f)
        except Exception as e:
            print('gintonix: could not save', name, e)

    # --- saved drum sounds (DRUM list) -------------------------------------------------
    USER_DRUMS = 'gintonix_drums.json'

    def _rnd_patch(p):
        return {k: (round(v, 5) if isinstance(v, float) else v) for k, v in p.items()}

    def load_user_drums():
        lst = _load_json(USER_DRUMS, [])
        out = []
        for d in lst if isinstance(lst, list) else []:
            try:
                out.append((str(d['name']), dict(d['a']), dict(d['b']) if 'b' in d else None))
            except Exception:
                pass
        app.user_drums = out

    def _write_user_drums():
        data = []
        for n, a, b in app.user_drums:
            d = {'name': n, 'a': _rnd_patch(a)}
            if b is not None and b != a:
                d['b'] = _rnd_patch(b)
            data.append(d)
        _save_json(USER_DRUMS, data)

    def user_drum_index(name):
        for i, (n, _, _) in enumerate(app.user_drums):
            if n == name:
                return i
        return -1

    def save_user_drum(name, a, b):
        a = dict(a)
        b = dict(b) if b != a else None
        i = user_drum_index(name)
        if i >= 0:
            app.user_drums[i] = (name, a, b)
        else:
            app.user_drums.append((name, a, b))
            app.user_drums.sort(key=lambda t: t[0].lower())
        _write_user_drums()

    def del_user_drum(i):
        del app.user_drums[i]
        _write_user_drums()

    def drum_list():
        """[(name, patch A, patch B or None)]: saved sounds first, then the factory ones"""
        return [(n, a, b) for n, a, b in app.user_drums] + [(n, d, None) for n, d in DRUMS]

    def drum_options():
        return '\n'.join(['* ' + _ascii(n) for n, _, _ in app.user_drums] + [_ascii(n) for n, _ in DRUMS])

    def _save_settings():
        # Writing to flash stalls the ESP32 for a moment: only mark it here, the
        # real write happens while stopped (or on quit).
        app.settings_dirty = True

    def _write_settings():
        app.settings_dirty = False
        _save_json(SETTINGS, {
            'v': 1, 'midi_ch': app.midi_ch, 'clock_ext': eng.clock_ext, 'clock_out': app.clock_out,
            'mute': MUTE_OTHER_SYNTHS, 'tempo': eng.tempo, 'slot': app.slot, 'cc_sel': app.cc_sel,
            'midi_out': eng.midi_out_ch, 'cpu_auto': app.cpu_auto, 'mmc': app.mmc, 'sync_ms': eng.sync_ms,
            'cc': [[cc, k] for cc, k in app.cc_map.items()],
        })

    DEFAULT_CC = {1: 'g:morph', 7: 'g:master'}

    def _cc_key(k):
        """what a learned knob maps to: the selected channel ('k:') or this channel"""
        if k.startswith('k:') and not app.cc_sel:
            return 'c%d:%s' % (app.sel, k[2:])
        return k

    def _valid_key(k):
        if k in G_KEYS:
            return True
        try:
            c, p = _split(k)
        except Exception:
            return False
        return c is not None and 0 <= c < NCH and p in KNOB_KEYS

    def _load_settings():
        global MUTE_OTHER_SYNTHS
        s = _load_json(SETTINGS, None)
        if not isinstance(s, dict):
            s = {}
            app.settings_dirty = True
        app.midi_ch = s.get('midi_ch', 0)
        if app.midi_ch not in MIDI_CH_VALS:
            app.midi_ch = 0
        eng.clock_ext = bool(s.get('clock_ext', False))
        app.clock_out = bool(s.get('clock_out', False))
        MUTE_OTHER_SYNTHS = bool(s.get('mute', MUTE_OTHER_SYNTHS))
        app.cc_sel = bool(s.get('cc_sel', True))
        app.cpu_auto = bool(s.get('cpu_auto', True))
        app.mmc = bool(s.get('mmc', s.get('mmc_in', True)))
        try:
            eng.sync_ms = max(0, min(100, int(s.get('sync_ms', 30))))
        except Exception:
            eng.sync_ms = 30
        try:
            eng.set_tempo(float(s['tempo']) if 'tempo' in s else tulip.seq_bpm())
        except Exception:
            pass
        app.cc_map = dict(DEFAULT_CC)
        if 'cc' in s:
            try:
                cm = {}
                for cc, k in s['cc']:
                    if _valid_key(k):
                        cm[int(cc)] = k
                app.cc_map = cm
            except Exception:
                pass
        try:
            app.slot = max(1, min(BANK_SLOTS, int(s.get('slot', 1))))
        except Exception:
            app.slot = 1
        try:
            eng.midi_out_ch = int(s.get('midi_out', 0)) % 17
        except Exception:
            pass
        eng.midi_setup()

    # --- clock -------------------------------------------------------------------------
    def _set_clock(ext, out):
        was_ext = eng.clock_ext
        was_running = eng.running and (eng.ext_playing or not was_ext)   # (armed = not playing)
        if eng.running and eng.clock_out and not (out and not ext):
            _midi_out(bytes([0xFC]))             # clock out ends: MIDI Stop to the slaves
        try:
            # neutral first (AMY runs on its own clock), then the new mode
            tulip.external_midi_sync(False)
            eng._raw('zY1Z')
            if ext:
                tulip.external_midi_sync(True)
            elif out:
                tulip.external_midi_sync(send=True)
            app.sync_touched = True
        except Exception as e:
            print('gintonix: MIDI clock sync not available in this firmware:', e)
            ext = out = False
        if ext != was_ext:
            eng._bpm_t = None
            if eng.running:
                eng.set_running(False)
        eng.clock_ext = ext
        eng.clock_out = out and not ext
        app.clock_out = out
        _ext_synths(ext)
        if ext:
            # AMY waits for the master's Start; PLAY (and switching to EXT while
            # playing) arms the loop on its grid
            eng.hold()
            eng.ext_origin = 1
            eng.ext_playing = False
        else:
            eng.ext_origin = None
            eng.ext_playing = False
            eng._held_at = None
        if ext and not eng.running:
            eng.set_running(True)            # EXT: armed for the master's Start
        elif ext != was_ext and was_running:
            eng.set_running(True)
        apply_tempo()
        app.midi_page_dirty = True

    def _ext_synths(on):
        """AMY acts on an incoming MIDI Start / Stop (0xFA / 0xFC) only if MIDI
        channel 11 / 13 is in use (the low nibble of the status byte).  A silent
        one-oscillator synth on each makes AMY restart / stop its tick count the
        moment the byte arrives: the pattern then starts exactly on the master's
        first clock instead of when Python gets to the MIDI Start."""
        if on:
            if app.ext_synths:
                return
            try:
                used = set(midi.config.synth_per_channel.keys())
            except Exception:
                used = set()
            for ch in (11, 13):
                if ch in used:
                    continue
                try:
                    eng._raw('i%div1in1Z' % ch)
                    amy.send(synth=ch, grab_midi_notes=0)
                    app.ext_synths.append(ch)
                except Exception as ex:
                    print('gintonix: EXT start/stop synth', ch, ex)
        else:
            for ch in app.ext_synths:
                try:
                    eng._raw('i%div0Z' % ch)
                except Exception:
                    pass
            app.ext_synths = []

    def _midi_out(b):
        try:
            tulip.midi_out(b)
        except Exception:
            pass

    def _mmc_send(cmd):
        _midi_out(bytes([0xF0, 0x7F, 0x7F, 0x06, cmd, 0xF7]))

    def _audible():
        """the loop really plays (EXT: armed but the master stopped = not)"""
        return eng.running and (eng.ext_playing or not eng.clock_ext)

    def transport(on):
        """PLAY / STOP button (EXT: PLAY arms, the master's Start runs it)"""
        if on == eng.running:
            return
        eng.set_running(on)
        if eng.clock_ext:
            if on and not eng.ext_playing:
                app.header.update_status('EXT: armed, waits for MIDI Start')
            return
        if app.mmc:
            if on:
                _mmc_send(0x02)                  # MMC PLAY
            else:
                _mmc_send(0x01)                  # MMC STOP + LOCATE 0
                _midi_out(bytes([0xF0, 0x7F, 0x7F, 0x06, 0x44, 0x06, 0x01, 0, 0, 0, 0, 0, 0xF7]))
        if not on and eng.clock_out:
            _midi_out(bytes([0xFC]))             # MIDI Stop to the slaves

    # --- MIDI -------------------------------------------------------------------------
    def midi_cb(m):
        if app is None or not m:
            return
        b = m[0]
        if len(m) == 1:
            # realtime from the clock master (AMY counts the MIDI clock itself)
            if b in (0xFA, 0xFB, 0xFC):
                nm = {0xFA: 'MIDI Start', 0xFB: 'MIDI Continue', 0xFC: 'MIDI Stop'}[b]
                if not eng.clock_ext:
                    app.mon_text = nm + '  (ignored: INT clock)'
                    return
                app.mon_text = nm
                if b == 0xFA:
                    app.spp = 0
                    eng.ext_start(0)
                elif b == 0xFB:
                    eng.ext_start(app.spp, cont=True)
                else:
                    eng.ext_stop()
            return
        if b == 0xF2:
            if len(m) >= 3:
                app.spp = m[1] | (m[2] << 7)     # Song Position (16ths)
                app.mon_text = 'Song Position %d' % app.spp
            return
        if len(m) < 2:
            return
        st = m[0] & 0xF0
        chan = (m[0] & 0x0F) + 1
        if app.midi_ch == -1 or (app.midi_ch != 0 and app.midi_ch != chan):
            return
        if st == 0xC0:
            app.mon_text = 'ch%d PC %d' % (chan, m[1])
            app.pending_slot = m[1] + 1          # loaded by the UI tick
            return
        if len(m) < 3:
            return
        if st == 0x90 and m[2] > 0:
            n = m[1]
            app.mon_text = 'ch%d note %d  v%d' % (chan, n, m[2])
            if NOTE_TRIG <= n < NOTE_TRIG + NCH:
                c = n - NOTE_TRIG
                eng.trigger(c, m[2] / 127.0)
                app.flash[c] = ticks_ms()
            elif NOTE_MUTE <= n < NOTE_MUTE + NCH:
                eng.hold_mute[n - NOTE_MUTE] = True
                app.pat_dirty = True
                app.strip_dirty = True
            elif NOTE_PAT <= n < NOTE_PAT + NPAT:
                eng.select_pattern(n - NOTE_PAT)
                app.grid_dirty = True
            return
        if st == 0x80 or (st == 0x90 and m[2] == 0):
            n = m[1]
            if NOTE_MUTE <= n < NOTE_MUTE + NCH:
                eng.hold_mute[n - NOTE_MUTE] = False
                app.pat_dirty = True
                app.strip_dirty = True
            return
        if st != 0xB0:
            return
        cc, val = m[1], m[2]
        app.mon_text = 'ch%d CC%d = %d' % (chan, cc, val)
        if app.learn_on and app.learn_target is not None:
            k = _cc_key(app.learn_target)
            for c in [c for c, kk in app.cc_map.items() if kk == k]:
                del app.cc_map[c]
            app.cc_map[cc] = k
            app.learn_on = False
            app.set_learn_target(None)
            _save_settings()
            app.midi_page_dirty = True
            return
        k = app.cc_map.get(cc)
        if k is not None:
            set_val(k, val / 127.0)
            if k.startswith('k:') or k in G_KEYS:
                kn = app.knob_by_key.get(k)
            else:
                c, p = _split(k)
                kn = app.knob_by_key.get('k:' + p) if c == app.sel else None
            if kn is not None:
                app.dirty_knobs[kn.key] = kn

    # MIDI Machine Control (e.g. an MPC's transport): F0 7F <device> 06 <command> F7
    MMC_PLAY = (0x02, 0x03)          # Play, Deferred Play
    MMC_STOP = (0x01, 0x09)          # Stop, Pause

    def sysex_cb(s):
        """Tulip hands every SysEx (except AMY's own) to midi.sysex_callback"""
        if app is None:
            return
        try:
            b = bytes(s)
            i = b.find(b'\xf0\x7f')
            if i >= 0 and len(b) >= i + 5 and b[i + 3] == 0x06:
                cmd = b[i + 4]
                if not (app.mmc and eng.clock_ext):
                    app.mon_text = 'MMC %02X  (ignored: MMC IN works with EXT)' % cmd
                    return
                if cmd in MMC_PLAY:
                    app.mon_text = 'MMC Play'
                    if not eng.ext_playing:
                        if app.spp:
                            eng.ext_start(app.spp, cont=True)
                        else:
                            eng.ext_start(0)
                elif cmd in MMC_STOP:
                    app.mon_text = 'MMC Stop' if cmd == 0x01 else 'MMC Pause'
                    if eng.ext_playing:
                        eng.ext_stop()
                elif cmd == 0x05:
                    app.spp = 0
                    app.mon_text = 'MMC Rewind'
                elif cmd == 0x44 and len(b) >= i + 11 and b[i + 6] == 0x01:
                    hh, mm, ss, ff = b[i + 7] & 0x1F, b[i + 8], b[i + 9], b[i + 10]
                    sec = hh * 3600 + mm * 60 + ss + ff / 30.0
                    app.spp = int(sec * eng.tempo / 15.0 + 0.5)
                    app.mon_text = 'MMC Locate %d:%02d:%02d' % (hh, mm, ss)
                return
        except Exception as ex:
            print('gintonix: sysex', ex)
        prev = getattr(app, 'prev_sysex_cb', None)
        if prev is not None:
            prev(s)

    # --- muting Tulip's own MIDI synths -------------------------------------------------
    def _mute_others(on):
        if on:
            if not MUTE_OTHER_SYNTHS or app.muted:
                return
            try:
                chans = list(midi.config.synth_per_channel.keys())
            except Exception:
                chans = [1, 10]
            app.muted = [c for c in chans if 1 <= c <= 16]
            for c in app.muted:
                try:
                    amy.send(synth=c, grab_midi_notes=0)
                    amy.send(synth=c, vel=0)
                except Exception:
                    pass
        else:
            for c in app.muted:
                try:
                    amy.send(synth=c, grab_midi_notes=1)
                except Exception:
                    pass
            app.muted = []

    # --- callbacks ---------------------------------------------------------------------
    def _seq_cb(t):
        if eng is not None:
            try:
                if app is not None and app.running:
                    _tick_watchdog()
                if DIAG:
                    t0 = ticks_ms()
                    eng.seq_cb(t)
                    d = ticks_ms() - t0
                    if app is not None and d > app.seq_max:
                        app.seq_max = d
                else:
                    eng.seq_cb(t)
            except Exception as ex:
                if app is not None:
                    app.tick_errors += 1
                    if app.tick_errors <= 3:
                        print('gintonix: seq error', ex)

    def _tick(gen):
        if app is None or not app.running or gen != app.tick_gen:
            return          # (an older chain, replaced by the watchdog)
        app.tick_last = ticks_ms()
        try:
            _tick_body()
        except Exception as ex:
            # never let one error stop the UI loop (that looks like a frozen screen)
            app.tick_errors += 1
            if app.tick_errors <= 3:
                try:
                    import sys
                    sys.print_exception(ex)
                except Exception:
                    print('gintonix: tick error', ex)
        try:
            tulip.defer(_tick, app.tick_gen, TICK_MS)
        except ValueError:
            try:
                tulip.defer(_tick, app.tick_gen, TICK_MS * 4)
            except ValueError:
                pass            # the watchdog in _seq_cb starts it again

    def _frame(arg):
        """display frame: the UI loop rides on Tulip's sequencer (tulip.defer),
        which stands still when an external MIDI clock stops: then this runs it"""
        if app is None or not app.running:
            return
        now = ticks_ms()
        if now - app.tick_last > 150 and now - app.frame_last >= TICK_MS:
            app.frame_last = now
            app.tick_fallback = True
            try:
                _tick_body()
            except Exception as ex:
                app.tick_errors += 1
                if app.tick_errors <= 3:
                    print('gintonix: tick error', ex)

    def _tick_watchdog():
        """Tulip drops a deferred call when MicroPython's schedule queue is full
        (e.g. after a long kit load the sequencer callbacks fill it): the UI loop
        would stop for good.  The sequencer callback keeps coming, so it restarts
        the loop when it has been quiet for too long."""
        now = ticks_ms()
        if now - app.tick_last > 600 or app.tick_fallback:
            app.tick_fallback = False
            app.tick_gen += 1
            app.tick_last = now
            app.tick_restarts += 1
            try:
                tulip.defer(_tick, app.tick_gen, 0)
            except Exception:
                pass

    def diag(now, t0):
        """DIAG = True (top of the file): one line every 2 s in the REPL / USB
        serial console, so a freeze can be traced (what was going up before it)"""
        dt = ticks_ms() - t0
        if dt > app.diag_max:
            app.diag_max = dt
        if now < app.diag_next:
            return
        app.diag_next = now + 2000
        try:
            mf = gc.mem_free() // 1024
        except Exception:
            mf = -1
        ld = app.cpu_load
        print('gintonix diag: amy %s  eco %d  mem_free %dk  tick_max %dms  seq_max %dms  events %d  errors %d  restarts %d  run %d' % (
            ('%d%%' % int(ld * 100 + 0.5)) if ld is not None else 'n/a', eng.eco, mf, app.diag_max,
            app.seq_max, len(eng._ev), app.tick_errors, app.tick_restarts, 1 if eng.running else 0))
        app.diag_max = 0
        app.seq_max = 0

    def _tick_body():
        now = ticks_ms()
        if DIAG:
            diag(now, app.tick_t0)
            app.tick_t0 = ticks_ms()
        eng.tick_ms(now)
        cpu_watch(now)
        if app.cpu_dirty:
            app.cpu_dirty = False
            app.header.show_cpu()
        if app.pending_slot is not None and app.header.dlg is None and app.picker is None:
            sl = app.pending_slot
            app.pending_slot = None
            load_slot(sl)
        if app.status_dirty:
            app.status_dirty = False
            app.header.update_status()
        if (app.dirty_ch or app.pat_dirty) and now - app.last_refresh >= REFRESH_MS:
            app.last_refresh = now
            need = app.pat_dirty
            app.pat_dirty = False
            chs = list(app.dirty_ch.keys())
            app.dirty_ch = {}
            for c in chs:
                eng.refresh(c)
                if eng.hit_sig_changed(c):
                    need = True
            if need and eng.running:
                eng.sync_loop()
        eng.py_random(now)
        eng.housekeeping()
        # channel lights from the pattern that is playing
        if eng.running and eng.led_step != app.flash_step:
            app.flash_step = eng.led_step
            if eng.led_step >= 0:
                p = eng.pats[eng.playing_pat]
                b = 1 << eng.led_step
                for c in range(NCH):
                    if p['trig'][c] & b and not (eng.mute[c] or eng.hold_mute[c]):
                        app.flash[c] = now
        if app.dirty_knobs and now - app.last_vis >= VIS_MS:
            app.last_vis = now
            for kn in app.dirty_knobs.values():
                kn.refresh()
            app.dirty_knobs = {}
        if app.knob_queue and app.active:
            for _ in range(KNOBS_PER_TICK):
                if not app.knob_queue:
                    break
                app.knob_queue.pop().refresh()
        if app.settings_dirty and not eng.running and not app.dirty_ch:
            _write_settings()
        if now - app.last_gc >= GC_MS and not app.dirty_ch:
            app.last_gc = now
            gc.collect()
        if app.active:
            if app.knobs_dirty:
                app.knobs_dirty = False
                app.knob_queue = list(app.all_knobs)
                for s in app.main.switches:
                    s.refresh()
            if app.strip_dirty:
                app.strip_dirty = False
                app.main.strip.refresh()
                if app.page == 1:
                    app.matrix.refresh()
            if app.grid_dirty:
                app.grid_dirty = False
                if app.page == 1:
                    app.matrix.refresh()
            if app.page == 0:
                app.main.tick(now)
            elif app.page == 1:
                app.matrix.tick(now)
            if app.midi_page_dirty and app.page == 2:
                app.midi_page_dirty = False
                app.midi_page.refresh()
            if app.mon_text is not None and app.page == 2:
                app.midi_page.mon.set_text(app.mon_text)
                app.mon_text = None
        if DIAG:
            d = ticks_ms() - now
            if d > app.diag_max:
                app.diag_max = d

    def activate(screen):
        if app is None:
            return
        _mute_others(True)
        try:
            tulip.frame_callback(_frame, 0)
        except Exception:
            pass

    def deactivate(screen):
        try:
            tulip.frame_callback()
        except Exception:
            pass

    def quit(screen):
        global app, eng
        try:
            transport(False)                 # (MIDI Stop to clock slaves)
        except Exception:
            pass
        try:
            midi.remove_callback(midi_cb)
        except Exception:
            pass
        try:
            if getattr(midi, 'sysex_callback', None) is sysex_cb:
                midi.sysex_callback = getattr(screen, 'prev_sysex_cb', None)
        except Exception:
            pass
        try:
            tulip.frame_callback()
        except Exception:
            pass
        try:
            tulip.seq_remove_callback(screen.seq_slot)
        except Exception:
            pass
        try:
            _ext_synths(False)
        except Exception:
            pass
        if getattr(screen, 'sync_touched', False):
            try:
                tulip.external_midi_sync(False)
            except Exception:
                pass
        try:
            eng._raw('zY1Z')                     # AMY's clock runs again (EXT held it)
        except Exception:
            pass
        _mute_others(False)
        try:
            _write_settings()
        except Exception:
            pass
        screen.running = False
        eng.release()
        app = None
        eng = None

    def run(screen):
        global app, eng
        app = screen
        app.offset_y = 0
        app.set_bg_color(0)
        app.group.set_style_bg_color(_c(C_BG), 0)
        app.quit_callback = quit
        app.activate_callback = activate
        app.deactivate_callback = deactivate
        app.muted = []
        app.learn_on = False
        app.learn_target = None
        app.cc_sel = True
        app.dirty_ch = {}
        app.pat_dirty = False
        app.last_refresh = 0
        app.last_vis = 0
        app.settings_dirty = False
        app.knob_queue = []
        app.last_gc = 0
        app.dirty_knobs = {}
        app.knobs_dirty = False
        app.strip_dirty = False
        app.grid_dirty = False
        app.midi_page_dirty = False
        app.mon_text = None
        app.pending_slot = None
        app.dirty = False
        app.status_dirty = False
        app.patch_name = KITS[0][0] if KITS else 'Init'
        app.source_slot = None
        app.slot = 1
        app.sync_touched = False
        app.page = 0
        app.sel = 0
        app.flash = [0] * NCH
        app.flash_step = -1
        app.tick_errors = 0
        app.diag_next = 0
        app.diag_max = 0
        app.seq_max = 0
        app.tick_t0 = ticks_ms()
        app.tick_gen = 0
        app.tick_last = ticks_ms()
        app.frame_last = 0
        app.tick_fallback = False
        app.tick_restarts = 0
        app.cpu_auto = True
        app.cpu_meter = hasattr(amy, 'render_load')
        app.cpu_load = None
        app.cpu_next = 0
        app.cpu_hold = 0
        app.cpu_calm = 0
        app.cpu_dirty = True
        app.eco_base = 0
        app.ext_synths = []
        app.spp = 0
        app.mmc = True

        if not getattr(app, 'app_dir', None):
            app.app_dir = tulip.pwd()            # files stay here even if the REPL cd()s away
        eng = GinTonic()
        eng.midi_out_fn = _midi_out
        _load_settings()
        app.bank = PresetBank(_path('gintonic_bank'))
        app.picker = None
        load_user_drums()
        if KITS:
            eng.set_state(kit_state(KITS[0], DRUM_BY_NAME), keep_tempo=True)

        def set_learn_target(k):
            app.learn_target = k
            for kn in app.all_knobs:
                kn.set_learn(kn.key == k)
            app.midi_page_dirty = True
        app.set_learn_target = set_learn_target

        def place(obj, x, y, w, h):
            app.add(obj, x=x, y=y)
            obj.group.set_size(w, h)
            obj.group.set_style_bg_color(_c(C_BG), 0)

        app.header = Header()
        place(app.header, 0, 0, 1024, 42)
        app.main = MainPage()
        place(app.main, 0, PAGE_Y, 1024, 600 - PAGE_Y)
        app.matrix = MatrixPage()
        place(app.matrix, 0, PAGE_Y, 1024, 600 - PAGE_Y)
        app.midi_page = MidiPage()
        place(app.midi_page, 0, PAGE_Y, 1024, 600 - PAGE_Y)
        app.tools = ToolsPage()
        place(app.tools, 0, PAGE_Y, 1024, 600 - PAGE_Y)
        app.pages = [app.main, app.matrix, app.midi_page, app.tools]
        app.all_knobs = app.main.knobs + app.matrix.knobs
        app.knob_by_key = {kn.key: kn for kn in app.all_knobs}
        app.focus_key = None
        set_focus('k:osc_freq')

        def show_page(j):
            app.page = j
            for i, pg in enumerate(app.pages):
                if i == j:
                    pg.group.remove_flag(lv.obj.FLAG.HIDDEN)
                else:
                    pg.group.add_flag(lv.obj.FLAG.HIDDEN)
            for i, b in enumerate(app.header.pages):
                b.set(i == j)
            if j == 0:
                app.main.refresh(knobs=False)
                app.knob_queue = list(app.all_knobs)
            elif j == 1:
                app.matrix.refresh()
                app.knob_queue = list(app.all_knobs)
            elif j == 2:
                app.midi_page.refresh()
        app.show_page = show_page

        def refresh_channel():
            # another channel selected: switches now, knobs a few per tick
            app.main.refresh(knobs=False)
            app.knob_queue = list(app.all_knobs)
            if app.page == 1:
                app.matrix.refresh()
            if app.learn_target is not None:
                set_learn_target(app.learn_target)
        app.refresh_channel = refresh_channel

        def refresh_all():
            if app.page == 0:
                app.main.refresh(knobs=False)
            elif app.page == 1:
                app.matrix.refresh()
            app.knob_queue = list(app.all_knobs)     # MAIN and SEQ knobs
            app.midi_page_dirty = True
        app.refresh_all = refresh_all

        if not load_slot(app.slot):
            app.header.update_slot_ui()
            app.status_dirty = True
        show_page(0)
        if eng.clock_ext or app.clock_out:
            _set_clock(eng.clock_ext, app.clock_out)
        else:
            apply_tempo()
        _mute_others(True)
        midi.add_callback(midi_cb)
        app.prev_sysex_cb = getattr(midi, 'sysex_callback', None)
        if app.prev_sysex_cb is sysex_cb:
            app.prev_sysex_cb = None
        midi.sysex_callback = sysex_cb
        app.seq_slot = tulip.seq_add_callback(_seq_cb, 0, 4)
        if app.seq_slot < 0:
            print('gintonix: no free sequencer slot (quit another music app)')
        app.present()
        tulip.defer(_tick, app.tick_gen, TICK_MS)
