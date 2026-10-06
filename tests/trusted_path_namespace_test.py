"""Check unmapped owners against read-only and writable immutable boundaries."""
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

bwrap, shell = shutil.which('bwrap'), shutil.which('sh')
if not bwrap or not shell:
    sys.exit(77)
shell = Path(shell).resolve()
if shell.stat().st_uid == os.geteuid():
    # A single-user installation cannot exercise an unmapped owner this way.
    sys.exit(77)

base = [bwrap, '--ro-bind', '/', '/', '--unshare-user', '--uid', '1000', '--gid', '1000']
check = subprocess.run([*base, str(shell), '-c', ':'], capture_output=True)
if check.returncode:
    print('User namespaces unavailable:', check.stderr.decode(errors='replace'))
    sys.exit(77)

probe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='hyprcapture-trust-namespace-') as root:
    boundary = root + '/store'
    target = boundary + '/package'
    cases = [('--ro-bind', target + '/' + shell.name, 0), ('--ro-bind', str(shell), 1)]
    owner_writable = (shell.stat().st_mode | shell.parent.stat().st_mode) & 0o200
    if owner_writable and not os.statvfs(shell.parent).f_flag & os.ST_RDONLY:
        cases.append(('--bind', target + '/' + shell.name, 1))
    else:
        print('Writable-path rejection case unavailable: source is already read-only')
    for mount, candidate, expected in cases:
        # The source directory is never modified. The writable bind only tests
        # mount flags; the probe performs stat/access calls and nothing else.
        result = subprocess.run([
            *base, '--tmpfs', root, '--dir', boundary, mount, str(shell.parent), target,
            probe, '--probe', candidate, boundary,
        ], capture_output=True)
        assert result.returncode == expected, (mount, candidate, result.returncode, result.stderr.decode(errors='replace'))

    # Nix also uses writable mounts whose realized inputs have mode 0555.
    # Find a foreign-owned, read-only executable without changing host files.
    for source in shell.parent.iterdir():
        try:
            info = source.stat()
        except OSError:
            continue
        if (not source.is_symlink() and stat.S_ISREG(info.st_mode) and info.st_uid != os.geteuid()
                and not info.st_mode & 0o222 and info.st_mode & 0o111):
            candidate = target + '/tool'
            result = subprocess.run([
                *base, '--tmpfs', root, '--dir', target, '--bind', str(source), candidate,
                probe, '--probe', candidate, boundary,
            ], capture_output=True)
            assert result.returncode == 0, (source, result.stderr.decode(errors='replace'))
            break
    else:
        print('Read-only-permission case unavailable: no suitable foreign-owned executable')
print('Unmapped-owner trust boundary checks passed')
