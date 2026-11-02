# gintonic_presets.py -- factory drum sounds and kits for GinTonic
#
# You can edit this file with a text editor (then install it again with
# install_gintonic.bat -> 2 = presets only).
#
# DRUMS: the drum sound library (DRUM menu on the MAIN page loads one into the
#        selected channel).  D(...) takes real units, like the Microtonic shows them:
#   osc=('sin'|'tri'|'saw', Hz)
#   mod=('dec', semitones, ms) | ('sine', semitones, Hz) | ('rand', semitones, Hz)
#       decaying: -96..+96 st, rate 10 ms..10 s (None = never moves);
#       sine / random: -48..+48 st, 0..2000 Hz / 0..20000 Hz
#   oatk, odec   oscillator attack / decay in ms
#   noise=('lp'|'bp'|'hp', Hz, Q, 'exp'|'lin'|'clap', attack ms, decay ms)
#   mix          noise share in % (0 = oscillator only, 100 = noise only)
#   eq=(Hz, dB)  dist (0..100)  level (dB, max +10)  pan (-100..100)
#   vel=(osc %, noise %, mod %)  (0..200, how much velocity / accent matter)
#   choke=True   stereo=True     (noise in stereo)
#
# The converted Microtonic presets are not in this file: they live in the
# 'kits' folder next to it (kits/*.json, one file per kit, Microtonic units).
#
# KITS: K(name, [8 drum names], patterns, tempo=, rate=, swing=, fill=, chain=)
#   patterns: {'A': [8 rows], ...}  one row per channel, 16 characters:
#     '.' = off   'x' = hit   'X' = accented hit   'f' = fill   'F' = accented fill
#   a row shorter than 16 = shorter pattern (all rows of a pattern same length)
#   chain='ABC' : patterns A, B, C play one after the other
#   rate: '1/8', '1/8T', '1/16', '1/16T', '1/32'   swing 0..100 %   fill 2..8 (x per step)
import math


def _k_freq(hz):
    return max(0.0, min(1.0, math.log(hz / 20.0) / math.log(1000.0)))


def _k_dec(ms):
    return max(0.0, min(1.0, math.log(max(ms, 10.0) / 10.0) / math.log(1000.0)))


def _k_att(ms):
    return max(0.0, min(1.0, (ms / 10000.0) ** (1.0 / 3.0)))


def _k_q(q):
    return max(0.0, min(1.0, math.log(q / 0.1) / math.log(10.0) / 5.0))


def D(osc=('sin', 632), mod=('dec', 0, 353), oatk=0, odec=316, noise=('lp', 20000, 0.707, 'exp', 0, 316),
      mix=0, eq=(632, 0), dist=0, level=0, pan=0, vel=(0, 0, 0), choke=False, stereo=False):
    p = {}
    p['wave'] = {'sin': 0, 'tri': 1, 'saw': 2}[osc[0]]
    p['osc_freq'] = _k_freq(osc[1])
    mode = {'dec': 0, 'sine': 1, 'rand': 2}[mod[0]]
    p['mod_mode'] = mode
    st = mod[1]
    r = mod[2]
    if mode == 0:
        p['mod_amt'] = 0.5 + st / 192.0
        p['mod_rate'] = 0.0 if r is None else max(0.0, min(1.0, 1.0 - math.log(max(r, 10.0) / 10.0) / math.log(1248.0)))
    else:
        p['mod_amt'] = 0.5 + st / 96.0
        if not r:
            p['mod_rate'] = 0.0
        elif mode == 1:
            p['mod_rate'] = max(0.0, min(1.0, math.log(r / 0.1) / math.log(20000.0)))
        else:
            p['mod_rate'] = max(0.0, min(1.0, math.log(max(r, 1.0)) / math.log(20000.0)))
    p['osc_att'] = _k_att(oatk)
    p['osc_dec'] = _k_dec(odec)
    nm, nf, nq, ne, na, nd = noise
    p['n_mode'] = {'lp': 0, 'bp': 1, 'hp': 2}[nm]
    p['n_freq'] = _k_freq(nf)
    p['n_q'] = _k_q(nq)
    p['n_env'] = {'exp': 0, 'lin': 1, 'clap': 2}[ne]
    p['n_att'] = _k_att(na)
    p['n_dec'] = _k_dec(nd * 1.5 if ne == 'lin' else nd)
    p['mix'] = max(0.0, min(1.0, mix / 100.0))
    p['eq_freq'] = _k_freq(eq[0])
    p['eq_gain'] = max(0.0, min(1.0, 0.5 + eq[1] / 80.0))
    p['dist'] = max(0.0, min(1.0, dist / 100.0))
    p['level'] = max(0.0, min(1.0, 0.5623 * math.pow(10.0, level / 40.0)))
    p['pan'] = max(0.0, min(1.0, 0.5 + pan / 200.0))
    p['vel_osc'] = vel[0] / 200.0
    p['vel_noise'] = vel[1] / 200.0
    p['vel_mod'] = vel[2] / 200.0
    p['choke'] = 1 if choke else 0
    p['n_stereo'] = 1 if stereo else 0
    return p


