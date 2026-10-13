#!/usr/bin/env python3
"""Prepare reviewable activation/restoration scripts; record a manually driven trial.
This Python program never changes compositor configuration or loads/unloads plugins.
Only the generated scripts do so, when the user explicitly runs them.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import time


def ctl(*args):
    return subprocess.check_output(['hyprctl', *args], text=True).strip()


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def private_dir(path):
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    st = path.lstat()
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or stat.S_IMODE(st.st_mode) != 0o700:
        raise SystemExit(f'Need owner-private directory: {path}')


def lua_string(value):
    if any(ord(c) < 32 or ord(c) > 126 for c in value):
        raise SystemExit('Command paths must use printable ASCII')
    return json.dumps(value)


def plugin_paths():
    found = set()
    for comm in Path('/proc').glob('[0-9]*/comm'):
        try:
            if comm.read_text().strip() == 'Hyprland':
                for line in (comm.parent / 'maps').read_text().splitlines():
                    parts = line.split(maxsplit=5)
                    if len(parts) == 6:
                        path = Path(parts[-1])
                        if path.suffix != '.so' or (str(path).startswith('/usr/lib/') and 'capture' not in path.name.lower()):
                            continue
                        try:
                            data = path.read_bytes()
                            if b'Hyprland-only screenshot overlay' in data and b'pluginAPIVersion' in data:
                                found.add(str(path))
                        except OSError:
                            pass
        except (OSError, ProcessLookupError):
            pass
    return sorted(found)


def prepare(build, out, original_plugin=None):
    build = build.resolve()
    private_dir(out)
    if (out / "state.json").exists():
        raise SystemExit("Prepared state already exists; use a new --out to preserve restoration data")
    runtime = Path('/dev/shm') / f'hyprcapture-{os.getuid()}'
    private_dir(runtime)
    logs = runtime / f'hdr-phase1-{time.time_ns()}'
    private_dir(logs)
    originals = plugin_paths()
    if original_plugin is not None:
        chosen = str(original_plugin.resolve())
        if chosen not in originals:
            raise SystemExit(f'Specified original plugin is not mapped: {chosen}')
        originals = [chosen]
    if len(originals) != 1:
        raise SystemExit(f'Ambiguous mappings: {originals}. Supply --original-plugin only when its identity is known; mappings can outlive an unloaded module.')
    active = [p for p in json.loads(ctl('-j', 'plugins', 'list')) if p['name'] == 'HyprCapture']
    if len(active) != 1:
        raise SystemExit('Need exactly one registered HyprCapture plugin')
    old = {}
    for key in ('helper', 'timing', 'timing_file'):
        old[key] = json.loads(ctl('-j', 'getoption', f'plugin:hyprcapture:{key}'))
    for name in ('hyprcapture-ui', 'hyprcapture.so'):
        source = build / name
        if name == 'hyprcapture.so' and not source.is_file():
            source = build / 'libhyprcapture.so'
        if not source.is_file():
            raise SystemExit(f'Missing build artifact: {source}')
        dest = out / name
        shutil.copy2(source, dest)
        dest.chmod(0o700)
    helper = out / 'helper.sh'
    helper.write_text('#!/bin/sh\nexport HYPRCAPTURE_TIMING_FILE=' + shlex.quote(str(logs / 'ui.log')) + '\nexec ' + shlex.quote(str(out / 'hyprcapture-ui')) + ' "$@" 2>>' + shlex.quote(str(logs / 'helper-stderr.log')) + '\n')
    helper.chmod(0o700)
    original = Path(originals[0])
    shutil.copy2(original, out / 'original-plugin.so')
    (out / 'original-plugin.so').chmod(0o700)
    state = dict(active_plugin_handle=active[0]['handle'], original_plugin=str(original), original_plugin_sha256=digest(original), old_options=old,
                 logs=str(logs), prepared=time.time(), version=ctl('version'),
                 new_plugin_sha256=digest(out / 'hyprcapture.so'), new_helper_sha256=digest(out / 'hyprcapture-ui'))
    (out / 'state.json').write_text(json.dumps(state, indent=2) + '\n')
    config = 'hl.config({plugin={hyprcapture={helper=' + lua_string(str(helper)) + ',timing=true,timing_file=' + lua_string(str(logs / 'plugin.log')) + '}}})'
    restore = 'hl.config({plugin={hyprcapture={helper=' + lua_string(old['helper']['str']) + ',timing=' + str(old['timing']['bool']).lower() + ',timing_file=' + lua_string(old['timing_file']['str']) + '}}})'
    for name, unload, load, expression in (
            ('activate.sh', original, out / 'hyprcapture.so', config),
            ('restore.sh', out / 'hyprcapture.so', original, restore)):
        text = '#!/bin/sh\nset -eu\n# Explicit user action: swaps the live plugin and runtime options.\n'
        text += 'if pgrep -x hyprcapture-ui >/dev/null; then echo "Close capture overlays before swapping the plugin" >&2; exit 1; fi\n'
        if name == 'activate.sh':
            check = 'import json,sys; p=[p for p in json.load(sys.stdin) if p["name"]=="HyprCapture"]; assert len(p)==1 and p[0]["handle"]==' + repr(state['active_plugin_handle']) + ', "Plugin registration changed; prepare again"'
            text += 'hyprctl -j plugins list | python3 -c ' + shlex.quote(check) + '\n'
            text += 'printf "%s  %s\\n" ' + shlex.quote(state['original_plugin_sha256']) + ' ' + shlex.quote(str(original)) + ' | sha256sum -c -\n'
        if name == 'restore.sh':
            text += 'restore_plugin=' + shlex.quote(str(original)) + '\n'
            text += 'if ! printf "%s  %s\\n" ' + shlex.quote(state['original_plugin_sha256']) + ' ' + shlex.quote(str(original)) + ' | sha256sum -c -; then restore_plugin=' + shlex.quote(str(out / 'original-plugin.so')) + '; fi\n'
        text += 'hyprctl plugin unload ' + shlex.quote(str(unload)) + '\n'
        empty_check = 'import json,sys; assert not any(p["name"]=="HyprCapture" for p in json.load(sys.stdin)), "Plugin unload failed; no replacement was loaded"'
        text += 'hyprctl -j plugins list | python3 -c ' + shlex.quote(empty_check) + '\n'
        text += 'hyprctl plugin load ' + ('"$restore_plugin"' if name == 'restore.sh' else shlex.quote(str(load))) + '\n'
        loaded_check = 'import json,sys; p=[p for p in json.load(sys.stdin) if p["name"]=="HyprCapture"]; assert len(p)==1, "Plugin load failed; run restore.sh"'
        text += 'hyprctl -j plugins list | python3 -c ' + shlex.quote(loaded_check) + '\n'
        text += 'hyprctl eval ' + shlex.quote(expression) + '\n'
        text += 'hyprctl configerrors\nhyprctl getoption plugin:hyprcapture:helper\n'
        if name == 'activate.sh':
            text += 'hyprctl -j plugins list > ' + shlex.quote(str(out / 'active-after-activate.json')) + '\n'
        (out / name).write_text(text)
        (out / name).chmod(0o700)
    print(f'Prepared {out}\nReview state.json, activate.sh and restore.sh. Nothing was applied.\n')
    print('1. Close capture overlays; run activate.sh yourself.\n2. Run --record hdr-window --seconds 30, then open screenshot, select window, scroll once and cancel.\n3. Repeat hdr-region, hdr-window-png (SDR export on HDR monitor), sdr-window and sdr-region, 5 trials each.\n   Switch monitor HDR/SDR yourself; preserve target dimensions/content and export format.\n4. Close overlay; run restore.sh yourself. Configuration files are never edited.')


def record(out, label, seconds):
    state = json.loads((out / 'state.json').read_text())
    start = time.monotonic_ns() // 1000
    manifest = dict(label=label, start_us=start, monitor=json.loads(ctl('-j', 'monitors')),
                    plugin_paths=plugin_paths(), plugin_list=ctl('plugins', 'list'), version=ctl('version'),
                    helper_option=json.loads(ctl('-j', 'getoption', 'plugin:hyprcapture:helper')), processes=[])
    manifest['plugin_hashes'] = {p: digest(p) for p in manifest['plugin_paths']}
    manifest['registered_plugins'] = json.loads(ctl('-j', 'plugins', 'list'))
    activation = out / 'active-after-activate.json'
    manifest['activated_registration'] = json.loads(activation.read_text()) if activation.exists() else []
    print(f'Recording {seconds}s: now open capture, select target, scroll once, then cancel.', flush=True)
    seen = set()
    while time.monotonic_ns() // 1000 < start + seconds * 1000000:
        for comm in Path('/proc').glob('[0-9]*/comm'):
            try:
                if comm.read_text().strip() != 'hyprcapture-ui':
                    continue
                exe = (comm.parent / 'exe').resolve(strict=True)
                key = (comm.parent.name, str(exe))
                if key not in seen:
                    seen.add(key)
                    manifest['processes'].append(dict(pid=int(key[0]), exe=str(exe), sha256=digest(exe), observed_us=time.monotonic_ns() // 1000))
            except (OSError, ProcessLookupError):
                pass
        time.sleep(.2)
    manifest['end_us'] = time.monotonic_ns() // 1000
    run = out / f'{label}-{time.time_ns()}'
    private_dir(run)
    (run / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for name in ('ui.log', 'plugin.log', 'helper-stderr.log'):
        path = Path(state['logs']) / name
        if path.exists():
            shutil.copy2(path, run / name)
    summarize(run)
    print(run)


def read_events(path):
    events = []
    if not path.exists():
        return events
    for line in path.read_text(errors='replace').splitlines():
        mono = re.search(r'monotonic_us=(\d+)', line)
        if not mono:
            continue
        # Both UI and plugin use this monotonic clock; wall clocks are irrelevant.
        events.append((int(mono[1]), line))
    return sorted(events)


def summarize(run):
    manifest = json.loads((run / 'manifest.json').read_text())
    events = sorted(read_events(run / 'ui.log') + read_events(run / 'plugin.log'))
    events = [(t, s) for t, s in events if manifest['start_us'] <= t <= manifest['end_us']]
    starts = [(t, s) for t, s in events if ' scroll.start hdr=' in s]
    summaries = []
    for index, (started, line) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else manifest['end_us'] + 1
        part = [(t, s) for t, s in events if started <= t < end]
        def first(name, after=started):
            return next((t for t, s in part if t >= after and name in s), None)
        begin = first('event=scroll.input_begin')
        paint = first('scroll.thumbnail_paint_begin seq=', begin or started)
        fields = {}
        for name in ('scroll.baseline_ready', 'scroll.begin_received', 'event=scroll.baseline_ack', 'scroll.thumbnail_paint_begin'):
            stamp = first(name, begin or started)
            fields[name + '_from_input_ms'] = (stamp - begin) / 1000 if stamp is not None and begin is not None else None
        fields['input_to_thumbnail_paint_begin_ms'] = (paint - begin) / 1000 if begin is not None and paint is not None else None
        fields['prewarmed_baseline_ms_before_input'] = ((begin - first('scroll.baseline_ready')) / 1000 if begin is not None and first('scroll.baseline_ready') is not None and first('scroll.baseline_ready') < begin else None)
        paint_line = next((s for t, s in part if t == paint and 'scroll.thumbnail_paint_begin' in s), '')
        match = re.search(r'seq=(\d+)', paint_line)
        sequence = int(match[1]) if match else None
        fields['first_painted_sequence'] = sequence
        frame_events = [(t, s) for t, s in part if sequence is not None and re.search(rf'\bseq={sequence}\b', s)]
        def frame_stamp(name):
            return next((t for t, s in frame_events if name in s), None)
        stages = ('scroll.capture_begin', 'scroll.readback_issue', 'scroll.submit',
                  'scroll.receive_begin', 'scroll.worker_begin', 'scroll.present_queued',
                  'scroll.present_begin', 'scroll.thumbnail_paint_begin')
        fields['first_frame_segments_ms'] = {}
        for left, right in zip(stages, stages[1:]):
            a, b = frame_stamp(left), frame_stamp(right)
            fields['first_frame_segments_ms'][left + '_to_' + right] = (b - a) / 1000 if a is not None and b is not None else None
        fields['durations'] = [s for _, s in part if 'elapsed_us=' in s or 'elapsed_ms=' in s]
        summaries.append(fields)
    active_handles = [p['handle'] for p in manifest.get('registered_plugins', []) if p['name'] == 'HyprCapture']
    activated_handles = [p['handle'] for p in manifest.get('activated_registration', []) if p['name'] == 'HyprCapture']
    result = dict(label=manifest['label'], endpoint='UI thumbnail Paint event entry; NOT display presentation', trials=summaries,
                  loaded_instrumented_plugin=bool(active_handles) and active_handles == activated_handles and any(v == json.loads((run.parent / 'state.json').read_text())['new_plugin_sha256'] for v in manifest.get('plugin_hashes', {}).values()),
                  loaded_instrumented_helper=any(p['sha256'] == json.loads((run.parent / 'state.json').read_text())['new_helper_sha256'] for p in manifest['processes']))
    (run / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'trials'}, indent=2))
    for trial in summaries:
        print(json.dumps({k: v for k, v in trial.items() if k != 'durations'}))
    if not summaries:
        print('No scroll.start events in this interval: this is not a completed measurement.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path.home() / '.cache/hyprcapture-hdr-phase1')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--prepare', type=Path, metavar='BUILD_DIR')
    group.add_argument('--record', metavar='LABEL')
    group.add_argument('--summarize', type=Path, metavar='RUN_DIR')
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--original-plugin', type=Path, help='Known original module when stale mappings make discovery ambiguous')
    args = parser.parse_args()
    if args.prepare:
        prepare(args.prepare, args.out.resolve(), args.original_plugin)
    elif args.record:
        if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.record) or not 1 <= args.seconds <= 300:
            parser.error('Use a simple label and 1..300 seconds')
        record(args.out.resolve(), args.record, args.seconds)
    else:
        summarize(args.summarize.resolve())


if __name__ == '__main__':
    main()
