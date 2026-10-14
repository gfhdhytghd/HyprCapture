#!/usr/bin/env python3
"""Phase 2B helper-only, user-operated A/B and manual editor/scroll recorder.
Preparing/recording never applies configuration or swaps compositor plugins.
"""
import argparse
import importlib.util
import json
import re
import shlex
import subprocess
from pathlib import Path


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


live = module('hdr_live', 'measure-live.py')


def summarize(run):
    manifest = json.loads((run / 'manifest.json').read_text())
    state = json.loads((run.parent / 'state.json').read_text())
    ui = [(t, s) for t, s in live.read_events(run / 'ui.log') if manifest['start_us'] <= t <= manifest['end_us']]
    plugin = [(t, s) for t, s in live.read_events(run / 'plugin.log') if manifest['start_us'] <= t <= manifest['end_us']]
    starts = [(t, s) for t, s in ui if ' overlay.begin_edit_requested ' in s]
    trials = []
    for index, (started, line) in enumerate(starts):
        pid = re.search(r'\bpid=(\d+)', line).group(1)
        end = starts[index+1][0] if index+1 < len(starts) else manifest['end_us'] + 1
        part = [(t, s) for t, s in ui if started <= t < end and f'pid={pid} ' in s]
        def first(name):
            return next((t for t, s in part if name in s), None)
        def delta(name):
            stamp = first(name)
            return (stamp-started)/1000 if stamp is not None else None
        ready = first(' overlay.editor_ready ')
        entered = first(' overlay.begin_edit ')
        hydrated_until = ready or entered or end
        input_stamp = next((t for t, s in plugin if started <= t < end and 'event=scroll.input_begin' in s), None)
        thumbnail_stamp = next((t for t, s in part if input_stamp is not None and t >= input_stamp and 'scroll.thumbnail_paint_begin seq=' in s), None)
        trials.append(dict(pid=int(pid), request_us=started,
            input_to_thumbnail_paint_begin_ms=(thumbnail_stamp-input_stamp)/1000 if thumbnail_stamp is not None else None,
            request_to_editor_ready_ms=delta(' overlay.editor_ready '),
            request_to_editor_first_paint_begin_ms=delta(' overlay.editor_first_paint_begin '),
            request_to_begin_edit_return_ms=delta(' overlay.begin_edit '),
            request_to_hdr_base_ready_ms=delta(' overlay.hdr_base_ready '),
            first_paint=next((s for _, s in part if ' overlay.editor_first_paint_begin ' in s), None),
            ui_durations=[s for _, s in part if 'elapsed_us=' in s and any(k in s for k in ('overlay.render', 'overlay.hdr_base', 'overlay.hdr_edit', 'overlay.begin_edit'))],
            plugin_capture_durations=[s for t, s in plugin if started <= t <= hydrated_until and any(k in s for k in ('window.readback','realbg.'))]))
    selected_helper = manifest.get('helper_option', {}).get('str')
    known_helper = selected_helper in {str(run.parent/(mode+'.sh')) for mode in ('before','cpu','after')}
    expected = state['before_sha256'] if selected_helper == str(run.parent/'before.sh') else state['after_sha256']
    observed = manifest.get('processes', [])
    helper_ok = known_helper and bool(observed) and all(p['sha256'] == expected for p in observed)
    handles = [p['handle'] for p in manifest.get('registered_plugins', []) if p['name']=='HyprCapture']
    prepared = [p['handle'] for p in state['plugin_list'] if p['name']=='HyprCapture']
    result = dict(label=manifest['label'], endpoint='Editor ready and first canvas Paint entry; NOT scanout/presentation',
                  helper_hash_verified=helper_ok, plugin_registration_unchanged=bool(handles) and handles==prepared,
                  complete_editor_trials=bool(trials) and all(t['request_to_begin_edit_return_ms'] is not None for t in trials) and helper_ok and bool(handles) and handles==prepared,
                  trials=trials, live_after_acceptance='manual trials required; absent entries are not accepted measurements')
    (run/'editor-summary.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='trials'}, indent=2))
    for trial in trials:
        print(json.dumps({k:v for k,v in trial.items() if not k.endswith('durations')}))
    if not trials: print('No editor entry in this interval; no accepted measurement.')


def prepare(build, baseline, out):
    if 'hyprcapture-hdr-phase1' in str(out): raise SystemExit('Phase-1 directory is reserved')
    old = {key: json.loads(live.ctl('-j','getoption','plugin:hyprcapture:'+key)) for key in ('helper','timing','timing_file')}
    subprocess.run(['python3', str(Path(__file__).with_name('phase2a-live.py')), '--prepare', str(build),
                    '--baseline', str(baseline), '--out', str(out)], check=True)
    state = json.loads((out/'state.json').read_text()); state['old_options'] = old
    (out/'state.json').write_text(json.dumps(state, indent=2)+'\n')
    logs = Path(state['logs'])
    for mode in ('before','cpu','after'):
        wrapper = out/(mode+'.sh')
        text = wrapper.read_text().replace(str(logs/(mode+'.log')), str(logs/'ui.log')).replace(str(logs/(mode+'-stderr.log')),str(logs/'helper-stderr.log'))
        wrapper.write_text(text)
        expression = 'hl.config({plugin={hyprcapture={helper='+json.dumps(str(wrapper))+',timing=true,timing_file='+json.dumps(str(logs/'plugin.log'))+'}}})'
        activate = out/('activate-'+mode+'.sh')
        lines = activate.read_text().splitlines()
        lines = [line if not line.startswith('hyprctl eval ') else 'hyprctl eval '+shlex.quote(expression) for line in lines]
        handle = next(p['handle'] for p in state['plugin_list'] if p['name']=='HyprCapture')
        check = 'import json,sys; p=[p for p in json.load(sys.stdin) if p["name"]=="HyprCapture"]; assert len(p)==1 and p[0]["handle"]=='+repr(handle)+', "Registration changed; prepare a fresh directory"'
        lines.insert(4,'hyprctl -j plugins list | python3 -c '+shlex.quote(check))
        activate.write_text('\n'.join(lines)+'\n')
    expression = 'hl.config({plugin={hyprcapture={helper='+json.dumps(old['helper']['str'])+',timing='+str(old['timing']['bool']).lower()+',timing_file='+json.dumps(old['timing_file']['str'])+'}}})'
    restore = out/'restore.sh'
    restore.write_text('\n'.join(line if not line.startswith('hyprctl eval ') else 'hyprctl eval '+shlex.quote(expression) for line in restore.read_text().splitlines())+'\n')
    print('Phase 2B: activate-before/after.sh select copied helpers and isolated timing logs; no plugin swap.')
    print('Alternate 5 cold and 5 warm trials per helper: open_editor -> click same kitty (2386x1656) -> immediate/1s wait -> one wheel notch -> cancel.')
    print('Keep HDR eDP-1 2880x1800 scale2 and output format unchanged; stop compilation during measurement. Close overlays and run restore.sh afterwards.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--prepare', type=Path)
    group.add_argument('--record')
    group.add_argument('--summarize', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--seconds', type=int, default=30)
    args = parser.parse_args(); out=args.out.resolve()
    if args.prepare:
        if not args.baseline: parser.error('--prepare needs --baseline')
        prepare(args.prepare.resolve(), args.baseline.resolve(), out)
    elif args.record:
        if not re.fullmatch('[a-zA-Z0-9_-]+',args.record) or not 1<=args.seconds<=300: parser.error('Use simple label and 1..300 seconds')
        live.summarize = summarize
        live.record(out, args.record, args.seconds)
    else: summarize(args.summarize.resolve())


if __name__=='__main__': main()
