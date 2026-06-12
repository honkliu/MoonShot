/*
 * wasm_api.rs — wasm-bindgen exports for the MoonShot index viewer.
 *
 * Exposed to JavaScript:
 *   parse_index(bytes: Uint8Array) -> String (JSON)
 *   search_index(bytes: Uint8Array, query: String, streams: String) -> String (JSON)
 */

use wasm_bindgen::prelude::*;
use crate::serializer::IndexSerializer;
use crate::posting_store::PostingStore;
use crate::block_table::{PAGE_SIZE, IB_HEADER_HAS_MORE, CONT_MARKER};
use crate::index_context::IndexContext;

// ── parse_index ──────────────────────────────────────────────────────────────

/// Parse a .idx file from raw bytes and return a JSON description of every
/// section (Header, SubIndex, DocData, Blocks with decoded term entries).
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

    let mut store = PostingStore::new();
    let (subindex, blocks) = IndexSerializer::decode(&mut store, data)
        .map_err(|e| format!("{e:?}"))?;

    let file_size = data.len();
    let hdr = parse_header(data);

    let mut out = String::from("{");

    // Header
    out.push_str(&format!(
        r#""header":{{"size":{},"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"subindex_off":{},"subindex_size":{},"docdata_off":{},"docdata_size":{},"blocks_off":{},"num_blocks":{},"file_size":{}}},"#,
        80, hdr.version, hdr.num_docs, hdr.num_terms,
        hdr.subindex_off, hdr.subindex_size,
        hdr.docdata_off, hdr.docdata_size,
        hdr.blocks_off, hdr.num_blocks, file_size
    ));

    // SubIndex
    out.push_str(r#""subindex":["#);
    for (i, e) in subindex.iter().enumerate() {
        if i > 0 { out.push(','); }
        out.push_str(&format!(
            r#"{{"term":{},"block_seq":{}}}"#,
            serde_json::to_string(&e.term).unwrap_or_default(),
            e.block_seq
        ));
    }
    out.push_str("],");

    // DocData
    out.push_str(r#""docdata":["#);
    let mut docs: Vec<_> = store.all_doc_stats().iter().collect();
    docs.sort_by_key(|(id, _)| *id);
    for (i, (doc_id, stats)) in docs.iter().enumerate() {
        if i > 0 { out.push(','); }
        out.push_str(&format!(
            r#"{{"doc_id":"{}","importance":{:.4},"doc_len":{}}}"#,
            doc_id, stats.importance, stats.doc_len
        ));
    }
    out.push_str("],");

    // Blocks
    out.push_str(r#""blocks":["#);
    for (i, blk) in blocks.iter().enumerate() {
        if i > 0 { out.push(','); }
        let seq      = (blk.ib_header & !IB_HEADER_HAS_MORE) as u32;
        let has_more = (blk.ib_header & IB_HEADER_HAS_MORE) != 0;
        let is_cont  = blk.ib_data.len() >= 2
            && u16::from_le_bytes([blk.ib_data[0], blk.ib_data[1]]) == CONT_MARKER;

        out.push_str(&format!(
            r#"{{"seq":{},"has_more":{},"is_continuation":{},"byte_off":{},"terms":["#,
            seq, has_more, is_cont,
            hdr.blocks_off as usize + i * PAGE_SIZE
        ));

        if !is_cont {
            let terms = decode_block_terms(blk, &store, &docs);
            for (j, t) in terms.iter().enumerate() {
                if j > 0 { out.push(','); }
                out.push_str(t);
            }
        }
        out.push_str("]}");
    }
    out.push_str("]}");

    Ok(out)
}

fn decode_block_terms(
    blk:   &crate::block_table::IndexBlock,
    _store: &PostingStore,
    _docs: &[(&u64, &crate::posting_store::DocStats)],
) -> Vec<String> {
    let data = &blk.ib_data;
    let len  = data.len();
    let mut terms = Vec::new();
    let mut ptr = 0usize;

    while ptr + 2 <= len {
        let key_len = u16::from_le_bytes([data[ptr], data[ptr+1]]) as usize;
        ptr += 2;
        if key_len == 0 || key_len == CONT_MARKER as usize { break; }
        if ptr + key_len + 8 > len { break; }

        let key = match std::str::from_utf8(&data[ptr..ptr+key_len]) {
            Ok(s) => s.to_string(), Err(_) => { ptr += key_len + 8; continue; }
        };
        ptr += key_len;
        let doc_freq = u32::from_le_bytes([data[ptr],data[ptr+1],data[ptr+2],data[ptr+3]]);
        let data_len = u32::from_le_bytes([data[ptr+4],data[ptr+5],data[ptr+6],data[ptr+7]]) as usize;
        ptr += 8;

        let dend = (ptr + data_len).min(len);
        let postings = decode_postings(&data[ptr..dend]);

        let mut entry = format!(
            r#"{{"key":{},"freq":{},"data_len":{},"data_off":{},"postings":["#,
            serde_json::to_string(&key).unwrap_or_default(),
            doc_freq, data_len, ptr
        );
        for (j, (doc_id, tf)) in postings.iter().enumerate() {
            if j > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"doc_id":"{}","tf":{}}}"#, doc_id, tf));
        }
        entry.push_str("]}");
        terms.push(entry);

        if ptr + data_len > len { break; }
        ptr += data_len;
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
    let results = ctx.search(query, 20, streams);
    let mut out = String::from("[");
    for (i, r) in results.iter().enumerate() {
        if i > 0 { out.push(','); }
        out.push_str(&format!(r#"{{"doc_id":"{}","score":{:.4}}}"#, r.doc_id, r.score));
    }
    out.push(']');
    out
}

// ── helpers ──────────────────────────────────────────────────────────────────

struct Header {
    version: u32, num_docs: u64, num_terms: u64,
    subindex_off: u64, subindex_size: u64,
    docdata_off: u64, docdata_size: u64,
    blocks_off: u64, num_blocks: u64,
}

fn parse_header(d: &[u8]) -> Header {
    Header {
        version:      u32_at(d, 8),
        num_docs:     u64_at(d, 16),
        num_terms:    u64_at(d, 24),
        subindex_off: u64_at(d, 32), subindex_size: u64_at(d, 40),
        docdata_off:  u64_at(d, 48), docdata_size:  u64_at(d, 56),
        blocks_off:   u64_at(d, 64), num_blocks:    u64_at(d, 72),
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
