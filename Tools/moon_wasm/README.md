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
   - Blue = File Header, Green = TermHeaderTable, Orange = DocData, Gray shades = PostingBlocks
4. **Right panel tabs**:
   - **File Header** — magic, version, section byte offsets
   - **Term Directory** — hierarchical physical layout tree: TermHeaderTable → DirectoryEntry → TermHeaderBlock → TermHeader → PostingBlock bytes
   - **DocData** — doc_id, importance, doc_len, filepath (if .meta loaded)
   - **Posting Blocks** — click a posting block header → expands decoded terms & highlights block bytes in hex; click a term row → highlights that term's posting bytes
   - **Search** — type a query; results show doc scores + paths
5. **Filter box** (top) — type to filter term rows live; if 3+ chars, also runs an index search
