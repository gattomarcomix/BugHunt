# Pacchetto unico BabyC: report, correzioni, test, riproduttori, benchmark, strumenti

Leggere per primo `REPORT_CONSOLIDATO.md`.

| cartella | contenuto |
|---|---|
| `REPORT_CONSOLIDATO.md` | cronologia delle valutazioni, correzioni, test, stato aperto |
| `reports/` | tutti i report singoli (da `REPORT_TEAM_2026-10-08` a `REPORT_TRANCHE3`) |
| `fixes/t1_w32_perf/` | tranche 1: chiamate annidate in VM (si applica a **W32**) |
| `fixes/t2_w33_va_list_runner/` | tranche 2: overflow variadiche e runner (si applica a **W33**) |
| `fixes/t3_w33_ptr_deref/` | tranche 3: `**p` con tipi a 8 byte nell'albero (si applica a **W33**, dopo t2) |
| `fixes/storico_gia_integrati/` | patch precedenti, già presenti negli snapshot successivi (solo riferimento) |
| `new_tests/` | 24 test di regressione (id 880-907 e 1200) con `.expect`; `open/` per i bug aperti |
| `repro/` | riproduttori con output atteso |
| `bench_real/` | 18 programmi C+Python verosimili, runner a 4 e 3 colonne |
| `tools/` | `asan_smoke.sh`, `compare_snapshots.py`, `ratios_common_python.py` |
| `data/` | risultati JSON delle misure |
| `piani_storici/` | piani scritti all'inizio (parzialmente superati) |
