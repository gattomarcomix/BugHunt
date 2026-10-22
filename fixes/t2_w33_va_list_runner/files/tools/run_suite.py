#!/usr/bin/env python3
"""Regression suite: run every src/tests/*.c that has a .expect and compare.

  - stdout and stderr are merged (several expects contain error text)
  - lines containing '[babyc-ir]' are ignored
  - CRLF/LF are normalised on both sides (the expect files are mixed)
  - 032_args* and 078_args_double_ptr* get extra argv "- arg1 arg2 arg3 arg4"
  - *analyze* tests are run with --analyze (must not execute main)
  - cwd is the tests/ directory; timeout 20 s per test

usage:
  python3 tools/run_suite.py [path/to/src]     (default: ../src relative to this file)

env:
  BABYC / BABYC_BIN   path to binary (default: <src>/babyc then <src>/picoc)
  BABYC_PATH=token|tree|linear|bytecode  (preferred path axis)
  BABYC_ACCEL=0|1     master switch for shape/kernel fast-paths (default 1)
  BABYC_STRICT=0|1    no silent tree residual on linear/bytecode (default 0)
  Legacy: BABYC_NO_IR=1, BABYC_EXEC=vm|vm_strict, BABYC_VM_STRICT=1,
          BABYC_NO_SHAPE_FP / BABYC_NO_KERNEL
"""
import glob, os, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'src')
root = os.path.abspath(root)

def find_bin():
    # Prefer env, then /tmp (fuse artifacts often report X_OK but are not executable)
    candidates = []
    for key in ('BABYC', 'BABYC_BIN'):
        v = os.environ.get(key)
        if v:
            candidates.append(v)
    candidates.extend(['/tmp/babyc', '/tmp/picoc'])
    for name in ('babyc', 'picoc'):
        candidates.append(os.path.join(root, name))
    for p in candidates:
        if not p or not os.path.isfile(p):
            continue
        # real exec probe (os.access is unreliable on fuse)
        try:
            r = subprocess.run([p], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1)
            return p  # any exit is fine; PermissionError means not runnable
        except PermissionError:
            continue
        except Exception:
            return p  # other errors (e.g. missing argc) still mean binary runs
    return os.path.join(root, 'babyc')

picoc = find_bin()
if not os.access(picoc, os.X_OK):
    print('error: cannot execute binary:', picoc, file=sys.stderr)
    print('hint: cp src/babyc /tmp/babyc && chmod +x /tmp/babyc', file=sys.stderr)
    sys.exit(2)

os.chdir(os.path.join(root, 'tests'))
passed = failed = skipped = 0
fails = []

def norm(s):
    return '\n'.join(
        l for l in s.replace('\r\n', '\n').split('\n')
        if '[babyc-ir]' not in l and '[babyc-irb]' not in l  # irb tag legacy filter
        and '[babyc]' not in l and '[babyc-vm]' not in l and '[babyc-cov]' not in l
        and '[babyc-bytecode-vm]' not in l and '[babyc-exec-profile]' not in l
    ).strip()

def result_matches_expectation(output, expected, returncode):
    """Compare output, but never treat silent abnormal termination as success.

    Existing .expect files encode output rather than an explicit exit status.
    Therefore an empty expectation unambiguously means a silent successful run;
    non-empty diagnostic expectations retain the historical output-based check.
    """
    if norm(output) != norm(expected):
        return False
    if not norm(expected) and returncode != 0:
        return False
    return True

# Harness regression: an empty .expect must not hide a crash/nonzero exit.
assert result_matches_expectation('', '', 0)
assert not result_matches_expectation('', '', 1)
assert not result_matches_expectation('unexpected', '', 0)

