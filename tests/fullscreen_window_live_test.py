#!/usr/bin/env python3
"""Opt-in 0.56 regression check against an explicitly supplied nested instance.

The session JSON contains runtime, instance and socket. No default/host IPC is
used. Start the solid-color fixture in that instance before running this test.
See docs/validation/fullscreen-window-decorations.md for setup and evidence.
"""
import argparse
import array
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import uuid

import numpy as np
from PIL import Image

MODES = [("normal", 0, 0), ("maximized", 1, 1), ("fullscreen", 2, 2),
         ("client-only", 0, 2), ("internal-only", 2, 0)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", choices=["before", "after"], required=True)
    parser.add_argument("--expect-content", action="store_true")
    args = parser.parse_args()
    state = json.loads(args.session.read_text())
    runtime = Path(state["runtime"]).resolve()
    assert runtime != Path(f"/run/user/{os.getuid()}").resolve(), "Use a private nested runtime"
    assert (runtime / "hypr" / state["instance"] / ".socket.sock").is_socket()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WAYLAND_DISPLAY=state["socket"],
               HYPRLAND_INSTANCE_SIGNATURE=state["instance"])
    private = Path(f"/dev/shm/hyprcapture-{os.getuid()}")
    private.mkdir(mode=0o700, exist_ok=True)
    token = "fsdeco-" + uuid.uuid4().hex
    screenshots, streams, recordings = [], [], []

    def ctl(*commands):
        result = subprocess.run(["/usr/bin/hyprctl", "-i", state["instance"], *commands],
                                env=env, text=True, capture_output=True, timeout=20)
        if result.returncode or result.stdout.startswith("error:"):
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    def request(action, payload):
        path = private / f"{token}-{uuid.uuid4().hex}.json"
        path.write_text(json.dumps(payload))
        path.chmod(0o600)
        try:
            ctl("eval", f"hl.plugin.hyprcapture.{action}({json.dumps(str(path))})")
            return json.loads(path.read_text()) if path.exists() else None
        finally:
            path.unlink(missing_ok=True)

    ctl("eval", 'hl.config({plugin={hyprcapture={notification_backend="hyprland"}}})')
    for name, internal, client in MODES:
        ctl("dispatch", f'hl.dsp.window.fullscreen_state({{internal={internal},client={client},action="set"}})')
        time.sleep(0.25)  # allow client configure/paint after the state transition
        target = next(w for w in json.loads(ctl("-j", "clients")) if w["class"] == "fsdeco-fixture")
        assert (target["fullscreen"], target["fullscreenClient"]) == (internal, client)
        base = {"id": token, "mode": "window", "windowAddress": target["address"],
                "targetGeometry": {"x": 0, "y": 0, "width": 800, "height": 600}}
        capture = request("window_capture", dict(base, defaults={"windowBorder": "keep", "windowShadow": "keep", "windowBackground": "white"}))
        info = capture["windows"][0]
        pixels = np.fromfile(info["artifactPath"], dtype=np.uint8).reshape(info["artifactHeight"], info["artifactWidth"], 4)
        Image.fromarray(pixels).save(output / f"{args.label}-{name}.png")
        row = {"mode": name, "internal": internal, "client": client,
               "contentSize": target["size"], "artifactSize": [info["artifactWidth"], info["artifactHeight"]],
               "visibleGeometry": info["visibleGeometry"], "fullGeometry": info["fullGeometry"],
               "alphaMin": int(pixels[:, :, 3].min()), "transparentPixels": int((pixels[:, :, 3] < 255).sum())}
        if args.expect_content and internal == 2:
            assert row["artifactSize"] == target["size"] and row["alphaMin"] == 255, row
            assert row["fullGeometry"] == row["visibleGeometry"], row
        screenshots.append(row)
        for window in capture["windows"]:
            for field in ("artifactPath", "realBackgroundPath"):
                if window.get(field):
                    Path(window[field]).unlink(missing_ok=True)
            Path(window["artifactPath"]).parent.rmdir()

        for transport in ("window", "window-gpu"):
            stream_id = f"{token}-{name}-{transport}"
            endpoint = private / f"{stream_id}.sock"
            server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            server.bind(str(endpoint))
            endpoint.chmod(0o600)
            server.listen(1)
            server.settimeout(8)
            handles = []
            try:
                request("window_stream_start", {"id": stream_id, "mode": transport,
                        "windowAddress": target["address"], "socketPath": str(endpoint), "fps": 10,
                        "defaults": {"mode": "window", "windowBackground": "transparent",
                                     "windowBorder": "keep", "windowShadow": "keep"}})
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(8)
                    data, ancillary, _, _ = connection.recvmsg(512, socket.CMSG_SPACE(8 * 4))
                    for level, kind, body in ancillary:
                        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                            fds = array.array("i")
                            fds.frombytes(body[:len(body) // fds.itemsize * fds.itemsize])
                            handles.extend(fds)
                    assert data[:4] == (b"HCSF" if transport == "window" else b"HCGF")
                    width, height = struct.unpack_from(">II", data, 64)
                    row = {"mode": name, "transport": transport, "size": [width, height],
                           "logicalSize": list(struct.unpack_from(">dd", data, 48))}
                    if transport == "window":
                        rgba = np.frombuffer(os.pread(handles[0], width * height * 4, 0), dtype=np.uint8).reshape(height, width, 4)
                        row["alphaMin"] = int(rgba[:, :, 3].min())
                    else:
                        row["shadowEnabled"] = bool(struct.unpack_from(">I", data, 112)[0] & 4)
                    if args.expect_content and internal == 2:
                        assert row["size"] == target["size"], row
                        assert row.get("alphaMin", 255) == 255 and not row.get("shadowEnabled", False), row
                    streams.append(row)
            finally:
                for fd in handles:
                    os.close(fd)
                try:
                    request("window_stream_stop", {"streamId": stream_id})
                except RuntimeError as error:
                    if "not active" not in str(error):
                        raise
                server.close()
                endpoint.unlink(missing_ok=True)

        video = output / f"{args.label}-record-{name}.mkv"
        assert not video.exists(), f"Keep each run's output immutable: {video}"
        request("record_start", dict(base, defaults={"windowBorder": "keep", "windowShadow": "keep",
                "windowBackground": "white", "recordFormat": "mkv", "recordCodec": "ffv1",
                "recordFps": 10, "recordWindowFpsLimit": 10, "recordAudio": "off", "recordWindowBackend": "compositor",
                "recordSaveDir": str(output), "recordFilenameTemplate": video.name, "showThumbnail": False, "clipboard": False}))
        try:
            time.sleep(0.4)
        finally:
            ctl("eval", "hl.plugin.hyprcapture.record_stop()")
        for _ in range(80):
            probe = subprocess.run(["/usr/bin/ffprobe", "-v", "error", "-show_entries", "stream=width,height", "-of", "json", str(video)], text=True, capture_output=True)
            if probe.returncode == 0 and json.loads(probe.stdout).get("streams"):
                break
            time.sleep(0.05)
        else:
            raise RuntimeError(f"No finalized recording at {video}")
        size = json.loads(probe.stdout)["streams"][0]
        row = {"mode": name, "size": [size["width"], size["height"]]}
        if args.expect_content and internal == 2:
            assert row["size"] == target["size"], row
            decoded = subprocess.run(["/usr/bin/ffmpeg", "-v", "error", "-i", str(video), "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgba", "-"], capture_output=True, check=True).stdout
            rgba = np.frombuffer(decoded, dtype=np.uint8).reshape(size["height"], size["width"], 4)
            assert np.all(rgba == [34, 149, 98, 255]), "Recording has a border/background outside fixture content"
        recordings.append(row)
        print(json.dumps({"mode": name, "screenshot": screenshots[-1], "streams": streams[-2:], "recording": row}), flush=True)
    (output / f"{args.label}.json").write_text(json.dumps({"screenshots": screenshots, "streams": streams, "recordings": recordings}, indent=2) + "\n")


if __name__ == "__main__":
    main()
