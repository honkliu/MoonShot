/*
 * wasm_api.rs — wasm-bindgen exports for the MoonShot index viewer.
 *
 * Exposed to JavaScript:
 *   parse_index_summary(bytes: Uint8Array, file_size: usize) -> String (JSON)
 *   parse_index(bytes: Uint8Array) -> String (JSON)
 *   search_index(bytes: Uint8Array, query: String, streams: String) -> String (JSON)
 */

use wasm_bindgen::prelude::*;
use crate::serializer::IndexSerializer;
use crate::posting_store::PostingStore;
use crate::block_table::{PAGE_SIZE, IB_HEADER_HAS_MORE, IB_DATA_OFFSET};
use crate::index_context::IndexContext;

// ── parse_index_summary ─────────────────────────────────────────────────────

/// Parse only the fixed 96-byte header. This is safe for multi-GB indexes and
/// lets the browser show metadata without expanding the entire file to JSON.
#[wasm_bindgen]
pub fn parse_index_summary(data: &[u8], file_size: usize) -> String {
    match parse_index_summary_inner(data, file_size) {
        Ok(s)  => s,
        Err(e) => format!(r#"{{"error":"{}"}}"#, e),
    }
}

fn parse_index_summary_inner(data: &[u8], file_size: usize) -> Result<String, String> {
    if data.len() < 96 {
        return Err("Index header is shorter than 96 bytes".to_string());
    }
    if &data[0..8] != b"MOONSHOT" {
        return Err("Not a valid MOONSHOT index (bad magic)".to_string());
    }
    let version = u32_at(data, 8);
    if version != 7 {
        return Err(format!("Unsupported index version {}; expected 7", version));
    }

    let hdr = parse_header(data);
    Ok(format!(
        r#"{{"header":{{"size":96,"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"subindex_off":{},"subindex_size":{},"pageskip_off":{},"pageskip_size":{},"docdata_off":{},"docdata_size":{},"blocks_off":{},"num_blocks":{},"ib_data_off":{},"file_size":{}}},"head_leaf_term_table":{{"head":[],"leaf_blocks":[]}},"docdata":[],"blocks":[],"summary_only":true}}"#,
        hdr.version, hdr.num_docs, hdr.num_terms,
        hdr.subindex_off, hdr.subindex_size,
        hdr.pageskip_off, hdr.pageskip_size,
        hdr.docdata_off, hdr.docdata_size,
        hdr.blocks_off, hdr.num_blocks, IB_DATA_OFFSET, file_size
    ))
}

// ── parse_index ──────────────────────────────────────────────────────────────

/// Parse a .idx file from raw bytes and return a JSON description of every
/// section (Header, Head/Leaf term table, DocData, IndexBlocks with decoded entries).
#[wasm_bindgen]
pub fn parse_index(data: &[u8]) -> String {
    match parse_index_inner(data) {
        Ok(s)  => s,
        Err(e) => format!(r#"{{"error":"{}"}}"#, e),
    }
}

fn parse_index_inner(data: &[u8]) -> Result<String, String> {
    if !IndexSerializer::is_valid_bytes(data) {
        return Err("Not a valid MOONSHOT index (bad magic)".to_string());
    }
    if data.len() >= 12 {
        let version = u32_at(data, 8);
        if version != 7 {
            return Err(format!("Unsupported index version {}; expected 7", version));
        }
    }

    let mut store = PostingStore::new();
    let (head_term_entries, leaf_term_blocks, blocks, _pageskip) = IndexSerializer::decode(&mut store, data)
        .map_err(|e| format!("{e:?}"))?;

    // Flatten LeafTermEntries for display and IndexBlock matching.
    let leaf_term_entries: Vec<&crate::block_table::LeafTermEntry> =
        leaf_term_blocks.iter().flat_map(|b| b.ltb_entries.iter()).collect();

    // Build IndexBlockID -> [LeafTermEntry] map once, reused per block (O(N) not O(N x M)).
    let mut block_entry_map: std::collections::HashMap<u32, Vec<&crate::block_table::LeafTermEntry>> =
        std::collections::HashMap::new();
    for e in &leaf_term_entries {
        block_entry_map.entry(e.lte_index_block_id).or_default().push(e);
    }

    let file_size = data.len();
    let hdr = parse_header(data);

    let mut head_ranges: Vec<(usize, usize)> = Vec::with_capacity(head_term_entries.len());
    let mut block_ranges: Vec<(usize, usize, Vec<(usize, usize)>)> =
        Vec::with_capacity(leaf_term_blocks.len());
    let mut table_pos = hdr.subindex_off as usize + 4; // dir_count
    for d in &head_term_entries {
        let len = 2 + d.hte_first_term.len() + 4;
        head_ranges.push((table_pos, len));
        table_pos += len;
    }
    table_pos += 4; // block_count
    for block in &leaf_term_blocks {
        let block_start = table_pos;
        let mut entry_pos = block_start + 4; // entry_count
        let mut entry_ranges = Vec::with_capacity(block.ltb_entries.len());
        for entry in &block.ltb_entries {
            let len = 2 + entry.lte_term.len() + 28;
            entry_ranges.push((entry_pos, len));
            entry_pos += len;
        }
        block_ranges.push((block_start, PAGE_SIZE, entry_ranges));
        table_pos += PAGE_SIZE;
    }

    let mut out = String::from("{");

    // Header
    out.push_str(&format!(
        r#""header":{{"size":96,"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"subindex_off":{},"subindex_size":{},"pageskip_off":{},"pageskip_size":{},"docdata_off":{},"docdata_size":{},"blocks_off":{},"num_blocks":{},"ib_data_off":{},"file_size":{}}},"#,
        hdr.version, hdr.num_docs, hdr.num_terms,
        hdr.subindex_off, hdr.subindex_size,
        hdr.pageskip_off, hdr.pageskip_size,
        hdr.docdata_off, hdr.docdata_size,
        hdr.blocks_off, hdr.num_blocks, IB_DATA_OFFSET, file_size
    ));

    // Head/Leaf term table — show two-level structure.
    out.push_str(r#""head_leaf_term_table":{"head":["#);
    for (i, d) in head_term_entries.iter().enumerate() {
        if i > 0 { out.push(','); }
        let (byte_offset, byte_len) = head_ranges[i];
        let block_id = d.hte_leaf_term_block_id as usize;
        let (block_offset, block_len) = block_ranges
            .get(block_id)
            .map(|(offset, len, _)| (*offset, *len))
            .unwrap_or((0, 0));
        out.push_str(&format!(
            r#"{{"hte_first_term":{},"hte_leaf_term_block_id":{},"byte_offset":{},"byte_len":{},"leaf_term_block_offset":{},"leaf_term_block_len":{}}}"#,
            serde_json::to_string(&d.hte_first_term).unwrap_or_default(),
            d.hte_leaf_term_block_id,
            byte_offset, byte_len, block_offset, block_len
        ));
    }
    out.push_str(r#"],"leaf_blocks":["#);
    for (i, block) in leaf_term_blocks.iter().enumerate() {
        if i > 0 { out.push(','); }
        let (block_offset, block_len, entry_ranges) = &block_ranges[i];
        out.push_str(&format!(
            r#"{{"id":{},"byte_offset":{},"byte_len":{},"entries":["#,
            i, block_offset, block_len));
        for (j, e) in block.ltb_entries.iter().enumerate() {
            if j > 0 { out.push(','); }
            let (byte_offset, byte_len) = entry_ranges[j];
            out.push_str(&format!(
                r#"{{"lte_term":{},"lte_doc_freq":{},"lte_index_block_id":{},"lte_index_offset":{},"lte_index_length":{},"lte_page_skip_offset":{},"lte_continuation_block_count":{},"lte_flags":{},"byte_offset":{},"byte_len":{}}}"#,
                serde_json::to_string(&e.lte_term).unwrap_or_default(),
                e.lte_doc_freq, e.lte_index_block_id, e.lte_index_offset, e.lte_index_length,
                e.lte_page_skip_offset, e.lte_continuation_block_count, e.lte_flags,
                byte_offset, byte_len
            ));
        }
        out.push_str("]}");
    }
    out.push_str("]},");

    // DocData
    out.push_str(r#""docdata":["#);
    let mut docs: Vec<_> = store.all_doc_stats().iter().collect();
    docs.sort_by_key(|(id, _)| *id);
    for (i, (doc_id, stats)) in docs.iter().enumerate() {
        if i > 0 { out.push(','); }
        out.push_str(&format!(
            r#"{{"doc_id":"{}","importance":{:.4},"doc_len":{},"path":{}}}"#,
            doc_id, stats.importance, stats.doc_len,
            serde_json::to_string(&stats.path).unwrap_or_default()
        ));
    }
    out.push_str("],");

    // Blocks
    out.push_str(r#""blocks":["#);
    for (i, blk) in blocks.iter().enumerate() {
        if i > 0 { out.push(','); }
        let seq      = (blk.ib_header & !IB_HEADER_HAS_MORE) as u32;
        let has_more = (blk.ib_header & IB_HEADER_HAS_MORE) != 0;
        let is_cont  = blk.is_continuation();

        out.push_str(&format!(
            r#"{{"seq":{},"has_more":{},"is_continuation":{},"byte_off":{},"terms":["#,
            seq, has_more, is_cont,
            hdr.blocks_off as usize + i * PAGE_SIZE
        ));

        let terms = decode_block_terms_from_headers(
            blk,
            block_entry_map.get(&seq).map(|v| v.as_slice()).unwrap_or(&[]));
        for (j, t) in terms.iter().enumerate() {
            if j > 0 { out.push(','); }
            out.push_str(t);
        }
        out.push_str("]}");
    }
    out.push_str("]}");

    Ok(out)
}

/// Decode terms in a block using LeafTermEntry records (blocks no longer store key headers).
/// For continuation blocks: the ib_data starts with CONT_MARKER + cont_len + cont_bytes.
/// For term-start blocks: entries are raw index bytes located by LeafTermEntry offsets.
fn decode_block_terms_from_headers(
    blk:      &crate::block_table::IndexBlock,
    entries:  &[&crate::block_table::LeafTermEntry],
) -> Vec<String> {
    let data = &blk.ib_data;
    let len  = data.len();
    let mut terms = Vec::new();

    for e in entries {
        let start = e.lte_index_offset as usize;
        let end   = (start + e.lte_index_length as usize).min(len);
        if start >= len { continue; }
        let postings = decode_postings(&data[start..end]);
        let shown = postings.len().min(5);  // cap at 5 to keep JSON small

        let mut entry = format!(
            r#"{{"lte_term":{},"lte_doc_freq":{},"lte_index_length":{},"lte_index_offset":{},"lte_continuation_block_count":{},"index_entries":["#,
            serde_json::to_string(&e.lte_term).unwrap_or_default(),
            e.lte_doc_freq, e.lte_index_length, e.lte_index_offset, e.lte_continuation_block_count
        );
        for (j, (doc_id, tf)) in postings[..shown].iter().enumerate() {
            if j > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"ie_doc_id":"{}","ie_term_frequency":{}}}"#, doc_id, tf));
        }
        if postings.len() > shown {
            if shown > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"ie_doc_id":"…","ie_term_frequency":"({} more)"}}"#,
                postings.len() - shown));
        }
        entry.push_str("]}");
        terms.push(entry);
    }

    // If this is a continuation block, also show the cont section summary
    if blk.is_continuation() && len >= 4 {
        let cont_len = u16::from_le_bytes([data[2], data[3]]) as usize;
        let postings = decode_postings(&data[4..((4 + cont_len).min(len))]);
        let shown = postings.len().min(5);
        let mut entry = format!(
            r#"{{"lte_term":"[continuation]","lte_doc_freq":0,"lte_index_length":{},"lte_index_offset":4,"index_entries":["#,
            cont_len
        );
        for (j, (doc_id, tf)) in postings[..shown].iter().enumerate() {
            if j > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"ie_doc_id":"{}","ie_term_frequency":{}}}"#, doc_id, tf));
        }
        if postings.len() > shown {
            if shown > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"ie_doc_id":"…","ie_term_frequency":"({} more)"}}"#,
                postings.len() - shown));
        }
        entry.push_str("]}");
        terms.insert(0, entry);
    }

    terms
}

