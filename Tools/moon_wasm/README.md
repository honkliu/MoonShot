# moon_wasm — MoonShot Index Viewer

Interactive browser-based viewer for `.idx` files, powered by WebAssembly
(compiled from RustBlade).

## Build

```bash
# Install wasm-pack once
cargo install wasm-pack

# Build the WASM package
wasm-pack build --target web --release
```

## Run

Because ES modules need HTTP (not `file://`), serve the directory:

```bash
# Python
python3 -m http.server 8080

# Node
npx serve .
```

Then open `http://localhost:8080`.

## Usage

1. Drag & drop a `.idx` file — or click **Open .idx**
2. Optionally load the companion `.idx.meta` for doc-id → filepath mapping
3. **Left panel** — hex view of the file; bytes coloured by section:
   - Blue = Header, Green = SubIndex, Orange = DocData, Gray shades = Blocks
4. **Right panel tabs**:
   - **Header** — magic, version, section byte offsets
   - **SubIndex** — click a row → jumps to that block in hex + Blocks tab
   - **DocData** — doc_id, importance, doc_len, filepath (if .meta loaded)
   - **Blocks** — click a block header → expands terms & highlights block bytes in hex; click a term row → highlights that term's bytes
   - **Search** — type a query; results show doc scores + paths
5. **Filter box** (top) — type to filter term rows live; if 3+ chars, also runs an index search
