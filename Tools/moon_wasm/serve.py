#!/usr/bin/env python3
"""
MoonShot index viewer server.

Usage:
    python3 serve.py                    # viewer only, upload idx manually
    python3 serve.py ~/moon.idx         # auto-load idx at startup
    python3 serve.py ~/moon.idx 8080    # custom port
"""
import sys, os, json, struct, http.server, socketserver, urllib.parse, mimetypes

idx_path = sys.argv[1] if len(sys.argv) > 1 else ''
port     = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
LARGE_INDEX_COPY_LIMIT = 128 * 1024 * 1024

# Write a tiny config that index.html reads to auto-load the idx.
# Path is made relative to the HTTP server root (this script's directory).
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)

served_idx_abs = ''
served_idx_size = 0

if idx_path:
    abs_idx = os.path.abspath(idx_path)
    try:
        rel = os.path.relpath(abs_idx, script_dir)
        # relpath on Windows raises ValueError across drives; also reject paths
        # that escape the serving root (start with ..) since the HTTP server
        # cannot reach them.
        if rel.startswith('..'):
            raise ValueError('outside serving root')
    except ValueError:
        # Cross-drive or outside root. Small files are copied for ordinary static
        # loading; large files are exposed through /idx-range so startup does not
        # duplicate a multi-GB index before the viewer can show a header summary.
        if os.path.getsize(abs_idx) > LARGE_INDEX_COPY_LIMIT:
            rel = '__configured_idx__'
            print(f'Large idx range-served  →  {abs_idx}')
        else:
            import shutil
            dest = os.path.join(script_dir, 'moon.idx')
            if abs_idx != dest:
                shutil.copy2(abs_idx, dest)
                print(f'Copied idx  →  {dest}')
            abs_idx = dest
            rel = 'moon.idx'
    config_val = rel.replace('\\', '/')
    served_idx_abs = abs_idx
    served_idx_size = os.path.getsize(abs_idx) if os.path.isfile(abs_idx) else 0
else:
    config_val = ''

with open('moonshot-config.js', 'w') as f:
    f.write(f'window.MOONSHOT_IDX = {json.dumps(config_val)};\n')
    f.write(f'window.MOONSHOT_IDX_SIZE = {served_idx_size};\n')

class ThreadingReusableTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class MoonShotHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()

    def do_GET(self):
        # The viewer changes frequently during development; ignore browser
        # conditional-cache headers so stale index.html / wasm bundles never win.
        for header in ('If-Modified-Since', 'If-None-Match'):
            if header in self.headers:
                del self.headers[header]

        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == '/':
            self.path = '/index.html'
            parsed = urllib.parse.urlparse(self.path)

        if parsed.path == '/idx-range':
            if not served_idx_abs or not os.path.isfile(served_idx_abs):
                self.send_error(404, 'no configured idx')
                return

            query = urllib.parse.parse_qs(parsed.query)
            try:
                start = int(query.get('start', ['0'])[0])
                length = int(query.get('length', ['96'])[0])
            except ValueError:
                self.send_error(400, 'invalid range')
                return

            size = os.path.getsize(served_idx_abs)
            start = max(0, min(start, size))
            length = max(0, min(length, size - start))

            try:
                with open(served_idx_abs, 'rb') as f:
                    f.seek(start)
                    data = f.read(length)
            except OSError as exc:
                self.send_error(500, str(exc))
                return

            self.send_response(206)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Range', f'bytes {start}-{start + len(data) - 1}/{size}' if data else f'bytes */{size}')
            self.send_header('X-File-Size', str(size))
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        if parsed.path == '/doc-paths':
            if not served_idx_abs or not os.path.isfile(served_idx_abs):
                self.send_error(404, 'no configured idx')
                return

            query = urllib.parse.parse_qs(parsed.query)
            raw_ids = query.get('ids', [''])[0]
            try:
                ids = [int(x) for x in raw_ids.split(',') if x.strip()]
            except ValueError:
                self.send_error(400, 'invalid ids')
                return

            try:
                with open(served_idx_abs, 'rb') as f:
                    header = f.read(96)
                    if len(header) < 96 or header[:8] != b'MOONSHOT':
                        self.send_error(400, 'invalid idx header')
                        return
                    num_docs = struct.unpack_from('<Q', header, 16)[0]
                    docdata_off = struct.unpack_from('<Q', header, 56)[0]
                    out = {}
                    for doc_id in ids:
                        if doc_id < 1 or doc_id > num_docs:
                            continue
                        f.seek(docdata_off + (doc_id - 1) * 1024)
                        rec = f.read(1024)
                        if len(rec) < 1024:
                            continue
                        path_len = min(struct.unpack_from('<H', rec, 16)[0], 999)
                        path = rec[24:24 + path_len].decode('utf-8', errors='replace') if path_len else ''
                        out[str(doc_id)] = path
            except OSError as exc:
                self.send_error(500, str(exc))
                return

            data = json.dumps(out).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        if parsed.path == '/file':
            query = urllib.parse.parse_qs(parsed.query)
            raw_path = query.get('path', [''])[0]
            if not raw_path:
                self.send_error(400, 'missing path')
                return

            file_path = os.path.abspath(os.path.expanduser(raw_path))
            if not os.path.isfile(file_path):
                self.send_error(404, 'file not found')
                return

            try:
                with open(file_path, 'rb') as f:
                    data = f.read()
            except OSError as exc:
                self.send_error(500, str(exc))
                return

            self.send_response(200)
            content_type = mimetypes.guess_type(file_path)[0] or 'application/octet-stream'
            if content_type.startswith('text/') or content_type in ('application/json', 'application/xml'):
                content_type += '; charset=utf-8'
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        return super().do_GET()

print(f'MoonShot viewer  →  http://localhost:{port}/index.html')
if config_val:
    print(f'Auto-loading     →  {idx_path}')
else:
    print('No idx specified  →  use the Open / Drop zone in the browser')

with ThreadingReusableTCPServer(('', port), MoonShotHandler) as srv:
    srv.serve_forever()
