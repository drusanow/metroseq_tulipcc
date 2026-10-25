"""Parity test: run the ORIGINAL Python engine (reference/metropolix_tulip.py)
on a virtual clock with stubbed Tulip/AMY modules, run the C engine (tests/sim)
on the same patterns, and check both produce the same CV: every gate rising
edge on every output, at the same time (within the Python's 1 ms defer
resolution), carrying the same pitch.

    python3 tests/py_parity.py            (after `make -C tests`)

Only deterministic patterns are compared (no RAND/BROWN order, prob 100%),
and swing is only used on /1 tracks, where the C port's swing-parity fix is
identical to the original behaviour.
"""
import heapq
import os
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
PPQ = 48                      # AMY sequencer ticks per quarter note
TOL_MS = 1.5                  # Python defers are integer ms


# --- virtual clock + Tulip / AMY stubs ---------------------------------------

class VClock:
    def __init__(self):
        self.now = 0.0
        self.q = []
        self.n = 0

    def defer(self, fn, arg, ms):
        self.n += 1
        heapq.heappush(self.q, (self.now + ms, self.n, fn, arg))

    def run_until(self, t):
        while self.q and self.q[0][0] <= t:
            when, _, fn, arg = heapq.heappop(self.q)
            self.now = when
            fn(arg)
        self.now = t


class _Any:
    """Absorbs any attribute access / call (stands in for lv.*)."""
    def __getattr__(self, name):
        return _Any()

    def __call__(self, *a, **k):
        return _Any()


def install_stubs(clock):
    tulip = types.ModuleType("tulip")
    tulip.UIScreen = object
    tulip.UIElement = object
    tulip.pal_to_lv = lambda c: c
    tulip.lv_depad = lambda o: None
    tulip.lv = _Any()
    tulip.defer = clock.defer
    amy = types.ModuleType("amy")
    amy.AMY_SEQUENCER_PPQ = PPQ
    amy.send = lambda **k: None
    amy.reset = lambda: None
    amy.SAW_DOWN = 0
    amy.FILTER_LPF = 0
    seq = types.ModuleType("sequencer")
    seq.tempo = lambda bpm: None
    seq.TulipSequence = lambda *a, **k: None
    sys.modules.update({"tulip": tulip, "amy": amy, "sequencer": seq})