fn decode_postings(data: &[u8]) -> Vec<(u64, u32)> {
    let mut result = Vec::new();
    let mut pos = 0usize;
    let mut prev = 0u64;
    while pos < data.len() {
        let (delta, n) = vb_read(data, pos); pos += n;
        if pos >= data.len() { break; }
        let (tf, m) = vb_read(data, pos); pos += m;
        prev += delta;
        result.push((prev, tf as u32));
    }
    result
}

// ── search_index ─────────────────────────────────────────────────────────────

/// Load an .idx from raw bytes and run a search query.
/// Returns JSON: [{"doc_id": "...", "score": 1.23}, ...]
#[wasm_bindgen]
pub fn search_index(data: &[u8], query: &str, streams: &str) -> String {
    let mut ctx = IndexContext::new();
    if ctx.load_from_bytes(data).is_err() {
        return r#"{"error":"Failed to load index"}"#.to_string();
    }
    let results = ctx.search(query, 0, streams);
    let mut out = String::from("[");
    for (i, r) in results.iter().enumerate() {
        if i > 0 { out.push(','); }
        let path = ctx.with_store(|s| s.get_doc_path(r.doc_id).to_string());
        out.push_str(&format!(r#"{{"doc_id":"{}","score":{:.4},"path":{}}}"#,
            r.doc_id, r.score,
            serde_json::to_string(&path).unwrap_or_default()));
    }
    out.push(']');
    out
}

