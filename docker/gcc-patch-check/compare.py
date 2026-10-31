"""Compare compile.sh results of two compilers (env BASE, NEW) per configuration: files that
compile, internal compiler errors, identical assembly; for -fcall-saved-blink configurations,
check that every function in NEW's output that writes blink saves it, and restores it if it
returns. Usage: BASE=arc-gcc-vanilla NEW=arc-gcc python3 compare.py [config ...]"""
import os, re, sys
B = '.'
BASE, NEW = os.environ.get('BASE', 'arc-gcc-vanilla'), os.environ.get('NEW', 'arc-gcc')
def load(img, cfg):
    d = f'{B}/out/{img}/{cfg}'; r = {}
    for fn in os.listdir(d):
        if fn.endswith('.rc'):
            n = fn[:-3]; rc = int(open(f'{d}/{fn}').read() or 99)
            err = open(f'{d}/{n}.err', errors='replace').read()
            asm = open(f'{d}/{n}.s', errors='replace').read() if os.path.exists(f'{d}/{n}.s') and rc == 0 else None
            r[n] = (rc, 'internal compiler error' in err, asm)
    return r
WRITE = re.compile(r'^\s+(?!st|push|ld|pop|j|b|enter|leave|sub_s\s+sp|add_s\s+sp)([a-z0-9_.]+)\s+blink\s*,', re.M)
def functions(asm):
    name, body = None, []
    for line in asm.splitlines():
        m = re.match(r'^([A-Za-z_$][\w.$]*):$', line)
        if m and not m[1].startswith('.L'):
            if name: yield name, body
            name, body = m[1], []
        elif name: body.append(line)
    if name: yield name, body
WRITE1 = re.compile(r'^\s+(?!st|push|ld|pop|j|b[a-z]*\s|enter|leave)([a-z0-9_.]+)\s+blink\s*,')
def blink_check(asm):
    """Per function: if blink is written as a general register, the function must save and
    restore it (push/st to the stack, or enter_s/leave_s including blink)."""
    bad = []; used = 0
    for name, body in functions(asm):
        if not any(WRITE1.match(l) for l in body): continue
        used += 1
        t = '\n'.join(body)
        saves = re.search(r'push_s\s+blink|st(\.a|\.aw)?\s+blink,\s*\[sp|enter_s\s+\[[^\]]*blink', t)
        restores = re.search(r'pop_s\s+blink|ld(\.ab)?\s+blink,\s*\[sp|leave_s\s+\[[^\]]*blink', t)
        returns = re.search(r'\bj(_s)?(\.d)?\s+\[blink\]|leave_s\s+\[[^\]]*pcl', t)
        if not saves or (returns and not restores): bad.append(name)
    return used, bad
for cfg in sys.argv[1:] or ['rf16', 'full', 'rf16blink', 'rf16blinkOs', 'fullblink']:
    a, b = load(BASE, cfg), load(NEW, cfg)
    common = sorted(set(a) & set(b))
    ok_a = sum(a[n][0] == 0 for n in common); ok_b = sum(b[n][0] == 0 for n in common)
    ice_a = [n for n in common if a[n][1]]; ice_b = [n for n in common if b[n][1]]
    rc_diff = [n for n in common if (a[n][0] == 0) != (b[n][0] == 0)]
    same = sum(1 for n in common if a[n][2] is not None and a[n][2] == b[n][2])
    both = sum(1 for n in common if a[n][2] is not None and b[n][2] is not None)
    print(f'== {cfg}: {len(common)} files; compiled {BASE} {ok_a} / {NEW} {ok_b}; ICE {len(ice_a)} / {len(ice_b)}; '
          f'success differs {len(rc_diff)}; identical asm {same}/{both}')
    if rc_diff: print('   success differs:', rc_diff[:10])
    new_ice = sorted(set(ice_b) - set(ice_a))
    if new_ice: print('   NEW ICEs:', new_ice[:10])
    if 'blink' in cfg:
        files = funcs = 0; bad = []
        for n in common:
            if b[n][2] is None: continue
            u, bd = blink_check(b[n][2])
            if u: files += 1; funcs += u
            bad += [(n, x) for x in bd]
        print(f'   patched: {funcs} functions in {files} files allocate blink; missing save/restore: {len(bad)} {bad[:5]}')
