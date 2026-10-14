#!/usr/bin/env python3
"""Prepare an isolated, user-operated helper-only Phase 2A A/B switch.
Never swaps the compositor plugin. Never writes the phase-1 measurement directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import time


def ctl(*args):
    return subprocess.check_output(['hyprctl', *args], text=True).strip()


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def private(p):
    p.mkdir(parents=True, mode=0o700, exist_ok=True)
    s=p.lstat()
    if not stat.S_ISDIR(s.st_mode) or s.st_uid!=os.getuid() or stat.S_IMODE(s.st_mode)!=0o700:
        raise SystemExit(f'Not owner-private: {p}')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prepare',type=Path,required=True,metavar='BUILD_DIR')
    p.add_argument('--baseline',type=Path,required=True)
    p.add_argument('--out',type=Path,default=Path.home()/'.cache/hyprcapture-hdr-phase2a-20261013')
    a=p.parse_args();out=a.out.resolve()
    if 'hyprcapture-hdr-phase1' in str(out):
        p.error('Phase-1 paths are reserved; choose a distinct directory')
    private(out)
    if (out/'state.json').exists():
        p.error('State already prepared; use a fresh directory')
    original=json.loads(ctl('-j','getoption','plugin:hyprcapture:helper'))['str']
    for source,name in ((a.prepare.resolve()/'hyprcapture-ui','after'),(a.baseline.resolve(),'before')):
        if not source.is_file():
            p.error(f'Missing binary: {source}')
        private(out/name)
        shutil.copy2(source,out/name/'hyprcapture-ui');(out/name/'hyprcapture-ui').chmod(0o700)
    logs=Path('/dev/shm')/f'hyprcapture-{os.getuid()}'
    private(logs);logs=logs/f'phase2a-{time.time_ns()}';private(logs)
    state=dict(original_helper=original,before_sha256=sha(out/'before'/'hyprcapture-ui'),after_sha256=sha(out/'after'/'hyprcapture-ui'),
               logs=str(logs),version=ctl('version'),monitors=json.loads(ctl('-j','monitors')),
               plugin_list=json.loads(ctl('-j','plugins','list')))
    (out/'state.json').write_text(json.dumps(state,indent=2)+'\n')
    for mode,binary,enabled in (('before','before','0'),('cpu','after','0'),('after','after','1')):
        wrapper=out/(mode+'.sh')
        wrapper.write_text('#!/bin/sh\nexport HYPRCAPTURE_HDR_DMABUF='+enabled+'\nexport HYPRCAPTURE_TIMING_FILE='+shlex.quote(str(logs/(mode+'.log')))+'\nexec '+shlex.quote(str(out/binary/'hyprcapture-ui'))+' "$@" 2>>'+shlex.quote(str(logs/(mode+'-stderr.log')))+'\n')
        wrapper.chmod(0o700)
        script=out/('activate-'+mode+'.sh')
        expression='hl.config({plugin={hyprcapture={helper='+json.dumps(str(wrapper))+'}}})'
        script.write_text('#!/bin/sh\nset -eu\n# User-operated helper selection only. The live plugin is never swapped.\nif pgrep -x hyprcapture-ui >/dev/null; then echo "Close overlays before switching helpers" >&2; exit 1; fi\nprintf "%s  %s\\n" '+shlex.quote(state[binary+'_sha256'])+' '+shlex.quote(str(out/binary/'hyprcapture-ui'))+' | sha256sum -c -\nhyprctl eval '+shlex.quote(expression)+'\nhyprctl getoption plugin:hyprcapture:helper\n')
        script.chmod(0o700)
    expression='hl.config({plugin={hyprcapture={helper='+json.dumps(original)+'}}})'
    restore=out/'restore.sh'
    restore.write_text('#!/bin/sh\nset -eu\nif pgrep -x hyprcapture-ui >/dev/null; then echo "Close overlays before restoring" >&2; exit 1; fi\nhyprctl eval '+shlex.quote(expression)+'\nhyprctl getoption plugin:hyprcapture:helper\n')
    restore.chmod(0o700)
    print(out)
    print('Review state.json and activate-before/cpu/after.sh and restore.sh; no settings were applied.')
    print('Use before for original startup latency; cpu for the instrumented PQ fallback with presentation feedback; after for FP16.')
    print('Alternate trials, keep cold/warm separate, verify /proc/<pid>/exe hashes, and do not benchmark during compilation.')
    print('Pair overlay.first_paint / first_update with compositor openlayer events and hdr.presented CLOCK_MONOTONIC seconds/nanoseconds.')
    print('Record WAYLAND_DEBUG=client in a separate diagnostic run to inspect child attach/commit before the first parent buffer commit.')


if __name__=='__main__':
    main()
