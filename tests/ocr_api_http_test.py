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

DRIVER, CLI, SAMPLE = sys.argv[1:4]
SAMPLE_PNG = Path(SAMPLE).read_bytes()
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
        self.word_requests = []
        self.expected_key = KEY
        self.expected_png = PNG
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
                        for body in (parent / 'request.json', parent / 'request-lines.json'):
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
                word_flag = False
                if official:
                    body = json.loads(raw)
                    check(base64.b64decode(body['file'], validate=True) == server.expected_png, 'official body has valid complete base64 PNG')
                    word_flag = body.get('returnWordBox', False)
                    expected_body = {'file': body['file'], 'fileType': 1, 'useDocOrientationClassify': False, 'useDocUnwarping': False, 'useTextlineOrientation': False, 'visualize': False}
                    if word_flag:
                        expected_body['returnWordBox'] = True
                    else:
                        check(any(path == self.path and fmt == 'paddlex' and flag for path, fmt, flag in server.word_requests), 'official field omitted only after flagged probe')
                    check(body == expected_body, 'official JSON flags')
                else:
                    message = email.parser.BytesParser(policy=email.policy.default).parsebytes(b'Content-Type: ' + self.headers['Content-Type'].encode() + b'\r\n\r\n' + raw)
                    parts = list(message.iter_parts())
                    check(len(parts) in (1, 2), 'file and optional word field only')
                    if len(parts) == 2:
                        check(parts[1].get_param('name', header='content-disposition') == 'return_word_box' and parts[1].get_payload(decode=True) == b'true', 'request word boxes')
                        word_flag = True
                    else:
                        check(any(path == self.path and fmt == 'simple' and flag for path, fmt, flag in server.word_requests), 'field omitted only after a flagged request to the same URL')
                    part = parts[0]
                    check(part.get_param('name', header='content-disposition') == 'file', 'multipart field file')
                    check(part.get_filename() == 'image.png' and part.get_content_type() == 'image/png', 'multipart filename and type')
                    check(part.get_payload(decode=True) == server.expected_png, 'multipart PNG bytes')
                server.word_requests.append((self.path, fmt, word_flag))
            code, response = 200, OFFICIAL if official else SIMPLE
            if self.path.startswith('/reject-field') and word_flag:
                code = 400 if '-400' in self.path else 422
                response = {'detail': [{'type': 'extra_forbidden', 'loc': ['body', 'returnWordBox' if official else 'return_word_box'], 'msg': 'Extra inputs are not permitted'}]}
                if code == 400:
                    response = {'errorCode': 400, 'errorMsg': 'Unsupported parameter: ' + ('returnWordBox' if official else 'return_word_box')}
                if self.path == '/reject-field-generic':
                    response = {'detail': 'validation failed'}
            elif self.path == '/words':
                response = json.loads(json.dumps(response))
                if (server.kind == 'paddlex') != official:
                    code = 415
                if official:
                    pruned = response['result']['ocrResults'][0]['prunedResult']
                    pruned['text_word'] = [['he', ' ', 'llo']]
                    pruned['text_word_boxes'] = [[[0,0,8,10], None, [8,0,20,10]]]
                else:
                    response['results'][0]['text_word'] = ['he', ' ', 'llo']
                    response['results'][0]['text_word_boxes'] = [[[0,0],[8,0],[8,10],[0,10]], None, [8,0,20,10]]
            elif self.path in ('/slow', '/cancel'):
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
            check(len(formats) <= 4 and formats.count('paddlex') <= 2 and formats.count('simple') <= 2, 'two formats with one field retry each')
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
        for server in (a, b):
            result = client.call(server, '/words')
            check([word['text'] for word in result['words'][0]] == ['he', 'llo'], 'HTTP words parsed and whitespace dropped')
            check(all(word['points'] == 4 for word in result['words'][0]), 'HTTP word geometry parsed')
        result = client.call(a, '/reject-field')
        check(result['text'] == 'hello', '422 unknown form field retries same format')
        check([(fmt, flag) for path, fmt, flag in a.word_requests if path == '/reject-field'] == [('simple', True), ('simple', False)], 'exactly one retry without field')
        before = len(a.requests)
        check(client.call(a, '/reject-field')['text'] == 'hello' and len(a.requests) == before + 1, 'memory cache remembers rejected field')
        check(client.call(a, '/reject-field-generic')['text'] == 'hello', 'generic 422 also probes same format without field')
        check([(fmt, flag) for path, fmt, flag in a.word_requests if path == '/reject-field-generic'] == [('simple', True), ('simple', False)], 'generic 422 exactly one field retry')
        before = len(a.requests)
        check(client.call(a, '/reject-field/layout-parsing')['text'] == 'hello', 'official guess can fall back to simple then retry field')
        check([fmt for _, fmt, _ in a.requests[before:]] == ['paddlex', 'paddlex', 'simple', 'simple'], 'four-request reverse detection bound')
        reject_hash = hashlib.sha256(f'http://127.0.0.1:{a.server_port}/reject-field'.encode()).hexdigest()
        # Both formats and both rejection statuses must degrade without an error.
        for server in (a, b):
            for code in (400, 422):
                path = f'/reject-field-{code}' + ('/layout-parsing' if server is b else '')
                before = len(server.requests)
                result = client.call(server, path)
                check(result['text'] == 'hello' and not result['error'] and result['words'] == [[]], 'rejected parameter falls back to line-only result')
                fmt = 'paddlex' if server is b else 'simple'
                check([(f, flag) for p, f, flag in server.word_requests if p == path] == [(fmt, True), (fmt, False)], 'same format retried once without optional parameter')
                before = len(server.requests)
                check(client.call(server, path)['text'] == 'hello' and len(server.requests) == before + 1, 'rejection remembered in memory for both formats')
        cache_file = client.cache / 'hyprcapture/ocr-api-format.json'
        entries = json.loads(cache_file.read_text())
        check(stat.S_IMODE(cache_file.stat().st_mode) == 0o600, 'cache file 0600')
        for server, fmt in ((a, 'simple'), (b, 'paddlex')):
            url = f'http://127.0.0.1:{server.server_port}/ocr'
            check(entries[hashlib.sha256(url.encode()).hexdigest()] == fmt, 'cache uses SHA-256 URL keys')
        check('http' not in cache_file.read_text() and KEY not in cache_file.read_text(), 'cache stores no URL or API key')
        check(isinstance(entries[reject_hash + ':no_word_box']['recorded_at'], int) and entries[reject_hash + ':no_word_box']['recorded_at'] > 0 and entries[reject_hash] == 'simple', 'backward compatible cache flag with timestamp')
        client.close()
        client = Client(root)
        before_reject = len(a.requests)
        check(client.call(a, '/reject-field')['text'] == 'hello' and len(a.requests) == before_reject + 1, 'disk cache remembers rejected field')
        for server in (a, b):
            for code in (400, 422):
                path = f'/reject-field-{code}' + ('/layout-parsing' if server is b else '')
                before = len(server.requests)
                check(client.call(server, path)['text'] == 'hello' and len(server.requests) == before + 1, 'rejection remembered on disk for both formats/statuses')
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
        check([fmt for path, fmt, _ in a.requests if path == '/400'] == ['simple', 'simple', 'paddlex'], '400 probes without field before changing format')
        check(client.call(a, '/change')['text'] == 'hello', 'prime simple cache')
        a.kind = 'paddlex'
        check(client.call(a, '/change')['text'] == 'hello', 'stale cache re-detects')
        check([fmt for path, fmt, _ in a.requests if path == '/change'] == ['simple', 'simple', 'simple', 'paddlex'], 'stale format with one field probe then the other format')
        before = len(a.requests)
        check(client.call(a, '/change')['text'] == 'hello' and len(a.requests) == before + 1, 're-detected format cached')
        change_hash = hashlib.sha256(f'http://127.0.0.1:{a.server_port}/change'.encode()).hexdigest()
        check(change_hash + ':no_word_box' not in json.loads(cache_file.read_text()), 'unrelated 422 does not cache a rejected field')
        check(json.loads(cache_file.read_text())[change_hash] == 'paddlex', 'stale persistent entry replaced')
        a.kind = 'simple'
        before = len(a.requests)
        check(client.call(a, '/change')['text'] == 'hello', 'stale official cache switches directly to simple')
        check([fmt for _, fmt, _ in a.requests[before:]] == ['paddlex', 'paddlex', 'simple'], 'reverse stale cache probes official without flag then switches')
        check(json.loads(cache_file.read_text())[change_hash] == 'simple', 'reverse stale persistent entry replaced')
        a.kind = 'none'
        before = len(a.requests)
        result = client.call(a, '/change')
        check('HTTP 422' in result['error'], 'both formats failing after stale cache returns error')
        check([fmt for _, fmt, _ in a.requests[before:]] == ['simple', 'simple', 'paddlex', 'paddlex'], 'stale cache failure bounded to four probes')
        check(change_hash not in json.loads(cache_file.read_text()), 'failed stale cache entry removed from disk')
        a.kind = 'paddlex'
        before = len(a.requests)
        check(client.call(a, '/change')['text'] == 'hello', 'call after failed stale detection starts fresh')
        check([fmt for _, fmt, _ in a.requests[before:]] == ['simple', 'simple', 'paddlex'], 'failed stale cache entry removed from memory')
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
        check('HTTP 422' in result['error'] and len([1 for path, _, _ in a.requests if path == '/both']) == 4, 'both mismatches stop after four probes')
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
        # CLI cache tools and forced probes share the production cache/engine.
        cli_env = {**os.environ, 'XDG_CACHE_HOME': str(client.cache), 'XDG_RUNTIME_DIR': str(root)}
        def cli(*args, server=None, env=None, expected=0):
            if server:
                server.expected_key = KEY
                server.expected_png = SAMPLE_PNG
            process = subprocess.Popen([CLI, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env or cli_env)
            if server:
                server.driver_pid = process.pid
            output, error = process.communicate(timeout=15)
            check(process.returncode == expected, f'CLI exit: {output} {error}')
            check(KEY not in output + error, 'key absent from CLI output')
            check(KEY not in '\0'.join([CLI, *args]), 'key absent from CLI argv')
            if server:
                check(not server.errors, str(server.errors))
                server.expected_png = PNG
            return output
        for args in [('--help',), ('cache', '--help'), ('cache', 'show', '--help'), ('cache', 'clear', '--help'), ('probe', '--help')]:
            check('Usage:' in cli(*args), 'CLI help')
        for args in [(), ('cache',), ('cache', 'unknown'), ('probe', '--url'), ('cache', 'show', '--key-file', 'x')]:
            cli(*args, expected=64)
        show = cli('cache', 'show', '--url', f'http://127.0.0.1:{a.server_port}/reject-field')
        check('* ' + reject_hash[:12] in show and 'age=' in show and 'expires-in=' in show, 'show marks URL hash and rejection age/expiry')
        check('http://' not in show and KEY not in show, 'show prints hashes only')
        before = len(a.requests)
        clear = cli('cache', 'clear', '--url', f'http://127.0.0.1:{a.server_port}/reject-field')
        check('Removed ' + reject_hash[:12] in clear, 'scoped clear reports removed hash')
        check(client.call(a, '/reject-field')['text'] == 'hello' and len(a.requests) == before + 2, 'same helper reloads external clear before next OCR')
        cli('cache', 'clear')
        check('No cached entries' in cli('cache', 'show'), 'clear all entries')
        check('No cached entries' in cli('cache', 'clear'), 'clear missing entries succeeds')
        # Legacy string-only cache remains inspectable and clearable.
        legacy_url = 'http://legacy.example/ocr'
        legacy_hash = hashlib.sha256(legacy_url.encode()).hexdigest()
        legacy = {legacy_hash: 'simple', 'b' * 64: 'paddlex'}
        cache_file.write_text(json.dumps(legacy)); cache_file.chmod(0o600)
        show = cli('cache', 'show')
        check(legacy_hash[:12] + ' simple' in show and 'bbbbbbbbbbbb paddlex' in show, 'old cache show')
        output = cli('cache', 'clear', '--url', legacy_url)
        check('Removed ' + legacy_hash[:12] in output and json.loads(cache_file.read_text()) == {'b' * 64: 'paddlex'}, 'old cache scoped clear retains other URLs')
        cli('cache', 'clear')
        key_file = root / 'api-key'
        key_file.write_text(KEY + '\n'); key_file.chmod(0o600)
        for server in (a, b):
            for path, count in [('/ocr', 0), ('/words', 2)]:
                url = f'http://127.0.0.1:{server.server_port}{path}'
                before = len(server.requests)
                output = cli('probe', '--url', url, '--key-file', str(key_file), server=server)
                fmt = 'paddlex' if server is b else 'simple'
                check(f'format: {fmt}' in output and f'words: {count}' in output and 'lines: 1' in output and 'latency:' in output, f'probe reports format, words, lines and latency: {fmt}/{count}: {output}')
                check(('word boxes: yes' if count else 'word boxes: not returned') in output, 'probe reports observed word support')
                h = hashlib.sha256(url.encode()).hexdigest()
                check(json.loads(cache_file.read_text())[h] == fmt, 'probe updates cache')
                # Force probe ignores the known format on every invocation.
                before = len(server.requests)
                cli('probe', '--url', url, '--key-file', str(key_file), server=server)
                check([fmt for _, fmt, _ in server.requests[before:]] == (['simple', 'paddlex'] if server is b else ['simple']), 'probe bypasses cached format')
        # Probe bypasses an unexpired rejected-parameter flag as well.
        url = f'http://127.0.0.1:{a.server_port}/words'
        h = hashlib.sha256(url.encode()).hexdigest()
        entries = json.loads(cache_file.read_text()); entries[h + ':no_word_box'] = {'recorded_at': int(time.time())}
        cache_file.write_text(json.dumps(entries))
        output = cli('probe', '--url', url, '--key-file', str(key_file), server=a)
        check('words: 2' in output and h + ':no_word_box' not in json.loads(cache_file.read_text()), 'successful forced probe clears rejection')
        key_file.chmod(0o644)
        before = len(a.requests)
        cli('probe', '--url', url, '--key-file', str(key_file), expected=1)
        check(len(a.requests) == before, 'unsafe key file rejected before network request')
        key_file.chmod(0o600)
        # Read defaults from a trusted fixture hyprctl, never the host session.
        fake_hyprctl = root / 'hyprctl'
        options = {'plugin:hyprcapture:ocr_api_url': {'str': url}, 'plugin:hyprcapture:ocr_api_key': {'str': KEY}, 'plugin:hyprcapture:ocr_timeout': {'int': 5}}
        fake_hyprctl.write_text('#!/usr/bin/python3\nimport json,sys\nassert sys.argv[1:3] == ["getoption","-j"]\nprint(' + repr(options) + '[sys.argv[3]] and json.dumps(' + repr(options) + '[sys.argv[3]]))\n')
        fake_hyprctl.chmod(0o700)
        output = cli('probe', server=a, env={**cli_env, 'PATH': str(root) + ':' + os.environ['PATH']})
        check('format: simple' in output and 'words: 2' in output, 'probe uses hyprctl defaults with key kept private')
        cli('probe', '--url', f'http://127.0.0.1:{a.server_port}/500', '--key-file', str(key_file), server=a, expected=1)
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
