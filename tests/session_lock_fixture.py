"""Private compositor lock-channel fixture for real offscreen helper tests."""
import os
from pathlib import Path
import socket
import tempfile
import threading


class SessionLockFixture:
    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='hc-lock-')
        directory = Path(self.temporary.name) / 'hypr' / 'test'
        directory.mkdir(parents=True, mode=0o700)
        self.env = dict(os.environ, XDG_RUNTIME_DIR=self.temporary.name, HYPRLAND_INSTANCE_SIGNATURE='test', HYPRCAPTURE_LOCK_GENERATION='fixture')
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.server.bind(str(directory / 'hyprcapture-lock.sock'))
        self.server.listen()
        self.server.settimeout(.1)
        self.clients = []
        self.locked = False
        self.stopping = threading.Event()
        self.mutex = threading.Lock()
        self.worker = threading.Thread(target=self.run)
        self.worker.start()
        return self

    def run(self):
        while not self.stopping.is_set():
            try:
                client, _ = self.server.accept()
            except socket.timeout:
                continue
            with self.mutex:
                client.send(b'locked' if self.locked else b'unlocked:fixture')
                self.clients.append(client)

    def lock(self):
        with self.mutex:
            self.locked = True
            for client in self.clients:
                try:
                    client.send(b'locked')
                except BrokenPipeError:
                    pass
                client.close()
            self.clients.clear()

    def __exit__(self, *_):
        self.stopping.set()
        self.worker.join(timeout=2)
        self.server.close()
        for client in self.clients:
            client.close()
        self.temporary.cleanup()
