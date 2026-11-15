"""Recorded pixel-edge accuracy; token matching follows compare_service.py.

No OCR engine, network, fonts or private reference directory is needed.
"""
import json
import math
import statistics as st
import subprocess
import sys
import tempfile
import unicodedata
from pathlib import Path

root = Path(__file__).resolve().parent / 'data/ocr-refine'
truth = json.loads((root / 'ground-truth.json').read_text())
vendor = json.loads((root / 'vendor.json').read_text())
prototype = json.loads((root / 'prototype.json').read_text())


def canon(text):
    return unicodedata.normalize('NFKC', text).replace(' ', '')


def tokens(row):
    if row.get('words'):
        return [(w['text'], w['box']) for w in row['words']]
    return list(zip(row.get('text_word') or [], row.get('text_word_boxes') or []))


def score(fixtures, split):
    saved = {x['file']: x['response']['results'] for x in fixtures}
    errors = {'cjk': [], 'latin': []}
    unmatched = total = 0
    for image in truth['images']:
        if split != 'all' and image['split'] != split:
            continue
        for row in saved[image['file']]:
            middle = st.mean(p[1] for p in row['box'])
            line = min(image['lines'], key=lambda l: abs(st.mean(
                (g['ink_box'][1] + g['ink_box'][3]) / 2 for g in l['glyphs'] if g['ink_box']) - middle))
            glyphs = [g for g in line['glyphs'] if g['ink_box']]
            text = ''.join(canon(g['text']) for g in glyphs)
            owner = [i for i, g in enumerate(glyphs) for _ in canon(g['text'])]
            cursor = 0
            for token, box in tokens(row):
                token = canon(token)
                if not token or box is None:
                    continue
                total += 1
                at = text.find(token, cursor)
                if at < 0:
                    unmatched += 1
                    continue
                cursor = at + len(token)
                ink = [glyphs[owner[i]]['ink_box'] for i in range(at, cursor)]
                left, right = min(b[0] for b in ink), max(b[2] for b in ink)
                xs = [p[0] for p in box] if isinstance(box[0], list) else [box[0], box[2]]
                errors['latin' if token.isascii() else 'cjk'].extend((abs(min(xs)-left), abs(max(xs)-right)))
    out = {'tokens': total, 'unmatched': unmatched}
    for kind, values in errors.items():
        if values:
            edges = sorted(values)
            out[kind] = (st.mean(edges), edges[int(.95*len(edges))-1], edges[-1])
    return out


def refine(fixtures):
    with tempfile.TemporaryDirectory(prefix='ocr-refine-') as directory:
        path = Path(directory) / 'input.json'
        path.write_text(json.dumps(fixtures))
        return json.loads(subprocess.check_output([sys.argv[1], str(path), str(root)], text=True))


def main():
    summary = {}
    failures = []
    print('source | split | kind | before mean/p95/max px | after mean/p95/max px | tokens/unmatched', flush=True)
    for source, fixtures in [('vendor', vendor), ('prototype', prototype), ('estimated', vendor)]:
        if source == 'estimated':
            fixtures = json.loads(json.dumps(fixtures))
            for fixture in fixtures:
                for row in fixture['response']['results']:
                    for key in ('words', 'text_word', 'text_word_boxes'):
                        row.pop(key, None)
        after = refine(fixtures)
        # Estimated before uses the same 1:2 initializer without pixel evidence,
        # supplied by the driver so it is scored with exactly the same tokens.
        before = fixtures
        if source == 'estimated':
            before = json.loads(subprocess.check_output([sys.argv[1], '--estimates', str(root / 'vendor.json'), str(root)], text=True))
        original = {x['file']: x['response']['results'] for x in before}
        for fixture in after:
            if fixture['file'] not in ('dense.png', 'mixed.png', 'skew.png'):
                continue
            changed = 0
            for old, row in zip(original[fixture['file']], fixture['response']['results']):
                old_boxes = [box for text, box in tokens(old) if canon(text) and box]
                new_boxes = [box for text, box in tokens(row) if canon(text) and box]
                changed += old_boxes != new_boxes
                for box in new_boxes:
                    assert len(box) == 4 and all(math.isfinite(c) for p in box for c in p)
                    area = sum(box[i][0]*box[(i+1)%4][1] - box[i][1]*box[(i+1)%4][0] for i in range(4))
                    assert area > 0, (source, fixture['file'], box)
            assert changed > 0, (source, fixture['file'], 'no refined geometry')
            print(f'{source} {fixture["file"]}: {changed} lines changed; finite clockwise quads', flush=True)
        summary[source] = {}
        for split in ('all', 'fit', 'holdout', 'validation'):
            a, b = score(before, split), score(after, split)
            summary[source][split] = {'before': a, 'after': b}
            for kind in ('cjk', 'latin'):
                if kind not in b:
                    failures.append(f'{source}/{split}/{kind}: no scored tokens')
                    continue
                print(f'{source} | {split} | {kind} | ' + '/'.join(f'{x:.3f}' for x in a.get(kind, ())) +
                      ' | ' + '/'.join(f'{x:.3f}' for x in b[kind]) + f' | {b["tokens"]}/{b["unmatched"]}', flush=True)
                if source == 'vendor' and any(v > limit + 1e-9 for v, limit in zip(b[kind], (1., 2.5, 6.))):
                    failures.append(f'{source}/{split}/{kind}: accuracy gate')
                if source == 'prototype' and b[kind][0] > a[kind][0] + .2 + 1e-9:
                    failures.append(f'{source}/{split}/{kind}: regression gate')
                if source == 'estimated' and b[kind][0] > 2. + 1e-9:
                    failures.append(f'{source}/{split}/{kind}: fallback accuracy gate')
            if b['unmatched'] or b['tokens'] != a['tokens']:
                failures.append(f'{source}/{split}: token matching/count gate')
        for item in after:
            if 'timing' in item:
                print(f'{source} dense timing: {item["timing"]}', flush=True)
                summary[source]['timing'] = item['timing']
                # Typical median is ~1.5 ms; the gate only catches algorithmic
                # regressions and must not flake on a loaded machine.
                if item['timing']['medianMs'] > 20:
                    failures.append(f'{source}: dense median exceeds 20 ms')
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(json.dumps(summary, indent=2) + '\n')
    if failures:
        raise AssertionError('; '.join(failures))


if __name__ == '__main__':
    main()
