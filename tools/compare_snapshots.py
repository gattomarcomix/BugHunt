#!/usr/bin/env python3
"""Confronto A/B a run alternati tra due eseguibili BabyC (stessa macchina, stesse condizioni).
Uso: python3 compare_snapshots.py A_babyc B_babyc [bench_dir] [--runs 5] [nomi bench...]
Modalita': VM lineare senza kernel (BABYC_EXEC=vm BABYC_NO_KERNEL=1 BABYC_NO_MEMO=1).
Alternare i run evita che il rumore del sandbox venga scambiato per una regressione."""
import subprocess,time,os,sys
a,b=sys.argv[1],sys.argv[2]; d=sys.argv[3] if len(sys.argv)>3 else "."
names=[x for x in sys.argv[4:] if not x.startswith("--")] or ["a01_sieve","a02_qsort","a03_heapsort","a04_matmul","a10_bsearch"]
e=os.environ.copy(); e.update({"BABYC_NO_MEMO":"1","BABYC_EXEC":"vm","BABYC_NO_KERNEL":"1"})
for n in names:
    t={a:[],b:[]}
    for _ in range(5):
        for x in (a,b):
            s=time.perf_counter(); subprocess.run([x,n+".c"],capture_output=True,env=e,cwd=d); t[x].append(time.perf_counter()-s)
    print("%-14s A min %.3f | B min %.3f | B/A %.2f"%(n,min(t[a]),min(t[b]),min(t[b])/min(t[a])))
