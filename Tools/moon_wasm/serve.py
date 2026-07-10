#!/usr/bin/env python3
"""
MoonShot index viewer server.

Usage:
    python3 serve.py                    # viewer only, upload idx manually
    python3 serve.py ~/moon.idx         # auto-load idx at startup
    python3 serve.py ~/moon.idx 8080    # custom port
"""
import sys, os, json, struct, http.server, socketserver, urllib.parse, mimetypes, socket

idx_path = sys.argv[1] if len(sys.argv) > 1 else ''
port     = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
LARGE_INDEX_COPY_LIMIT = 128 * 1024 * 1024
DOC_REC_SIZE = 256
DOC_PATH_MAX = 64
DOC_PATH_OFFSET = 192
PATH_PREFIX_SIDECAR_BYTES = 20 * 4096
PATH_PREFIX_SIDECAR_MAGIC = b'MSPATHS\0'
resolved_doc_path_cache = {}
served_prefix_cache = None

def decode_path_prefix_sidecar(sidecar):
    if len(sidecar) != PATH_PREFIX_SIDECAR_BYTES or sidecar[:8] != PATH_PREFIX_SIDECAR_MAGIC:
        return []
    version, count = struct.unpack_from('<HH', sidecar, 8)
    if version != 1:
        return []
    entry_off = struct.unpack_from('<I', sidecar, 12)[0]
    string_off = struct.unpack_from('<I', sidecar, 16)[0]
    string_bytes = struct.unpack_from('<I', sidecar, 20)[0]
    if entry_off < 32 or string_off > len(sidecar) or string_off + string_bytes > len(sidecar):
        return []
    prefixes = []
    for index in range(count):
        off = entry_off + index * 8
        if off + 8 > len(sidecar):
            break
        str_off, length, _flags = struct.unpack_from('<IHH', sidecar, off)
        begin = string_off + str_off
        end = begin + length
        if end > string_off + string_bytes or end > len(sidecar):
            break
        prefixes.append(sidecar[begin:end].decode('utf-8', errors='replace'))
    return prefixes

def decode_doc_path(payload, prefixes):
    if not payload:
        return ''
    if len(payload) < 2:
        return payload.decode('utf-8', errors='replace')
    prefix_id = struct.unpack_from('<H', payload, 0)[0]
    filename = payload[2:].decode('utf-8', errors='replace')
    if prefix_id == 0xffff or prefix_id >= len(prefixes):
        return filename
    prefix = prefixes[prefix_id]
    if not prefix:
        return filename
    sep = '\\' if '\\' in prefix else '/'
    return prefix + filename if prefix.endswith(('/', '\\')) else prefix + sep + filename

def load_served_prefixes():
    global served_prefix_cache
    if served_prefix_cache is not None:
        return served_prefix_cache
    if not served_idx_abs or not os.path.isfile(served_idx_abs):
        served_prefix_cache = []
        return served_prefix_cache
    try:
        with open(served_idx_abs, 'rb') as f:
            f.seek(136)
            served_prefix_cache = decode_path_prefix_sidecar(f.read(PATH_PREFIX_SIDECAR_BYTES))
    except OSError:
        served_prefix_cache = []
    return served_prefix_cache

def indexed_search_roots(prefixes):
    roots = []
    seen = set()
    for prefix in prefixes:
        if not prefix:
            continue
        norm = os.path.normpath(prefix)
        drive, tail = os.path.splitdrive(norm)
        parts = [part for part in tail.replace('/', os.sep).split(os.sep) if part]
        if drive and parts:
            root = os.path.join(drive + os.sep, parts[0])
        elif os.path.isabs(norm) and parts:
            root = os.path.join(os.sep, parts[0])
        else:
            continue
        key = os.path.normcase(root)
        if key not in seen and os.path.isdir(root):
            seen.add(key)
            roots.append(root)
    return roots

