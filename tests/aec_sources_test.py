"""Exercise verified pre-seeding and rejection before any dependency executes."""
import hashlib
import json
import pathlib
import subprocess
import tarfile
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hyprcapture-aec-sources-') as folder:
    root = pathlib.Path(folder)
    source = root / 'fixture'
    source.mkdir()
    (source / 'CMakeLists.txt').write_text('add_library(fixture INTERFACE)\n')
    archive = root / 'fixture.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        tar.add(source, arcname='fixture')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    manifest = root / 'sources.json'
    manifest.write_text(json.dumps([dict(name='fixture', url=archive.as_uri(), sha256=digest)]))
    cache = root / 'cache'
    def prepare(offline):
        return subprocess.run(['cmake', f'-DCACHE={cache}', f'-DMANIFEST={manifest}',
                               f'-DOFFLINE={offline}', '-P', str(repo / 'scripts/prepare-aec-sources.cmake')],
                              capture_output=True, text=True, timeout=15)
    assert prepare(1).returncode != 0, 'offline missing archive must fail'
    result = prepare(0)
    assert result.returncode == 0, result.stderr
    result = prepare(1)
    assert result.returncode == 0, result.stderr
    project = root / 'project'
    project.mkdir()
    (project / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.19)\n'
        'project(offline NONE)\ninclude(FetchContent)\n'
        'FetchContent_Declare(fixture URL https://invalid.example/unpinned.tar.gz)\n'
        'FetchContent_MakeAvailable(fixture)\n'
        'if(NOT TARGET fixture)\nmessage(FATAL_ERROR "missing pre-seed")\nendif()\n')
    result = subprocess.run(['cmake', '-S', str(project), '-B', str(root / 'build'), '-G', 'Ninja',
                             '-C', str(cache / 'pinned-sources.cmake')], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
    (cache / 'archives' / f'fixture-{digest}').write_bytes(b'retargeted archive')
    assert prepare(1).returncode != 0, 'cached archive must be rechecked even with extracted source present'
    archive.write_bytes(b'retargeted archive')
    assert prepare(0).returncode != 0, 'replacement must fail checksum before extraction'
print('PASS: hash-checked extraction, offline pre-seeding, missing/corrupt/retargeted rejection')
