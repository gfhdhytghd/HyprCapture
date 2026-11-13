"""Offline curl integration/detection. Run with python3 -I in a private netns."""
import base64
import email.parser
import email.policy
import hashlib
import http.server
import json
import os
from pathlib import Path
import selectors
import stat
import subprocess
import sys
import tempfile
import threading
import time

DRIVER = sys.argv[1]
KEY = 'secret-key-"\\-for-proc-check'
PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=') + b'\0' * 25000
SIMPLE = {'results': [{'text': 'hello', 'confidence': .99, 'box': [[.5, 0], [20, 0], [20, 10], [.5, 10]]}]}
OFFICIAL = {'errorCode': 0, 'result': {'ocrResults': [{'prunedResult': {'rec_texts': ['hello'], 'rec_polys': [[[0, 0], [20, 0], [20, 10], [0, 10]]], 'rec_scores': [.99]}}]}}
checks = 0

def check(value, message):
    global checks
    checks += 1
    assert value, message

class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, kind):
        super().__init__(('127.0.0.1', 0), Handler)
        self.kind = kind
        self.requests = []
        self.errors = []
        self.expected_key = KEY
        self.driver_pid = None
        self.curl_pids = set()
        self.temp_dirs = set()
        self.seen_body_private = False
        self.lock = threading.Lock()

    def inspect(self):
        # Inspect only descendants of our test driver.
        parents = {}
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            try:
                info = (proc / 'stat').read_text().rsplit(') ', 1)[1].split()
                parents[int(proc.name)] = int(info[1])
            except (OSError, IndexError, ValueError):
                pass
        for pid in parents:
            ancestor = pid
            for _ in range(10):
                ancestor = parents.get(ancestor)
                if ancestor == self.driver_pid:
                    try:
                        args = Path(f'/proc/{pid}/cmdline').read_bytes()
                        if b'curl\0' not in args and b'/curl\0' not in args:
                            break
                        check(KEY.encode() not in args, 'API key absent from curl argv')
                        check(b'http://' not in args, 'API URL absent from curl argv')
                        check(b'base64' not in args and b'fileType' not in args, 'request body absent from curl argv')
                        self.curl_pids.add(pid)
                        config = os.readlink(f'/proc/{pid}/fd/0')
                        check(config.endswith(' (deleted)'), 'curl config stdin is unlinked')
                        parent = Path(config.removesuffix(' (deleted)')).parent
                        self.temp_dirs.add(parent)
                        check(stat.S_IMODE(parent.stat().st_mode) == 0o700, 'private OCR directory is 0700')
                        body = parent / 'request.json'
                        if body.exists():
                            check(stat.S_IMODE(body.stat().st_mode) == 0o600, 'JSON body is 0600')
                            self.seen_body_private = True
                    except FileNotFoundError:
                        pass
                    break
                if ancestor is None:
                    break

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        server = self.server
        try:
            raw = self.rfile.read(int(self.headers['Content-Length']))
            official = self.headers['Content-Type'].startswith('application/json')
            fmt = 'paddlex' if official else 'simple'
            with server.lock:
                server.requests.append((self.path, fmt, time.monotonic()))
                server.inspect()
                expected = ('token ' if official else 'Bearer ') + server.expected_key
                check(self.headers.get('Authorization') == (expected if server.expected_key else None), 'authorization scheme/key or absence')
                if official:
                    body = json.loads(raw)
                    check(base64.b64decode(body['file'], validate=True) == PNG, 'official body has valid complete base64 PNG')
                    check(body == {'file': body['file'], 'fileType': 1, 'useDocOrientationClassify': False, 'useDocUnwarping': False, 'useTextlineOrientation': False, 'visualize': False}, 'official JSON flags')
                else:
                    message = email.parser.BytesParser(policy=email.policy.default).parsebytes(b'Content-Type: ' + self.headers['Content-Type'].encode() + b'\r\n\r\n' + raw)
                    parts = list(message.iter_parts())
                    check(len(parts) == 1, 'exactly one multipart field')
                    part = parts[0]
                    check(part.get_param('name', header='content-disposition') == 'file', 'multipart field file')
                    check(part.get_filename() == 'image.png' and part.get_content_type() == 'image/png', 'multipart filename and type')
                    check(part.get_payload(decode=True) == PNG, 'multipart PNG bytes')
            code, response = 200, OFFICIAL if official else SIMPLE
            if self.path in ('/slow', '/cancel'):
                time.sleep(2)
            elif self.path == '/budget':
                time.sleep(.7)
                code = 200 if official else 422
            elif self.path == '/both':
                code = 422
            elif self.path == '/drop':
                self.close_connection = True
                return
            elif self.path == '/400':
                code = 200 if official else 400
            elif self.path == '/401':
                code = 401
            elif self.path == '/403':
                code = 403
            elif self.path == '/500':
                code = 500
            elif self.path == '/error-400':
                code, response = 400, {'errorCode': 3, 'errorMsg': 'model unavailable'}
            elif self.path == '/error-code':
                response = {'errorCode': 3, 'errorMsg': 'model unavailable'}
            elif self.path == '/reflect':
                response = {'errorCode': 3, 'errorMsg': server.expected_key}
            elif self.path == '/reflect-url':
                response = {'errorCode': 3, 'errorMsg': 'http://' + self.headers['Host'] + self.path}
            elif self.path == '/bad-json':
                response = b'{'
            elif self.path == '/unknown':
                response = OFFICIAL if official else {}
            elif self.path == '/huge':
                response = b'x' * (8 * 1024 * 1024 + 1)
            elif self.path == '/redirect':
                code = 302
            elif self.path == '/change':
                code = 200 if server.kind != 'none' and (official == (server.kind == 'paddlex')) else 422
            elif server.kind == 'simple' and official:
                code = 422
            elif server.kind == 'paddlex' and not official:
                code = 415
            data = response if isinstance(response, bytes) else json.dumps(response).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            if code == 302:
                self.send_header('Location', 'file:///etc/passwd')
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass
        except Exception as exc:
            server.errors.append(repr(exc))
            self.close_connection = True

