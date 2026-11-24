#!/usr/bin/env python3
# The chain's tools (translator, SPIRV-Cross, Metal compiler) against dmsh_chain.lock. The lock is part of the chain version; these checks decide no output, so are not.
# Usage: tool_lock.py --check-tools   on a mismatch: [RED] lines, exit 1
#        tool_lock.py --update-lock   rewrite the lock from the actual tools (after rebuilding the translator or spirv-cross, or changing Xcode; then dmsh_chain.py --header)
import os, sys, hashlib, subprocess, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dmsh_chain import LOCK, XLATE_DIR, SPIRV_CROSS, xlate_source_sha

def sha16(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16]

def tools_now():
    xl = os.path.join(XLATE_DIR, 'dmsh_xlate')
    t: dict = {'dmsh_xlate': sha16(xl) if os.path.isfile(xl) else 'missing'}
    t['xlate_source'] = xlate_embedded()
    sc = shutil.which(SPIRV_CROSS)
    t['spirv-cross'] = sha16(sc) if sc else 'missing'
    t['metal_macosx'] = (subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '--version'], capture_output=True, text=True).stdout.splitlines()[0:1] or ['missing'])[0]
    return t

NO_RUN = 'does not run'
def xlate_embedded():
    """or 'does not run (<its first error line>)', so a translator that cannot start is not read as a stale one"""
    try: r = subprocess.run([os.path.join(XLATE_DIR, 'dmsh_xlate'), '--source-sha'], capture_output=True, text=True, errors='replace', timeout=60)
    except (OSError, subprocess.SubprocessError) as e: return f'{NO_RUN} ({e})'
    if r.returncode == 0 and r.stdout.strip(): return r.stdout.strip()
    return '%s (%s)' % (NO_RUN, (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or [f'exit code {r.returncode}'])[0])

def no_run_message(embedded): return f'the translator {embedded}: {os.path.join(XLATE_DIR, "dmsh_xlate")} is the committed native executable (dm/tools/native_tools.sh builds it); check out the repository again if it is missing or damaged'

def check_tools():
    want = dict(l.rstrip('\n').split('\t', 1) for l in open(LOCK) if '\t' in l and not l.startswith('#'))
    now = tools_now()
    ran = not now['xlate_source'].startswith(NO_RUN)
    bad = [f'{k}: lock {want.get(k)} / actual {v}' for k, v in now.items() if want.get(k) != v and (ran or k != 'xlate_source')]
    if not ran: bad.append(no_run_message(now['xlate_source']))
    elif now['xlate_source'] != xlate_source_sha(): bad.append(f'the translator was not built from the in-repo source: embedded source {now["xlate_source"]} / in repo {xlate_source_sha()} (rebuild it: dm/tools/native_tools.sh dmsh_xlate)')
    return bad

if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['--update-lock']:
        t = tools_now()
        if t['xlate_source'].startswith(NO_RUN): sys.exit('[RED] ' + no_run_message(t['xlate_source']))
        if t['xlate_source'] != xlate_source_sha(): sys.exit(f'[RED] the translator was not built from the in-repo source (embedded {t["xlate_source"]} / in repo {xlate_source_sha()}): rebuild it first (dm/tools/native_tools.sh dmsh_xlate)')
        open(LOCK, 'w').write('# Translation chain tool fingerprints (written by tool_lock.py --update-lock; regenerate after changing tools and commit it)\n' + ''.join(f'{k}\t{v}\n' for k, v in t.items()))
        print('wrote', LOCK)
    elif a == ['--check-tools']:
        bad = check_tools()
        for b in bad: print('[RED] translation chain tools do not match the lock file: ' + b)
        sys.exit(1 if bad else 0)
    else: sys.exit('usage: tool_lock.py --check-tools | --update-lock')