for c in sorted(glob.glob('*.c')):
    exp = c[:-2] + '.expect'
    if not os.path.exists(exp):
        skipped += 1
        continue
    args = [picoc]
    if 'analyze' in c:
        args.append('--analyze')
    args.append(c)
    if c.startswith('032_args') or c.startswith('078_args_double_ptr') or c.startswith('1066_args_') or c.startswith('1068_args_'):
        args += ['-', 'arg1', 'arg2', 'arg3', 'arg4']
    try:
        r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=(35 if c == "283_bigblock_bodyir.c" else 20))
        out = r.stdout.decode('latin1', errors='replace')
        rc = r.returncode
    except subprocess.TimeoutExpired:
        out = '<TIMEOUT>'
        rc = -1
    # Token-only expectations are allowed for intentional diagnostic-format
    # differences, matching the dual-pass runner. Keep the semantic assertion;
    # do not infer a pass from a missing expectation.
    if (os.environ.get('BABYC_PATH') == 'token' or os.environ.get('BABYC_NO_IR')) and os.path.exists(c[:-2] + '.expect.noir'):
        exp = c[:-2] + '.expect.noir'
    e = open(exp, encoding='latin1', newline='').read()
    # Empty .expect means silent success, not merely no output: preserve the
    # process status so crashes/early exits cannot be counted as passes.
    if result_matches_expectation(out, e, rc):
        passed += 1
    elif "can't read file" in out and ".h" in out:
        # env-dependent system header (e.g. sqlite3.h) — not a product regression
        skipped += 1
    else:
        failed += 1
        fails.append(c)

# Dedicated structural-fast-path regression: check the hit counter as well as
# output equivalence against GCC and with the fast path disabled.
# These two structural checks intentionally exercise the string fast path and
# compact bytecode VM. A token-only run disables both mechanisms, so report them
# as skipped rather than as product failures or false passes.
if (os.environ.get('BABYC_PATH') == 'token' or os.environ.get('BABYC_NO_IR')):
    skipped += 2
    print('string-scan-fastpath: SKIP (BABYC_NO_IR disables this optimization)')
    print('sparse-local-snapshot: SKIP (BABYC_NO_IR disables compact bytecode)')
else:
    fp_test = os.path.join(here, 'test_string_scan_fastpath.py')
    try:
        fp_env = os.environ.copy()
        # The structural test must independently control the fast-path flags;
        # inherited benchmark settings must not suppress its required hit.
        fp_env.pop("BABYC_NO_KERNEL", None)
        fp_env.pop("BABYC_NO_SHAPE_FP", None)
        fp_env.pop("BABYC_ACCEL", None)
        fp = subprocess.run([sys.executable, fp_test, '--babyc', picoc],
                            env=fp_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=60)
        print(fp.stdout.strip())
        if fp.returncode == 0:
            passed += 1
        else:
            failed += 1
            fails.append('string-scan-fastpath-regression')
    except subprocess.TimeoutExpired:
        failed += 1
        fails.append('string-scan-fastpath-regression-TIMEOUT')

    # Structural VM regression: large local frame with sparse writes must execute
    # in compact/strict mode and preserve the same result as the linear executor.
    sparse_test = os.path.join(here, 'test_sparse_local_snapshot.py')
    try:
        sparse = subprocess.run([sys.executable, sparse_test, picoc],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=60)
        print(sparse.stdout.strip())
        if sparse.returncode == 0:
            passed += 1
        else:
            failed += 1
            fails.append('sparse-local-snapshot-regression')
    except subprocess.TimeoutExpired:
        failed += 1
        fails.append('sparse-local-snapshot-regression-TIMEOUT')


# W37: differential FP correctness and compact-bytecode coverage floor.
for name, argv in (
    ('fp-modes', [os.path.join(here, 'test_fp_modes.py'), '--babyc', picoc]),
    ('bytecode-vm', [os.path.join(here, 'test_bytecode_vm.py'), '--babyc', picoc]),
):
    if name == 'bytecode-vm' and (os.environ.get('BABYC_PATH') == 'token' or os.environ.get('BABYC_NO_IR')):
        skipped += 1
        print('bytecode-vm: SKIP (BABYC_NO_IR disables IR/bytecode)')
        continue
    try:
        gate = subprocess.run([sys.executable] + argv,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=900)
        summary = gate.stdout.strip().splitlines()
        print(summary[-1] if summary else name)
        if gate.returncode == 0:
            passed += 1
        else:
            failed += 1
            fails.append(name + '-regression')
    except subprocess.TimeoutExpired:
        failed += 1
        fails.append(name + '-TIMEOUT')

print('binary=%s' % picoc)
print('passed=%d failed=%d skipped=%d' % (passed, failed, skipped))
if fails:
    print(fails)
sys.exit(1 if failed else 0)
