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
use crate::block_table::{PAGE_SIZE, IB_HEADER_HAS_MORE};
use crate::index_context::IndexContext;

// ── parse_index ──────────────────────────────────────────────────────────────

/// Parse a .idx file from raw bytes and return a JSON description of every
/// section (Header, TermHeaderTable, DocData, Blocks with decoded term entries).
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
    let (term_directory, term_header_blocks, blocks, _pageskip) = IndexSerializer::decode(&mut store, data)
        .map_err(|e| format!("{e:?}"))?;

    // Flatten TermHeaders for display and posting-block matching
    let term_headers: Vec<&crate::block_table::TermHeader> =
        term_header_blocks.iter().flat_map(|b| b.headers.iter()).collect();

    // Build posting_block_id → [TermHeader] map once, reused per block (O(N) not O(N×M))
    let mut block_entry_map: std::collections::HashMap<u32, Vec<&crate::block_table::TermHeader>> =
        std::collections::HashMap::new();
    for e in &term_headers {
        block_entry_map.entry(e.posting_block_id).or_default().push(e);
    }

    let file_size = data.len();
    let hdr = parse_header(data);

    let mut dir_ranges: Vec<(usize, usize)> = Vec::with_capacity(term_directory.len());
    let mut block_ranges: Vec<(usize, usize, Vec<(usize, usize)>)> =
        Vec::with_capacity(term_header_blocks.len());
    let mut table_pos = hdr.subindex_off as usize + 4; // dir_count
    for d in &term_directory {
        let len = 2 + d.first_term.len() + 4;
        dir_ranges.push((table_pos, len));
        table_pos += len;
    }
    table_pos += 4; // block_count
    for block in &term_header_blocks {
        let block_start = table_pos;
        table_pos += 4; // entry_count
        let mut header_ranges = Vec::with_capacity(block.headers.len());
        for header in &block.headers {
            let len = 2 + header.term.len() + 28;
            header_ranges.push((table_pos, len));
            table_pos += len;
        }
        block_ranges.push((block_start, table_pos - block_start, header_ranges));
    }

    let mut out = String::from("{");

    // Header
    out.push_str(&format!(
        r#""header":{{"size":96,"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"subindex_off":{},"subindex_size":{},"pageskip_off":{},"pageskip_size":{},"docdata_off":{},"docdata_size":{},"blocks_off":{},"num_blocks":{},"file_size":{}}},"#,
        hdr.version, hdr.num_docs, hdr.num_terms,
        hdr.subindex_off, hdr.subindex_size,
        hdr.pageskip_off, hdr.pageskip_size,
        hdr.docdata_off, hdr.docdata_size,
        hdr.blocks_off, hdr.num_blocks, file_size
    ));

    // TermHeaderTable — show two-level structure
    out.push_str(r#""term_header_table":{"dir":["#);
    for (i, d) in term_directory.iter().enumerate() {
        if i > 0 { out.push(','); }
        let (byte_offset, byte_len) = dir_ranges[i];
        let block_id = d.term_header_block_id as usize;
        let (block_offset, block_len) = block_ranges
            .get(block_id)
            .map(|(offset, len, _)| (*offset, *len))
            .unwrap_or((0, 0));
        out.push_str(&format!(
            r#"{{"first_term":{},"term_header_block_id":{},"byte_offset":{},"byte_len":{},"header_block_offset":{},"header_block_len":{}}}"#,
            serde_json::to_string(&d.first_term).unwrap_or_default(),
            d.term_header_block_id,
            byte_offset, byte_len, block_offset, block_len
        ));
    }
    out.push_str(r#"],"blocks":["#);
    for (i, block) in term_header_blocks.iter().enumerate() {
        if i > 0 { out.push(','); }
        let (block_offset, block_len, header_ranges) = &block_ranges[i];
        out.push_str(&format!(
            r#"{{"id":{},"byte_offset":{},"byte_len":{},"headers":["#,
            i, block_offset, block_len));
        for (j, e) in block.headers.iter().enumerate() {
            if j > 0 { out.push(','); }
            let (byte_offset, byte_len) = header_ranges[j];
            out.push_str(&format!(
                r#"{{"term":{},"doc_freq":{},"posting_block_id":{},"posting_offset":{},"posting_length":{},"skip_list_offset":{},"continuation_block_count":{},"flags":{},"byte_offset":{},"byte_len":{}}}"#,
                serde_json::to_string(&e.term).unwrap_or_default(),
                e.doc_freq, e.posting_block_id, e.posting_offset, e.posting_length,
                e.skip_list_offset, e.continuation_block_count, e.flags,
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

/// Decode terms in a block using TermHeader records (blocks no longer store key headers).
/// For continuation blocks: the ib_data starts with CONT_MARKER + cont_len + cont_bytes.
/// For term-start blocks: entries are raw posting bytes located by TermHeader posting offsets.
fn decode_block_terms_from_headers(
    blk:      &crate::block_table::IndexBlock,
    entries:  &[&crate::block_table::TermHeader],
) -> Vec<String> {
    let data = &blk.ib_data;
    let len  = data.len();
    let mut terms = Vec::new();

    for e in entries {
        let start = e.posting_offset as usize;
        let end   = (start + e.posting_length as usize).min(len);
        if start >= len { continue; }
        let postings = decode_postings(&data[start..end]);
        let shown = postings.len().min(5);  // cap at 5 to keep JSON small

        let mut entry = format!(
            r#"{{"key":{},"freq":{},"data_len":{},"data_off":{},"continuation_block_count":{},"postings":["#,
            serde_json::to_string(&e.term).unwrap_or_default(),
            e.doc_freq, e.posting_length, e.posting_offset, e.continuation_block_count
        );
        for (j, (doc_id, tf)) in postings[..shown].iter().enumerate() {
            if j > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"doc_id":"{}","tf":{}}}"#, doc_id, tf));
        }
        if postings.len() > shown {
            if shown > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"doc_id":"…","tf":"({} more)"}}"#,
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
            r#"{{"key":"[continuation]","freq":0,"data_len":{},"data_off":4,"postings":["#,
            cont_len
        );
        for (j, (doc_id, tf)) in postings[..shown].iter().enumerate() {
            if j > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"doc_id":"{}","tf":{}}}"#, doc_id, tf));
        }
        if postings.len() > shown {
            if shown > 0 { entry.push(','); }
            entry.push_str(&format!(r#"{{"doc_id":"…","tf":"({} more)"}}"#,
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
    let results = ctx.search(query, 20, streams);
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