// ── helpers ──────────────────────────────────────────────────────────────────

struct Header {
    version: u32, num_docs: u64, num_terms: u64,
    subindex_off: u64, subindex_size: u64,
    pageskip_off: u64, pageskip_size: u64,
    docdata_off: u64, docdata_size: u64,
    blocks_off: u64, num_blocks: u64,
}

fn parse_header(d: &[u8]) -> Header {
    // v3 header layout (96B):
    // [magic:8][version:4][reserved:4][num_docs:8][num_terms:8]
    // [subindex_off:8][subindex_size:8]
    // [pageskip_off:8][pageskip_size:8]   ← NEW in v3
    // [docdata_off:8][docdata_size:8]
    // [blocks_off:8][num_blocks:8]
    Header {
        version:      u32_at(d, 8),
        num_docs:     u64_at(d, 16),
        num_terms:    u64_at(d, 24),
        subindex_off: u64_at(d, 32), subindex_size: u64_at(d, 40),
        pageskip_off: u64_at(d, 48), pageskip_size: u64_at(d, 56),
        docdata_off:  u64_at(d, 64), docdata_size:  u64_at(d, 72),
        blocks_off:   u64_at(d, 80), num_blocks:    u64_at(d, 88),
    }
}

fn u32_at(d: &[u8], o: usize) -> u32 { u32::from_le_bytes([d[o],d[o+1],d[o+2],d[o+3]]) }
fn u64_at(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes([d[o],d[o+1],d[o+2],d[o+3],d[o+4],d[o+5],d[o+6],d[o+7]])
}
fn vb_read(data: &[u8], start: usize) -> (u64, usize) {
    let mut val = 0u64; let mut shift = 0u8; let mut pos = start;
    loop {
        if pos >= data.len() { break; }
        let b = data[pos]; pos += 1;
        val |= ((b & 0x7F) as u64) << shift;
        if (b & 0x80) == 0 { break; }
        shift += 7;
    }
    (val, pos - start)
}