def resolve_existing_doc_path(path, prefixes):
    if not path:
        return path
    if os.path.isabs(path) and os.path.isfile(path):
        return path
    cache_key = os.path.normcase(path)
    if cache_key in resolved_doc_path_cache:
        return resolved_doc_path_cache[cache_key] or path

    for prefix in prefixes:
        if not prefix:
            continue
        candidate = os.path.join(prefix, path)
        if os.path.isfile(candidate):
            resolved_doc_path_cache[cache_key] = candidate
            return candidate

    basename = os.path.basename(path)
    if basename:
        suffix = os.path.normcase(os.path.normpath(path))
        has_dir = os.path.dirname(path) not in ('', '.')
        for root in indexed_search_roots(prefixes):
            for current_root, _dirs, files in os.walk(root):
                if basename not in files:
                    continue
                candidate = os.path.join(current_root, basename)
                if has_dir and not os.path.normcase(candidate).endswith(suffix):
                    continue
                resolved_doc_path_cache[cache_key] = candidate
                return candidate

    resolved_doc_path_cache[cache_key] = ''
    return path

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
                    header = f.read(136)
                    if len(header) < 136 or header[:8] != b'MOONSHOT':
                        self.send_error(400, 'invalid idx header')
                        return
                    num_docs = struct.unpack_from('<Q', header, 16)[0]
                    docdata_off = struct.unpack_from('<Q', header, 64)[0]
                    f.seek(136)
                    prefixes = decode_path_prefix_sidecar(f.read(PATH_PREFIX_SIDECAR_BYTES))
                    first_doc_id = 0
                    if num_docs:
                        f.seek(docdata_off)
                        first = f.read(DOC_REC_SIZE)
                        if len(first) == DOC_REC_SIZE:
                            first_doc_id = struct.unpack_from('<I', first, 0)[0]
                    out = {}
                    for doc_id in ids:
                        ordinal = doc_id - first_doc_id
                        if ordinal < 0 or ordinal >= num_docs:
                            continue
                        f.seek(docdata_off + ordinal * DOC_REC_SIZE)
                        rec = f.read(DOC_REC_SIZE)
                        if len(rec) < DOC_REC_SIZE:
                            continue
                        stored_doc_id = struct.unpack_from('<I', rec, 0)[0]
                        if stored_doc_id != doc_id:
                            continue
                        path_len = min(struct.unpack_from('<H', rec, 18)[0], DOC_PATH_MAX)
                        path = decode_doc_path(rec[DOC_PATH_OFFSET:DOC_PATH_OFFSET + path_len], prefixes) if path_len else ''
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

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != '/embed-bge':
            self.send_error(404, 'not found')
            return
        try:
            length = int(self.headers.get('Content-Length', '0'))
            body = self.rfile.read(min(length, 65536 + 1024))
            request = json.loads(body.decode('utf-8'))
            host = str(request.get('host') or '127.0.0.1')
            port = int(request.get('port') or 8765)
            text = str(request.get('text') or '')
            if not text:
                raise ValueError('empty text')
            if port <= 0 or port > 65535:
                raise ValueError('invalid port')
            payload = text.encode('utf-8')[:65536]
            with socket.create_connection((host, port), timeout=20) as sock:
                sock.sendall(struct.pack('<I', len(payload)))
                sock.sendall(payload)
                dim_bytes = self._recv_exact(sock, 4)
                dim = struct.unpack('<I', dim_bytes)[0]
                if dim <= 0 or dim > 4096:
                    raise ValueError(f'invalid embedding dim {dim}')
                vector_bytes = self._recv_exact(sock, dim)
            values = struct.unpack(f'<{dim}b', vector_bytes)
            response = {'dim': dim, 'vector': [value / 128.0 for value in values]}
            data = json.dumps(response).encode('utf-8')
            self.send_response(200)
        except Exception as exc:
            data = json.dumps({'error': str(exc)}).encode('utf-8')
            self.send_response(500)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    @staticmethod
    def _recv_exact(sock, size):
        chunks = []
        remaining = size
        while remaining:
            chunk = sock.recv(remaining)
            if not chunk:
                raise ConnectionError('embedding service closed connection')
            chunks.append(chunk)
            remaining -= len(chunk)
        return b''.join(chunks)

print(f'MoonShot viewer  →  http://localhost:{port}/index.html')
if config_val:
    print(f'Auto-loading     →  {idx_path}')
else:
    print('No idx specified  →  use the Open / Drop zone in the browser')

with ThreadingReusableTCPServer(('', port), MoonShotHandler) as srv:
    srv.serve_forever()