def load_reference(clock):
    install_stubs(clock)
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "metropolix_ref", os.path.join(ROOT, "reference", "metropolix_tulip.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class App:
    pass


def py_run(scenario, dur_ms):
    clock = VClock()
    m = load_reference(clock)
    app = App()
    m.app = app
    app.bpm = 120
    app.playing = False
    app.page = -1
    app.rebuilding = False
    app.root = 0
    app.scale_idx = 0
    app.scale_ivals = m.SCALES[0][1]
    app.base_octave = 0
    app.merge_cv = False
    app._shared_high = False
    app._shared_owner = None
    app.ticks_per_16th = m.ticks_per_16th()
    app.clk = 0
    app._last16 = 0
    app._base_tick = None
    app._last_disp = [None] * m.NUM_TRACKS
    app._last_mod_disp = None
    app.header = app.seq_page = app.track_page = None
    app.mod_page = app.setup_page = None
    app.stages = [m.Stage() for _ in range(m.NUM_STAGES)]
    app.tracks = [m.Track(i) for i in range(m.NUM_TRACKS)]
    app.mod_lanes = [m.ModLane(i) for i in range(m.NUM_MOD_LANES)]
    app.mod_sel = 0
    app.mod_bus = {}
    m._load_default()
    snap = m._snapshot()
    scenario(snap)
    m._apply_snapshot(snap)

    rec = []
    m.cv.gate = lambda t, high: rec.append((clock.now, "GATE", t, 1 if high else 0))
    m.cv.pitch = lambda t, midi: rec.append((clock.now, "PITCH", t, float(midi)))

    app.playing = True
    m.start_playback()
    tick_ms = 60000.0 / app.bpm / PPQ
    x = 0
    while x * tick_ms <= dur_ms:
        clock.run_until(x * tick_ms)
        m._master_tick(x)
        x += 1
    return snap, rec


# --- C side -----------------------------------------------------------------

def write_pattern(snap, path):
    with open(path, "w") as f:
        f.write("global %d %d %d %d %d\n" % (snap["bpm"], snap["root"], snap["scale"],
                                             snap["boct"], 1 if snap["merge"] else 0))
        for i, s in enumerate(snap["stages"]):
            f.write("stage %d %d %d %d %d %d %d %d %d\n" % (
                i, s["p"], s["n"], s["g"], int(s["sl"]), int(s["sk"]), s["r"],
                int(round(s["pr"] * 100)), s["ac"]))
        for i, t in enumerate(snap["tracks"]):
            f.write("track %d %d %d %d %d %d %d %d %d %d\n" % (
                i, t["o"], t["l"], t["d"], int(round(t["sw"] * 100)),
                int(round(t["gf"] * 100)), t["tr"], t["oc"], int(t["inv"]), int(t["ma"])))
        for i, md in enumerate(snap["mods"]):
            vals = " ".join(str(int(round(v * 100))) for v in md["v"])
            f.write("mod %d %s %d %d %d %d %d\n" % (
                i, vals, md["o"], md["l"], md["d"], int(round(md["dp"] * 100)), md["ds"]))


def c_run(snap, dur_ms):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        path = f.name
    write_pattern(snap, path)
    out = subprocess.run([SIM, path, str(dur_ms)], check=True, capture_output=True,
                         text=True).stdout
    os.unlink(path)
    rec = []
    for line in out.splitlines():
        t, kind, o, v = line.split()
        if kind == "GATE":
            rec.append((int(t) / 1000.0, "GATE", int(o), int(v)))
        elif kind == "PITCH":
            rec.append((int(t) / 1000.0, "PITCH", int(o), int(v) / 100.0))
    return rec


# --- comparison --------------------------------------------------------------

def rising_edges(rec, outs=2):
    """[(time_ms, pitch_at_that_moment)] per physical output."""
    level = [0] * outs
    pitch = [None] * outs
    edges = [[] for _ in range(outs)]
    for t, kind, o, v in rec:
        if kind == "PITCH":
            pitch[o] = v
        elif v and not level[o]:
            edges[o].append((t, pitch[o]))
            level[o] = v
        else:
            level[o] = v
    return edges


def compare(name, scenario, dur_ms=8000):
    snap, py = py_run(scenario, dur_ms)
    cr = c_run(snap, dur_ms)
    pe, ce = rising_edges(py), rising_edges(cr)
    ok = True
    for o in range(2):
        # ignore edges in the last 100 ms (end-of-window boundary effects)
        a = [e for e in pe[o] if e[0] < dur_ms - 100]
        b = [e for e in ce[o] if e[0] < dur_ms - 100]
        if len(a) != len(b):
            print("  %s out%d: edge count python=%d C=%d" % (name, o, len(a), len(b)))
            ok = False
        for (ta, pa), (tb, pb) in zip(a, b):
            if abs(ta - tb) > TOL_MS or (pa is not None and pb is not None
                                         and abs(pa - pb) > 0.01):
                print("  %s out%d: python (%.2fms, %s) vs C (%.2fms, %s)"
                      % (name, o, ta, pa, tb, pb))
                ok = False
                break
    n = sum(len(e) for e in ce)
    print("%-28s %s  (%d gate edges compared)" % (name, "OK  " if ok else "FAIL", n))
    return ok


def sc_default(s):
    pass


def sc_ratchet_swing(s):
    s["stages"][2]["r"] = 3
    s["stages"][4]["r"] = 2
    s["stages"][6]["g"] = 1      # SING
    s["stages"][6]["n"] = 3
    s["tracks"][0]["sw"] = 0.3


def sc_orders_quant(s):
    s["root"], s["scale"], s["boct"] = 2, 2, -1
    s["tracks"][0]["o"] = 2      # PEND
    s["tracks"][0]["l"] = 5
    s["tracks"][1]["inv"] = True
    s["tracks"][1]["tr"] = -5
    s["stages"][3]["sk"] = True
    s["stages"][1]["ac"] = 2
    s["stages"][7]["g"] = 3      # REST


def sc_merge(s):
    s["merge"] = True
    s["tracks"][1]["d"] = 0
    s["tracks"][1]["gf"] = 0.3
    s["tracks"][0]["gf"] = 0.7


def sc_mod_lanes(s):
    s["mods"][0]["dp"] = 0.5
    s["mods"][0]["d"] = 0        # 1/16
    s["mods"][1]["v"] = [0.3, -0.3, 0.6, 0.0, -0.6, 0.3, 0.0, 0.6]
    s["mods"][1]["dp"] = 0.5
    s["mods"][1]["ds"] = 10      # T2 Pulses
    s["mods"][1]["o"] = 1        # REV
    s["mods"][2]["v"] = [0.2, 0.4, -0.2, 0.6, 0.0, -0.4, 0.2, 0.0]
    s["mods"][2]["dp"] = 0.75
    s["mods"][2]["ds"] = 3       # T1 Pitch
    s["mods"][2]["o"] = 2        # PEND
    s["mods"][2]["l"] = 6


def sc_slide_tempo(s):
    s["bpm"] = 173
    s["stages"][1]["sl"] = True
    s["stages"][5]["sl"] = True
    s["tracks"][0]["d"] = 4      # 1/4.
    s["tracks"][1]["d"] = 2      # 1/8.
    s["tracks"][1]["oc"] = -1


def main():
    if not os.path.exists(SIM):
        print("build the simulator first: make -C tests")
        return 2
    results = [
        compare("default pattern", sc_default),
        compare("ratchet + swing + SING", sc_ratchet_swing),
        compare("pendulum/skip/accum/quant", sc_orders_quant),
        compare("merge TRK2>TRK1 priority", sc_merge),
        compare("mod lanes (3 routed)", sc_mod_lanes),
        compare("slide + 173bpm + dotted", sc_slide_tempo),
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