class Client:
    def __init__(self, root):
        self.cache = root / 'cache'
        self.image = root / 'image \'";,$.png'
        self.image.write_bytes(PNG)
        self.process = subprocess.Popen([DRIVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                        env={**os.environ, 'XDG_CACHE_HOME': str(self.cache), 'XDG_RUNTIME_DIR': str(root)})
        self.select = selectors.DefaultSelector()
        self.select.register(self.process.stdout, selectors.EVENT_READ)

    def call(self, server=None, path='/ocr', key=KEY, **kwargs):
        if server:
            before = len(server.requests)
            server.driver_pid = self.process.pid
            server.expected_key = key
            request = {'url': f'http://127.0.0.1:{server.server_port}{path}', 'image': str(self.image), 'key': key, **kwargs}
        else:
            request = kwargs
        self.process.stdin.write(json.dumps(request) + '\n')
        self.process.stdin.flush()
        assert self.select.select(10), 'driver did not return in 10 seconds'
        line = self.process.stdout.readline()
        assert line, 'driver exited: ' + self.process.stderr.read()
        result = json.loads(line)
        if server:
            formats = [fmt for _, fmt, _ in server.requests[before:]]
            check(len(formats) <= 2 and len(set(formats)) == len(formats), 'each call tries at most two distinct formats')
            check(not server.errors, str(server.errors))
            check(KEY not in result['error'] and request['url'] not in result['error'], 'error redaction')
        return result

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=5)
        check(self.process.returncode == 0, self.process.stderr.read())
        self.select.close()
        self.process.stdout.close()
        self.process.stderr.close()

