#!/usr/bin/env python3
"""
MoonShot index viewer server.

Usage:
    python3 serve.py                    # viewer only, upload idx manually
    python3 serve.py ~/moon.idx         # auto-load idx at startup
    python3 serve.py ~/moon.idx 8080    # custom port
"""
import sys, os, json, http.server, socketserver

idx_path = sys.argv[1] if len(sys.argv) > 1 else ''
port     = int(sys.argv[2]) if len(sys.argv) > 2 else 8080

# Write a tiny config that index.html reads to auto-load the idx.
# Path is made relative to the HTTP server root (this script's directory).
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)

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
        # Cross-drive or outside root: copy the file into the serving directory.
        import shutil
        dest = os.path.join(script_dir, 'moon.idx')
        if abs_idx != dest:
            shutil.copy2(abs_idx, dest)
            print(f'Copied idx  →  {dest}')
        rel = 'moon.idx'
    config_val = rel.replace('\\', '/')
else:
    config_val = ''

with open('moonshot-config.js', 'w') as f:
    f.write(f'window.MOONSHOT_IDX = {json.dumps(config_val)};\n')

print(f'MoonShot viewer  →  http://localhost:{port}/index.html')
if config_val:
    print(f'Auto-loading     →  {idx_path}')
else:
    print('No idx specified  →  use the Open / Drop zone in the browser')

with socketserver.TCPServer(('', port), http.server.SimpleHTTPRequestHandler) as srv:
    srv.serve_forever()