DRUMS = [
    # --- kicks
    ('Gin Kick', D(osc=('sin', 48), mod=('dec', 30, 55), odec=520, noise=('lp', 3500, 0.7, 'exp', 0, 12),
                   mix=8, eq=(70, 4), level=2, vel=(30, 60, 40))),
    ('808 Kick', D(osc=('sin', 45), mod=('dec', 24, 70), odec=900, mix=0, eq=(55, 3), level=2, vel=(40, 0, 30))),
    ('909 Kick', D(osc=('tri', 52), mod=('dec', 36, 32), odec=330, noise=('hp', 2500, 0.7, 'exp', 0, 9),
                   mix=12, eq=(90, 5), dist=12, level=1, vel=(30, 60, 30))),
    ('Punch Kick', D(osc=('sin', 58), mod=('dec', 40, 22), odec=230, noise=('lp', 5000, 0.7, 'exp', 0, 14),
                     mix=15, dist=35, eq=(120, 4), level=-2, vel=(25, 50, 20))),
    ('Hard Kick', D(osc=('sin', 52), mod=('dec', 48, 40), odec=260, mix=0, dist=45, eq=(3000, -10),
                    level=-5, vel=(20, 0, 20))),
    ('Sub Boom', D(osc=('sin', 38), mod=('dec', 12, 120), oatk=3, odec=1300, mix=0, level=-3)),
    # --- snares / claps / rims
    ('Tonic Snare', D(osc=('tri', 185), mod=('dec', 9, 45), odec=110, noise=('bp', 3800, 0.9, 'exp', 0, 190),
                      mix=58, eq=(5000, 3), level=-2, vel=(40, 60, 0))),
    ('808 Snare', D(osc=('sin', 238), mod=('dec', 5, 30), odec=95, noise=('hp', 1800, 0.7, 'exp', 0, 160),
                    mix=50, level=-3, vel=(40, 50, 0))),
    ('909 Snare', D(osc=('tri', 205), mod=('dec', 12, 25), odec=80, noise=('bp', 5200, 0.6, 'exp', 0, 240),
                    mix=65, dist=10, eq=(220, 3), level=-2, vel=(40, 70, 0))),
    ('Gated Snare', D(osc=('tri', 180), mod=('dec', 7, 40), odec=90, noise=('bp', 3000, 0.8, 'lin', 0, 260),
                      mix=70, level=-3, stereo=True, vel=(30, 40, 0))),
    ('Clap', D(osc=('sin', 1000), odec=10, noise=('bp', 1150, 1.6, 'clap', 22, 260), mix=100,
               eq=(2400, 4), level=-1, stereo=True, vel=(0, 60, 0))),
    ('Rim Shot', D(osc=('tri', 470), mod=('dec', 6, 15), odec=28, noise=('bp', 3200, 2.5, 'exp', 0, 14),
                   mix=35, eq=(1800, 4), level=-4, vel=(40, 40, 0))),
    ('Snap', D(osc=('sin', 2300), mod=('dec', -12, 20), odec=18, noise=('hp', 3500, 0.7, 'exp', 1, 55),
               mix=75, level=-5)),
    # --- hats / cymbals / shakers
    ('Closed Hat', D(osc=('sin', 7100), mod=('sine', 30, 1650), odec=40, noise=('hp', 8000, 0.8, 'exp', 0, 42),
                     mix=72, eq=(10000, 3), level=-3, choke=True, vel=(60, 60, 0))),
    ('Open Hat', D(osc=('sin', 7100), mod=('sine', 30, 1650), odec=300, noise=('hp', 7600, 0.8, 'exp', 0, 420),
                   mix=72, eq=(10000, 3), level=-5, choke=True, vel=(50, 50, 0))),
    ('Pedal Hat', D(osc=('sin', 6600), mod=('sine', 28, 1500), odec=60, noise=('hp', 6200, 1.2, 'lin', 6, 70),
                    mix=80, level=-9, choke=True)),
    ('Metal Hat', D(osc=('saw', 3150), mod=('sine', 41, 1210), odec=55, noise=('hp', 9000, 0.7, 'exp', 0, 50),
                    mix=55, eq=(7000, 6), level=0, vel=(50, 50, 0))),
    ('Ride', D(osc=('sin', 4900), mod=('sine', 36, 1110), odec=900, noise=('bp', 9500, 0.9, 'exp', 0, 700),
               mix=45, eq=(6000, 3), level=-7, stereo=True, vel=(40, 40, 0))),
    ('Crash', D(osc=('saw', 3750), mod=('sine', 44, 1810), odec=1100, noise=('hp', 5200, 0.7, 'exp', 4, 1700),
                mix=70, eq=(8000, 4), level=-9, stereo=True)),
    ('Shaker', D(osc=('sin', 1000), odec=10, noise=('bp', 7200, 1.4, 'lin', 25, 75), mix=100, level=-8,
                 stereo=True, vel=(0, 80, 0))),
    ('Tambourine', D(osc=('sin', 6100), mod=('rand', 30, 9000), odec=140, noise=('hp', 8500, 1.0, 'exp', 2, 150),
                     mix=60, level=-9)),
    # --- toms / percussion
    ('Low Tom', D(osc=('sin', 92), mod=('dec', 12, 110), odec=420, noise=('lp', 1200, 0.7, 'exp', 0, 40),
                  mix=8, level=-1, pan=-30, vel=(40, 40, 20))),
    ('Mid Tom', D(osc=('sin', 138), mod=('dec', 12, 100), odec=360, noise=('lp', 1600, 0.7, 'exp', 0, 35),
                  mix=8, level=-2, vel=(40, 40, 20))),
    ('High Tom', D(osc=('sin', 205), mod=('dec', 12, 90), odec=300, noise=('lp', 2200, 0.7, 'exp', 0, 30),
                   mix=8, level=-3, pan=30, vel=(40, 40, 20))),
    ('Conga', D(osc=('sin', 300), mod=('dec', 7, 22), odec=190, noise=('bp', 1500, 1.5, 'exp', 0, 12),
                mix=10, level=-3, pan=25, vel=(50, 50, 30))),
    ('Bongo', D(osc=('sin', 470), mod=('dec', 5, 18), odec=120, noise=('bp', 2400, 1.5, 'exp', 0, 10),
                mix=10, level=-5, pan=-25, vel=(50, 50, 30))),
    ('Tabla', D(osc=('sin', 260), mod=('dec', -9, 160), odec=380, noise=('bp', 900, 2.0, 'exp', 0, 20),
                mix=10, level=-3, vel=(50, 40, 60))),
    ('Cowbell', D(osc=('saw', 562), mod=('sine', 7, 270), odec=230, noise=('bp', 840, 6.0, 'exp', 0, 200),
                  mix=45, eq=(840, 8), level=-2, vel=(40, 40, 0))),
    ('Clave', D(osc=('sin', 2450), odec=32, mix=0, level=-6, vel=(40, 0, 0))),
    ('Wood Block', D(osc=('tri', 1150), mod=('dec', 3, 10), odec=45, noise=('bp', 1100, 4.0, 'exp', 0, 12),
                     mix=25, level=-6)),
    # --- electronic / fx
    ('Zap', D(osc=('sin', 180), mod=('dec', 60, 80), odec=180, mix=0, level=-4, vel=(30, 0, 50))),
    ('Laser', D(osc=('saw', 300), mod=('dec', 48, 150), odec=260, mix=0, eq=(2000, -12), level=-9)),
    ('Blip', D(osc=('tri', 880), odec=60, mix=0, level=-7)),
    ('Noise Burst', D(noise=('bp', 2200, 3.0, 'lin', 0, 140), mix=100, dist=40, level=-6, stereo=True)),
    ('FM Bell', D(osc=('sin', 660), mod=('sine', 24, 935), odec=900, mix=0, level=-3, vel=(30, 0, 60))),
    ('Bubble', D(osc=('sin', 520), mod=('rand', 12, 40), odec=260, mix=0, level=-6)),
    ('Riser', D(osc=('saw', 200), mod=('dec', -36, 600), oatk=300, odec=200, noise=('hp', 3000, 0.7, 'exp', 400, 200),
                mix=40, level=-10)),
    ('Glitch', D(osc=('saw', 90), mod=('rand', 30, 3000), odec=90, noise=('bp', 600, 5.0, 'exp', 0, 60),
                 mix=40, dist=60, level=-9)),
    ('Init', D()),
]