with tempfile.TemporaryDirectory(dir=Path.cwd(), prefix='ocr-http-') as folder:
    root = Path(folder)
    a, b = Server('simple'), Server('paddlex')
    for server in (a, b):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    client = Client(root)
    try:
        for server, expected in ((a, ['simple']), (b, ['simple', 'paddlex'])):
            result = client.call(server)
            check(result['text'] == 'hello', f'first-call recognition: {result}')
            check([fmt for path, fmt, _ in server.requests if path == '/ocr'] == expected, 'first-call detection, exactly one retry')
            before = len(server.requests)
            check(client.call(server)['text'] == 'hello', 'memory cache recognition')
            check(len(server.requests) == before + 1 and server.requests[-1][1] == expected[-1], 'memory cache avoids retry')
        cache_file = client.cache / 'hyprcapture/ocr-api-format.json'
        entries = json.loads(cache_file.read_text())
        check(stat.S_IMODE(cache_file.stat().st_mode) == 0o600, 'cache file 0600')
        for server, fmt in ((a, 'simple'), (b, 'paddlex')):
            url = f'http://127.0.0.1:{server.server_port}/ocr'
            check(entries[hashlib.sha256(url.encode()).hexdigest()] == fmt, 'cache uses SHA-256 URL keys')
        check('http' not in cache_file.read_text() and KEY not in cache_file.read_text(), 'cache stores no URL or API key')
        client.close()
        client = Client(root)
        before = len(b.requests)
        check(client.call(b)['text'] == 'hello' and len(b.requests) == before + 1, 'disk cache reused across helper processes')
        check(client.call(a, key='')['text'] == 'hello', 'empty key omits Authorization')
        check(client.call(b, '/model/layout-parsing')['text'] == 'hello' and b.requests[-1][1] == 'paddlex', 'layout-parsing guesses official first')
        for url in ('https://one.aistudio-app.com/ocr', 'https://aistudio.baidu.com/ocr', 'https://X.AISTUDIO.BAIDU.COM:443/ocr'):
            check(client.call(guess=url)['paddlex'], 'AI Studio host guess')
        for url in ('https://notaistudio-app.com/ocr', 'https://aistudio.baidu.com.evil/ocr', 'http://127.0.0.1/ocr?a=/layout-parsing', 'http://127.0.0.1?a=/layout-parsing'):
            check(not client.call(guess=url)['paddlex'], 'non-AI Studio host/query is simple')
        check(client.call(a, '/unknown')['text'] == 'hello', 'unknown valid JSON triggers fallback')
        check([fmt for path, fmt, _ in a.requests if path == '/unknown'] == ['simple', 'paddlex'], 'unknown JSON retries exactly once')
        check(client.call(a, '/400')['text'] == 'hello', 'HTTP 400 triggers format fallback')
        check([fmt for path, fmt, _ in a.requests if path == '/400'] == ['simple', 'paddlex'], '400 retries once')
        check(client.call(a, '/change')['text'] == 'hello', 'prime simple cache')
        a.kind = 'paddlex'
        check(client.call(a, '/change')['text'] == 'hello', 'stale cache re-detects')
        check([fmt for path, fmt, _ in a.requests if path == '/change'] == ['simple', 'simple', 'paddlex'], 'stale format then the other format, exactly two attempts')
        before = len(a.requests)
        check(client.call(a, '/change')['text'] == 'hello' and len(a.requests) == before + 1, 're-detected format cached')
        change_hash = hashlib.sha256(f'http://127.0.0.1:{a.server_port}/change'.encode()).hexdigest()
        check(json.loads(cache_file.read_text())[change_hash] == 'paddlex', 'stale persistent entry replaced')
        a.kind = 'simple'
        before = len(a.requests)
        check(client.call(a, '/change')['text'] == 'hello', 'stale official cache switches directly to simple')
        check([fmt for _, fmt, _ in a.requests[before:]] == ['paddlex', 'simple'], 'reverse stale cache uses two distinct formats')
        check(json.loads(cache_file.read_text())[change_hash] == 'simple', 'reverse stale persistent entry replaced')
        a.kind = 'none'
        before = len(a.requests)
        result = client.call(a, '/change')
        check('HTTP 422' in result['error'], 'both formats failing after stale cache returns error')
        check([fmt for _, fmt, _ in a.requests[before:]] == ['simple', 'paddlex'], 'stale cache failure stops after two distinct attempts')
        check(change_hash not in json.loads(cache_file.read_text()), 'failed stale cache entry removed from disk')
        a.kind = 'paddlex'
        before = len(a.requests)
        check(client.call(a, '/change')['text'] == 'hello', 'call after failed stale detection starts fresh')
        check([fmt for _, fmt, _ in a.requests[before:]] == ['simple', 'paddlex'], 'failed stale cache entry removed from memory')
        a.kind = 'simple'
        for path, fragment in (('/drop', 'unreachable'), ('/401', 'HTTP 401'), ('/403', 'HTTP 403'), ('/500', 'HTTP 500'), ('/bad-json', 'not valid JSON'),
                               ('/error-code', 'model unavailable'), ('/error-400', 'model unavailable'), ('/huge', '8 MiB'), ('/redirect', 'HTTP 302'), ('/reflect', '[redacted]')):
            before = len(a.requests)
            result = client.call(a, path)
            check(fragment in result['error'], f'{path} error: {result}')
            check(len(a.requests) == before + 1, f'{path} is not retried')
        long_key = 'long-private-key-' + 'X' * 1500
        result = client.call(a, '/reflect', key=long_key)
        check('[redacted]' in result['error'] and 'long-private-key' not in result['error'], 'long key redacted before truncation')
        result = client.call(a, '/reflect-url')
        check('[redacted]' in result['error'], 'reflected URL redacted')
        result = client.call(a, '/both')
        check('HTTP 422' in result['error'] and len([1 for path, _, _ in a.requests if path == '/both']) == 2, 'both mismatches stop after two attempts')
        for path, options, fragment in (('/slow', {'timeout': 1}, 'timed'), ('/cancel', {'cancelMs': 100}, 'cancelled'), ('/budget', {'timeout': 1}, 'timed')):
            before = len(a.requests)
            result = client.call(a, path, **options)
            check(fragment in result['error'], f'{path}: {result}')
            check(result['ms'] < 1600, 'timeout/cancellation bounded')
            expected = 2 if path == '/budget' else 1
            check(len(a.requests) == before + expected, 'timeout not retried; attempts share one budget')
        time.sleep(.1)
        check(a.curl_pids and b.curl_pids, 'curl inspected for both formats')
        check(b.seen_body_private, 'official body permissions inspected')
        for server in (a, b):
            for pid in server.curl_pids:
                proc = Path(f'/proc/{pid}/stat')
                check(not proc.exists() or proc.read_text().rsplit(') ', 1)[1].startswith('Z'), 'curl killed/reaped after cancellation or completion')
            for directory in server.temp_dirs:
                check(not directory.exists(), 'private request directory removed')
        check(not a.errors and not b.errors, 'all server-side assertions passed')
        # Refused connection is a real error; this port is closed after shutdown.
        closed = Server('simple')
        port = closed.server_port
        closed.server_close()
        result = client.call(url=f'http://127.0.0.1:{port}/ocr', image=str(client.image), key=KEY)
        check('unreachable' in result['error'], 'network error reported')
    finally:
        client.close()
        for server in (a, b):
            server.shutdown()
            server.server_close()
print(f'OCR API HTTP: {checks} checks passed (offline, two fake servers)')
