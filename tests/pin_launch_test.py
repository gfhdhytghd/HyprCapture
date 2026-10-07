#!/usr/bin/env python3
"""Exercise the real pin helper's load/ready/accepted process boundary offscreen."""

import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import zlib


def png_chunk(kind, payload):
    return struct.pack("!I", len(payload)) + kind + payload + struct.pack("!I", zlib.crc32(kind + payload))


PNG = (
    b"\x89PNG\r\n\x1a\n"
    + png_chunk(b"IHDR", struct.pack("!IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
    + png_chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff"))
    + png_chunk(b"IEND", b"")
)


def launch(helper, source, ready_socket):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    return subprocess.Popen(
        [helper, "--pin-image", str(source), "--pin-consume-source", "--pin-ready-socket", str(ready_socket),
         "--pin-geometry", "212,330,788,430"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


def main():
    helper = str(Path(sys.argv[1]).resolve())
    runtime = Path("/dev/shm") / f"hyprcapture-{os.geteuid()}"
    if not runtime.exists():
        runtime.mkdir(mode=0o700)
    assert runtime.is_dir() and runtime.stat().st_uid == os.geteuid()
    with tempfile.TemporaryDirectory(prefix="pin-launch-test-", dir=runtime) as temporary:
        root = Path(temporary)
        for name, payload, accept in (("valid", PNG, True), ("invalid", b"invalid image", False), ("unaccepted", PNG, False)):
            source = root / f"{name}.png"
            source.write_bytes(payload)
            ready_path = root / f"{name}.socket"
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(str(ready_path))
                os.chmod(ready_path, 0o600)
                server.listen(1)
                server.settimeout(5)
                process = launch(helper, source, ready_path)
                try:
                    connection, _ = server.accept()
                    with connection:
                        connection.settimeout(5)
                        reply = b""
                        while b"\n" not in reply:
                            data = connection.recv(4096)
                            assert data, (name, reply)
                            reply += data
                        if payload == PNG:
                            assert reply == b"ready\n", reply
                            assert not source.exists(), "successful private source must be consumed"
                        else:
                            assert reply.startswith(b"error:"), reply
                            assert source.exists(), "invalid source must be retained"
                        if accept:
                            connection.sendall(b"accepted\n")
                            try:
                                process.wait(timeout=0.1)
                            except subprocess.TimeoutExpired:
                                pass
                            else:
                                raise AssertionError("accepted pin exited prematurely")
                            process.terminate()
                            process.wait(timeout=5)
                        else:
                            assert process.wait(timeout=5) == 1, "failed/unaccepted launch must exit"
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
        source = root / "missing-server.png"
        source.write_bytes(PNG)
        process = launch(helper, source, root / "missing.socket")
        assert process.wait(timeout=5) == 1
        assert source.exists(), "missing parent must not consume source"
    print("pin launch handshake tests passed")


if __name__ == "__main__":
    main()
