use std::collections::HashMap;

use wasm_bindgen::prelude::*;

use crate::block_table::{
    IndexBlock,
    IndexBlockContinuationHeader,
    LeafTermEntry,
    INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
    INDEX_FILE_HEADER_SIZE,
    INDEX_FORMAT_VERSION,
    PAGE_SIZE,
    DOC_REC_SIZE,
};
use crate::index_context::IndexContext;
use crate::posting_store::PostingStore;
use crate::serializer::IndexSerializer;

#[wasm_bindgen]
pub fn parse_index_summary(data: &[u8], file_size: usize) -> String {
    match parse_index_summary_inner(data, file_size) {
        Ok(value) => value,
        Err(error) => format!(r#"{{"error":"{}"}}"#, error),
    }
}

fn parse_index_summary_inner(data: &[u8], file_size: usize) -> Result<String, String> {
    let header = parse_header(data)?;
    let subindex_size = header.docdata_off.saturating_sub(header.head_term_off);
    let docdata_size = header.num_docs * DOC_REC_SIZE as u64;
    Ok(format!(
        r#"{{"header":{{"size":{},"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"head_term_off":{},"head_term_count":{},"leaf_term_off":{},"leaf_term_count":{},"docdata_off":{},"docdata_size":{},"index_block_off":{},"index_block_count":{},"subindex_off":{},"subindex_size":{},"pageskip_off":0,"pageskip_size":0,"blocks_off":{},"num_blocks":{},"ib_data_off":0,"file_size":{}}},"head_leaf_term_table":{{"head":[],"leaf_blocks":[]}},"docdata":[],"blocks":[],"summary_only":true}}"#,
        INDEX_FILE_HEADER_SIZE,
        header.version,
        header.num_docs,
        header.num_terms,
        header.head_term_off,
        header.head_term_count,
        header.leaf_term_off,
        header.leaf_term_count,
        header.docdata_off,
        docdata_size,
        header.index_block_off,
        header.index_block_count,
        header.head_term_off,
        subindex_size,
        header.index_block_off,
        header.index_block_count,
        file_size
    ))
}

#[wasm_bindgen]
pub fn parse_index(data: &[u8]) -> String {
    match parse_index_inner(data) {
        Ok(value) => value,
        Err(error) => format!(r#"{{"error":"{}"}}"#, error),
    }
}

fn parse_index_inner(data: &[u8]) -> Result<String, String> {
    let header = parse_header(data)?;
    let mut store = PostingStore::new();
    let (head_entries, leaf_blocks, blocks) = IndexSerializer::decode(&mut store, data)
        .map_err(|error| format!("{error:?}"))?;

    let mut block_entry_map: HashMap<u32, Vec<LeafTermEntry>> = HashMap::new();
    for entry in leaf_blocks.iter().flat_map(|block| block.entries()) {
        block_entry_map.entry(entry.lte_index_block_id).or_default().push(entry);
    }

    let mut out = String::from("{");
    let subindex_size = header.docdata_off.saturating_sub(header.head_term_off);
    let docdata_size = header.num_docs * DOC_REC_SIZE as u64;
    out.push_str(&format!(
        r#""header":{{"size":{},"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"head_term_off":{},"head_term_count":{},"leaf_term_off":{},"leaf_term_count":{},"docdata_off":{},"docdata_size":{},"index_block_off":{},"index_block_count":{},"subindex_off":{},"subindex_size":{},"pageskip_off":0,"pageskip_size":0,"blocks_off":{},"num_blocks":{},"ib_data_off":0,"file_size":{}}},"#,
        INDEX_FILE_HEADER_SIZE,
        header.version,
        header.num_docs,
        header.num_terms,
        header.head_term_off,
        header.head_term_count,
        header.leaf_term_off,
        header.leaf_term_count,
        header.docdata_off,
        docdata_size,
        header.index_block_off,
        header.index_block_count,
        header.head_term_off,
        subindex_size,
        header.index_block_off,
        header.index_block_count,
        data.len()
    ));

    out.push_str(r#""head_leaf_term_table":{"head":["#);
    for (index, entry) in head_entries.iter().enumerate() {
        if index > 0 { out.push(','); }
        out.push_str(&format!(
            r#"{{"hte_first_term":{},"hte_leaf_term_block_id":{},"byte_offset":{},"byte_len":32,"leaf_term_block_offset":{},"leaf_term_block_len":{}}}"#,
            serde_json::to_string(entry.first_term()).unwrap_or_default(),
            entry.hte_leaf_term_block_id,
            header.head_term_off as usize + index * 32,
            header.leaf_term_off as usize + entry.hte_leaf_term_block_id as usize * PAGE_SIZE,
            PAGE_SIZE
        ));
    }

    out.push_str(r#"],"leaf_blocks":["#);
    for (block_index, block) in leaf_blocks.iter().enumerate() {
        if block_index > 0 { out.push(','); }
        let block_offset = header.leaf_term_off as usize + block_index * PAGE_SIZE;
        out.push_str(&format!(
            r#"{{"id":{},"byte_offset":{},"byte_len":{},"entry_count":{},"entries":["#,
            block_index,
            block_offset,
            PAGE_SIZE,
            block.entry_count()
        ));
        for entry_index in 0..block.entry_count() {
            if entry_index > 0 { out.push(','); }
            let Some(entry) = block.entry(entry_index) else { continue; };
            let entry_offset = block.ltb_directory[entry_index] as usize;
            out.push_str(&format!(
                r#"{{"lte_term":{},"lte_doc_freq":{},"lte_index_block_id":{},"lte_index_offset":{},"lte_index_length":{},"lte_continuation_block_count":{},"lte_flags":{},"byte_offset":{},"byte_len":{}}}"#,
                serde_json::to_string(&entry.lte_term).unwrap_or_default(),
                entry.lte_doc_freq,
                entry.lte_index_block_id,
                entry.lte_index_offset,
                entry.lte_index_length,
                entry.lte_continuation_block_count,
                entry.lte_flags,
                block_offset + entry_offset,
                entry.byte_len()
            ));
        }
        out.push_str("]}");
    }
    out.push_str("]},");

    out.push_str(r#""docdata":["#);
    let mut docs: Vec<_> = store.all_doc_stats().iter().collect();
    docs.sort_by_key(|(doc_id, _)| *doc_id);
    for (index, (doc_id, stats)) in docs.iter().enumerate() {
        if index > 0 { out.push(','); }
        out.push_str(&format!(
            r#"{{"doc_id":"{}","importance":{:.4},"doc_len":{},"path":{}}}"#,
            doc_id,
            stats.importance,
            stats.doc_len,
            serde_json::to_string(&stats.path).unwrap_or_default()
        ));
    }
    out.push_str("],");

    out.push_str(r#""blocks":["#);
    for (seq, block) in blocks.iter().enumerate() {
        if seq > 0 { out.push(','); }
        out.push_str(&format!(
            r#"{{"seq":{},"byte_off":{},"terms":["#,
            seq,
            header.index_block_off as usize + seq * PAGE_SIZE
        ));

        let terms = decode_block_terms(
            block,
            block_entry_map.get(&(seq as u32)).map(|entries| entries.as_slice()).unwrap_or(&[]));
        for (index, term) in terms.iter().enumerate() {
            if index > 0 { out.push(','); }
            out.push_str(term);
        }
        out.push_str("]}");
    }
    out.push_str("]}");

    Ok(out)
}

fn decode_block_terms(block: &IndexBlock, entries: &[LeafTermEntry]) -> Vec<String> {
    let mut terms = Vec::new();

    if let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.ib_data) {
        let len = header.ibch_data_length as usize;
        if len > 0 && INDEX_BLOCK_CONTINUATION_HEADER_SIZE + len <= PAGE_SIZE {
            let postings = decode_postings(&block.ib_data[INDEX_BLOCK_CONTINUATION_HEADER_SIZE..INDEX_BLOCK_CONTINUATION_HEADER_SIZE + len]);
            terms.push(format_postings("[continuation]", 0, INDEX_BLOCK_CONTINUATION_HEADER_SIZE, len, 0, postings));
        }
    }

    for entry in entries {
        let start = entry.lte_index_offset as usize;
        let len = entry.lte_index_length as usize;
        if start + len > PAGE_SIZE { continue; }
        let postings = decode_postings(&block.ib_data[start..start + len]);
        terms.push(format_postings(
            &entry.lte_term,
            entry.lte_doc_freq,
            start,
            len,
            entry.lte_continuation_block_count,
            postings));
    }

    terms
}

fn format_postings(term: &str,
                   doc_freq: u32,
                   offset: usize,
                   len: usize,
                   continuation_count: u32,
                   postings: Vec<(u64, u32)>) -> String
{
    let shown = postings.len().min(5);
    let mut out = format!(
        r#"{{"lte_term":{},"lte_doc_freq":{},"lte_index_length":{},"lte_index_offset":{},"lte_continuation_block_count":{},"index_entries":["#,
        serde_json::to_string(term).unwrap_or_default(),
        doc_freq,
        len,
        offset,
        continuation_count
    );
    for (index, (doc_id, tf)) in postings[..shown].iter().enumerate() {
        if index > 0 { out.push(','); }
        out.push_str(&format!(r#"{{"ie_doc_id":"{}","ie_term_frequency":{}}}"#, doc_id, tf));
    }
    if postings.len() > shown {
        if shown > 0 { out.push(','); }
        out.push_str(&format!(r#"{{"ie_doc_id":"...","ie_term_frequency":"({} more)"}}"#, postings.len() - shown));
    }
    out.push_str("]}");
    out
}

fn decode_postings(data: &[u8]) -> Vec<(u64, u32)> {
    let mut result = Vec::new();
    let mut pos = 0usize;
    while pos < data.len() {
        let (doc_id, doc_bytes) = vb_read(data, pos);
        pos += doc_bytes;
        if pos >= data.len() { break; }
        let (tf, tf_bytes) = vb_read(data, pos);
        pos += tf_bytes;
        result.push((doc_id, tf as u32));
    }
    result
}

#[wasm_bindgen]
pub fn search_index(data: &[u8], query: &str, streams: &str) -> String {
    let mut context = IndexContext::new();
    if context.load_from_bytes(data).is_err() {
        return r#"{"error":"Failed to load index"}"#.to_string();
    }
    let results = context.search(query, 0, streams);
    let mut out = String::from("[");
    for (index, result) in results.iter().enumerate() {
        if index > 0 { out.push(','); }
        let path = context.with_store(|store| store.get_doc_path(result.doc_id).to_string());
        out.push_str(&format!(
            r#"{{"doc_id":"{}","score":{:.4},"path":{}}}"#,
            result.doc_id,
            result.score,
            serde_json::to_string(&path).unwrap_or_default()
        ));
    }
    out.push(']');
    out
}

struct Header {
    version: u32,
    num_docs: u64,
    num_terms: u64,
    head_term_off: u64,
    head_term_count: u64,
    leaf_term_off: u64,
    leaf_term_count: u64,
    docdata_off: u64,
    index_block_off: u64,
    index_block_count: u64,
}

fn parse_header(data: &[u8]) -> Result<Header, String> {
    if data.len() < INDEX_FILE_HEADER_SIZE {
        return Err(format!("Index header is shorter than {} bytes", INDEX_FILE_HEADER_SIZE));
    }
    if &data[0..8] != b"MOONSHOT" {
        return Err("Not a valid MOONSHOT index (bad magic)".to_string());
    }
    let version = u32_at(data, 8);
    if version != INDEX_FORMAT_VERSION {
        return Err(format!("Unsupported index version {}; expected {}", version, INDEX_FORMAT_VERSION));
    }
    Ok(Header {
        version,
        num_docs: u64_at(data, 16),
        num_terms: u64_at(data, 24),
        head_term_off: u64_at(data, 32),
        head_term_count: u64_at(data, 40),
        leaf_term_off: u64_at(data, 48),
        leaf_term_count: u64_at(data, 56),
        docdata_off: u64_at(data, 64),
        index_block_off: u64_at(data, 72),
        index_block_count: u64_at(data, 80),
    })
}

fn u32_at(data: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}
fn u64_at(data: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap())
}
fn vb_read(data: &[u8], start: usize) -> (u64, usize) {
    let mut value = 0u64;
    let mut shift = 0u8;
    let mut pos = start;
    loop {
        if pos >= data.len() { break; }
        let byte = data[pos];
        pos += 1;
        value |= ((byte & 0x7F) as u64) << shift;
        if byte & 0x80 == 0 { break; }
        shift += 7;
    }
    (value, pos - start)
}