def K(name, drums, patterns, tempo=120, rate='1/16', swing=0, fill=2, chain=''):
    return (name, {'drums': drums, 'patterns': patterns, 'tempo': tempo, 'rate': rate,
                   'swing': swing, 'fill': fill, 'chain': chain})


KITS = [
    K('Gin & Tonic',
      ['Gin Kick', 'Tonic Snare', 'Closed Hat', 'Open Hat', 'Clap', 'Low Tom', 'Rim Shot', 'Cowbell'],
      {'A': ['X.....x...X.....',
             '....X.......X...',
             'x.x.x.X.x.x.x.X.',
             '..............x.',
             '....x.......x...',
             '................',
             '...x.....x....x.',
             '................'],
       'B': ['X.....x...X..x..',
             '....X.......X..f',
             'x.xxx.X.x.x.x.X.',
             '..............x.',
             '....x.......x...',
             '.............x..',
             '...x.....x......',
             '..x.....x.......'],
       'C': ['X.....x...X.....',
             '....X...x...F...',
             'x.x.x.x.x.x.....',
             '..........x.....',
             '....x.......xxxx',
             '............x.x.',
             '................',
             '................'],
       'D': ['X...............',
             '..........f.FFFF',
             'x.x.x.x.........',
             '................',
             '................',
             '..........xx..x.',
             '................',
             '................']},
      tempo=118, swing=12, fill=3, chain='AB'),
    K('808 Garden',
      ['808 Kick', '808 Snare', 'Closed Hat', 'Open Hat', 'Clap', 'Conga', 'Clave', 'Cowbell'],
      {'A': ['X......x..x.....',
             '....X.......X...',
             'x.xxx.x.x.x.xx.x',
             '......x.......x.',
             '............X...',
             '.......x.x....x.',
             '...x......x.....',
             '................'],
       'B': ['X......x..x..x..',
             '....X.......X...',
             'x.xxx.x.x.x.xx.x',
             '......x.......x.',
             '....x.......X...',
             '.x.....x.x....x.',
             '...x......x.....',
             'x.....x.....x...']},
      tempo=98, swing=28, chain='AB'),
    K('909 Club',
      ['909 Kick', '909 Snare', 'Closed Hat', 'Open Hat', 'Clap', 'Ride', 'Rim Shot', 'Crash'],
      {'A': ['X...X...X...X...',
             '................',
             'x.x.x.x.x.x.x.x.',
             '..X...X...X...X.',
             '....X.......X...',
             '................',
             '................',
             '................'],
       'B': ['X...X...X...X...',
             '............x..f',
             'xxx.xxx.xxx.xxx.',
             '..X...X...X...X.',
             '....X.......X...',
             'x.x.x.x.x.x.x.x.',
             '..x.....x....x..',
             '................'],
       'C': ['X...X...X...X...',
             '....x..xx..xFFFF',
             '................',
             '..X...X...X...X.',
             '....X.......X...',
             'x.x.x.x.x.x.x.x.',
             '................',
             'X...............']},
      tempo=126, chain='AB'),
    K('Electro Break',
      ['Punch Kick', 'Gated Snare', 'Metal Hat', 'Zap', 'Clap', 'Laser', 'Blip', 'Bubble'],
      {'A': ['X.....X...X..x..',
             '....X.......X...',
             'x.xxx.xxx.xxx.xx',
             '.......x........',
             '....x.......x...',
             '..............x.',
             'x..x..x..x..x...',
             '................'],
       'B': ['X.....X...X.X...',
             '....X.......X.ff',
             'x.xxx.xxx.xxx.xx',
             '.......x......x.',
             '....x.......x...',
             '..x.......x.....',
             'x..x..x..x..x..x',
             '.....x.....x....']},
      tempo=112, swing=8, chain='AB'),
    K('Minimal Tonic',
      ['Sub Boom', 'Snap', 'Pedal Hat', 'Shaker', 'Rim Shot', 'Clave', 'Wood Block', 'FM Bell'],
      {'A': ['X.......X.......',
             '........x.......',
             '..x...x...x...x.',
             'x.xxx.xxx.xxx.xx',
             '...x.....x...x..',
             '.....x.......x..',
             '..........x.....',
             'x...............'],
       'B': ['X.......X..x....',
             '........x.....x.',
             '..x...x...x...x.',
             'x.xxx.xxx.xxx.xx',
             '...x.....x...x..',
             '.....x...x...x..',
             '......x...x.....',
             '........x.......']},
      tempo=122, swing=18, chain='AB'),
    K('Industrial Tonic',
      ['Hard Kick', 'Noise Burst', 'Metal Hat', 'Glitch', 'Clap', 'Tabla', 'Crash', 'Riser'],
      {'A': ['X..X..X...X..X..',
             '....X.......X...',
             'xxxxxxxxxxxxxxxx',
             '..x.....x.x.....',
             '....X.......X...',
             '.x....x.....x...',
             'x...............',
             '................'],
       'B': ['X..X..X...X..XXX',
             '....X..x....X.FF',
             'xxxxxxxxxxxxxxxx',
             '..x.x...x.x.x...',
             '....X.......X...',
             '.x....x.....x..x',
             '................',
             '........x.......']},
      tempo=136, fill=4, chain='AB'),
    K('Latin Tonic',
      ['Gin Kick', 'Rim Shot', 'Closed Hat', 'Conga', 'Bongo', 'Cowbell', 'Clave', 'Tambourine'],
      {'A': ['X..x..X.X..x..X.',
             '.......x......x.',
             'x.xxx.xxx.xxx.xx',
             '..x.X..x..x.X..x',
             'x...x.x.x...x.x.',
             'X...x...X...x...',
             'x..x...x..x.x...',
             '....x.......x...']},
      tempo=104, swing=20),
]
