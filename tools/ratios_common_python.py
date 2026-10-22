#!/usr/bin/env python3
"""Ricalcola i rapporti tempo_Python/tempo_BabyC di piu' risultati JSON con lo STESSO riferimento Python
(best of 7, data/python_best_of_7.json), per non confondere il rumore di Python con un cambiamento di BabyC.
Uso: python3 ratios_common_python.py data/results_*.json"""
import json,math,sys
py=json.load(open("data/python_best_of_7.json"))
g=lambda xs: math.exp(sum(map(math.log,xs))/len(xs))
for f in sys.argv[1:]:
    R={r["bench"]:r for r in json.load(open(f))}
    print(f)
    for col in ("tree-nk","vm-nk","bc-nk"):
        if col not in next(iter(R.values())): continue
        out=[]
        for lab,pre in (("slice1","i"),("slice2","a"),("tutti","")):
            xs=[py[b]/R[b][col]["t"] for b in R if b.startswith(pre) and R[b][col]["ok"]]
            out.append("%s=%.2fx"%(lab,g(xs)))
        print("  %-8s %s"%(col," ".join(out)))
