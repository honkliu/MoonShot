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
   - Blue = File Header, Green = Head/Leaf term table, Orange = DocData, Gray shades = IndexBlocks
4. **Right panel tabs**:
   - **File Header** — magic, version, section byte offsets
   - **Head/Leaf Terms** — hierarchical physical layout tree: Head/Leaf term table → HeadTermEntry → LeafTermBlock → LeafTermEntry → IndexEntry bytes
   - **DocData** — doc_id, importance, doc_len, filepath (if .meta loaded)
   - **IndexBlocks** — click an IndexBlock header → expands decoded terms and highlights block bytes in hex; click a term row → highlights that term's IndexEntry bytes
   - **Search** — real MoonShot document search; results show filename, path, doc id, and score; click a result to display file content in the page
5. **Filter box** (top) — filters visible term rows only; it does not run document search
