#!/usr/bin/env python3
"""Suite 'real code' per BabyC: confronta albero (IR), VM, Python e gcc.

Uso:  python3 run_real_bench.py --babyc /path/babyc [--runs 3] [--timeout 120] [--only i0,a0]
Colonne BabyC (tutte con BABYC_NO_MEMO=1):
  tree      = default (kernel/fast-path attivi)
  tree-nk   = BABYC_NO_KERNEL=1          (albero 'onesto')
  vm        = BABYC_EXEC=vm              (kernel attivi)
  vm-nk     = BABYC_EXEC=vm + NO_KERNEL  (VM 'onesta': e' la colonna che conta)
Rapporto = tempo_Python / tempo_BabyC  (>1 = BabyC piu' veloce di Python).
Un caso con output diverso da gcc/Python NON entra nelle medie.
"""
import argparse, glob, math, os, re, subprocess, sys, time, json

NOISE = re.compile(r'^\[(babyc|babyc-cov|babyc-vm|babyc-ir|hy|fill-hy|call)\]')
HERE = os.path.dirname(os.path.abspath(__file__))

def run(cmd, env=None, timeout=120):
    t = time.perf_counter()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout, cwd=HERE)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT", time.perf_counter() - t
    out = "\n".join(l for l in r.stdout.replace("\r", "").split("\n") if not NOISE.match(l)).strip()
    return r.returncode, out, time.perf_counter() - t

def best(cmd, env, runs, timeout):
    b, out, rc = 1e18, None, None
    for _ in range(runs):
        rc, out, d = run(cmd, env, timeout)
        if out == "TIMEOUT":
            return rc, out, d
        b = min(b, d)
        if rc != 0:
            return rc, out, b
    return rc, out, b

def geomean(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float('nan')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--babyc", required=True)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--only", default="")
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    base = os.environ.copy(); base["BABYC_NO_MEMO"] = "1"
    cols = [("tree-nk", {"BABYC_NO_KERNEL": "1"}),
            ("vm-nk", {"BABYC_EXEC": "vm", "BABYC_NO_KERNEL": "1"}),
            ("bc-nk", {"BABYC_EXEC": "vm", "BABYC_BYTECODE_VM": "1", "BABYC_NO_KERNEL": "1"})]
    files = sorted(glob.glob(os.path.join(HERE, "[ia][0-9][0-9]_*.c")))
    if a.only:
        pre = tuple(a.only.split(","))
        files = [f for f in files if os.path.basename(f).startswith(pre)]

    rows = []
    hdr = "%-18s %7s %7s | %8s %8s %8s | %s" % ("bench", "gcc", "python", "tree-nk", "vm-nk", "bc-nk", "speedup vs Python (tree-nk / vm-nk / bc-nk)")
    print(hdr); print("-" * len(hdr))
    for f in files:
        name = os.path.basename(f)[:-2]
        exe = "/tmp/rb_" + name
        subprocess.run(["gcc", "-O2", "-w", "-o", exe, f], capture_output=True)
        _, ref, tg = best([exe], None, a.runs, a.timeout)
        _, pyo, tp = best(["python3", name + ".py"], None, a.runs, a.timeout)
        row = {"bench": name, "gcc": tg, "python": tp, "ref": ref, "py_ok": pyo == ref}
        cells, sp = [], []
        for cname, extra in cols:
            env = dict(base); env.update(extra)
            rc, out, t = best([a.babyc, name + ".c"], env, a.runs, a.timeout)
            ok = (out == ref and rc == 0)
            row[cname] = {"t": t, "ok": ok, "rc": rc}
            if out == "TIMEOUT": cells.append("%8s" % "TIMEOUT")
            elif ok: cells.append("%8.3f" % t)
            elif rc not in (0, None): cells.append("%8s" % ("rc%d" % rc))
            else: cells.append("%8s" % "DIFF")
        for cname in ("tree-nk", "vm-nk", "bc-nk"):
            c = row[cname]; sp.append("%5.2fx" % (tp / c["t"]) if c["ok"] else "  -  ")
        print("%-18s %7.4f %7.3f | %s | %s%s" % (name, tg, tp, " ".join(cells), " ".join(sp), "" if row["py_ok"] else "  [PY!=GCC]"))
        rows.append(row)

    print("\nMedia geometrica di (tempo_Python / tempo_BabyC), solo casi corretti:")
    for label, pref in (("slice 1 (interi/chiamate, i*)", "i"), ("slice 2 (array/puntatori, a*)", "a"), ("tutti", "")):
        line = "  %-32s" % label
        for cname, _ in cols:
            xs = [r["python"] / r[cname]["t"] for r in rows if r["bench"].startswith(pref) and r[cname]["ok"]]
            n = sum(1 for r in rows if r["bench"].startswith(pref))
            line += "  %s=%.2fx(%d/%d)" % (cname, geomean(xs), len(xs), n)
        print(line)
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)

if __name__ == "__main__":
    main()
