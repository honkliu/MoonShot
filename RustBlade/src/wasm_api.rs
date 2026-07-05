use std::collections::HashMap;

use wasm_bindgen::prelude::*;

// Rust/WASM inspection surface for the C++ v19 index format. There is no native
// C++ peer file; the binary structures below mirror BlockTable.h/IndexSerializer.h.

use crate::block_table::{
    IndexBlock,
    IndexBlockContinuationHeader,
    LeafTermEntry,
    INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
    INDEX_FILE_HEADER_SIZE,
    INDEX_FORMAT_VERSION,
    PAGE_SIZE,
    DOC_REC_SIZE,
    DOC_PATH_MAX,
    DOC_PATH_OFFSET,
    DOC_PATH_PREFIX_ID_BYTES,
    DOC_PATH_PREFIX_INVALID,
};
use crate::index_context::IndexContext;
use crate::executor::IndexSearchExecutor;
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
        r#"{{"header":{{"size":{},"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"head_term_off":{},"head_term_count":{},"leaf_term_off":{},"leaf_term_count":{},"docdata_off":{},"docdata_size":{},"index_block_off":{},"index_block_count":{},"term_mphf_header_off":{},"term_mphf_header_count":{},"term_mphf_displacement_off":{},"term_mphf_displacement_count":{},"term_mphf_entry_off":{},"term_mphf_entry_page_count":{},"subindex_off":{},"subindex_size":{},"pageskip_off":0,"pageskip_size":0,"blocks_off":{},"num_blocks":{},"ib_data_off":0,"file_size":{}}},"head_leaf_term_table":{{"head":[],"leaf_blocks":[]}},"docdata":[],"blocks":[],"summary_only":true}}"#,
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
        header.term_mphf_header_off,
        header.term_mphf_header_count,
        header.term_mphf_displacement_off,
        header.term_mphf_displacement_count,
        header.term_mphf_entry_off,
        header.term_mphf_entry_page_count,
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
    let (head_entries, leaf_blocks, blocks, docdata, _, prefixes, _, _, _) = IndexSerializer::decode(&mut store, data)
        .map_err(|error| format!("{error:?}"))?;

    let mut block_entry_map: HashMap<u32, Vec<LeafTermEntry>> = HashMap::new();
    for entry in leaf_blocks.iter().flat_map(|block| block.entries()) {
        block_entry_map.entry(entry.LTE_IndexBlockID).or_default().push(entry);
    }

    let mut out = String::from("{");
    let subindex_size = header.docdata_off.saturating_sub(header.head_term_off);
    let docdata_size = header.num_docs * DOC_REC_SIZE as u64;
    out.push_str(&format!(
        r#""header":{{"size":{},"magic":"MOONSHOT","version":{},"num_docs":{},"num_terms":{},"head_term_off":{},"head_term_count":{},"leaf_term_off":{},"leaf_term_count":{},"docdata_off":{},"docdata_size":{},"index_block_off":{},"index_block_count":{},"term_mphf_header_off":{},"term_mphf_header_count":{},"term_mphf_displacement_off":{},"term_mphf_displacement_count":{},"term_mphf_entry_off":{},"term_mphf_entry_page_count":{},"subindex_off":{},"subindex_size":{},"pageskip_off":0,"pageskip_size":0,"blocks_off":{},"num_blocks":{},"ib_data_off":0,"file_size":{}}},"#,
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
        header.term_mphf_header_off,
        header.term_mphf_header_count,
        header.term_mphf_displacement_off,
        header.term_mphf_displacement_count,
        header.term_mphf_entry_off,
        header.term_mphf_entry_page_count,
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
            r#"{{"HTE_FirstTerm":{},"HTE_LeafTermBlockID":{},"byte_offset":{},"byte_len":32,"leaf_term_block_offset":{},"leaf_term_block_len":{}}}"#,
            serde_json::to_string(entry.first_term()).unwrap_or_default(),
            entry.HTE_LeafTermBlockID,
            header.head_term_off as usize + index * 32,
            header.leaf_term_off as usize + entry.HTE_LeafTermBlockID as usize * PAGE_SIZE,
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
            let entry_offset = block.LTB_Directory[entry_index] as usize;
            out.push_str(&format!(
                r#"{{"LTE_Term":{},"LTE_DocFreq":{},"LTE_IndexBlockID":{},"LTE_IndexOffset":{},"LTE_IndexLength":{},"LTE_ContinuationBlockCount":{},"LTE_Flags":{},"byte_offset":{},"byte_len":{}}}"#,
                serde_json::to_string(&entry.LTE_Term).unwrap_or_default(),
                entry.LTE_DocFreq,
                entry.LTE_IndexBlockID,
                entry.LTE_IndexOffset,
                entry.LTE_IndexLength,
                entry.LTE_ContinuationBlockCount,
                entry.LTE_Flags,
                block_offset + entry_offset,
                entry.byte_len()
            ));
        }
        out.push_str("]}");
    }
    out.push_str("]},");

    out.push_str(r#""docdata":["#);
    let mut docs: Vec<(u64, f32, u32, String)> = Vec::new();
    let first_doc_id = if header.num_docs > 0 && docdata.len() >= DOC_REC_SIZE { u32_at(&docdata, 0) as u64 } else { 0 };
    for slot in 0..header.num_docs as usize {
        let offset = slot * DOC_REC_SIZE;
        if offset + DOC_REC_SIZE > docdata.len() { break; }
        let doc_id = u32_at(&docdata, offset) as u64;
        if doc_id != first_doc_id + slot as u64 { continue; }
        let importance = u16_at(&docdata, offset + 4) as f32 / 65535.0;
        let doc_len = u32_at(&docdata, offset + 30);
        let path_len = (u16_at(&docdata, offset + 18) as usize).min(DOC_PATH_MAX);
        let path = if path_len > 0 {
            decode_doc_path(&docdata[offset + DOC_PATH_OFFSET..offset + DOC_PATH_OFFSET + path_len], &prefixes)
        } else {
            String::new()
        };
        docs.push((doc_id, importance, doc_len, path));
    }
    docs.sort_by_key(|(doc_id, _, _, _)| *doc_id);
    for (index, (doc_id, importance, doc_len, path)) in docs.iter().enumerate() {
        if index > 0 { out.push(','); }
        out.push_str(&format!(
            r#"{{"doc_id":"{}","importance":{:.4},"doc_len":{},"path":{}}}"#,
            doc_id,
            importance,
            doc_len,
            serde_json::to_string(path).unwrap_or_default()
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

    if let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.IB_Data) {
        let len = header.IBCH_DataLength as usize;
        if len > 0 && INDEX_BLOCK_CONTINUATION_HEADER_SIZE + len <= PAGE_SIZE {
            let postings = decode_postings(&block.IB_Data[INDEX_BLOCK_CONTINUATION_HEADER_SIZE..INDEX_BLOCK_CONTINUATION_HEADER_SIZE + len]);
            terms.push(format_postings("[continuation]", 0, INDEX_BLOCK_CONTINUATION_HEADER_SIZE, len, 0, postings));
        }
    }

    for entry in entries {
        let start = entry.LTE_IndexOffset as usize;
        let len = entry.LTE_IndexLength as usize;
        if start + len > PAGE_SIZE { continue; }
        let postings = decode_postings(&block.IB_Data[start..start + len]);
            terms.push(format_postings(
                &entry.LTE_Term,
                entry.LTE_DocFreq,
                start,
                len,
                entry.LTE_ContinuationBlockCount as u32,
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
        r#"{{"LTE_Term":{},"LTE_DocFreq":{},"LTE_IndexLength":{},"LTE_IndexOffset":{},"LTE_ContinuationBlockCount":{},"index_entries":["#,
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
        let tf = data[pos] as u32;
        pos += 1;
        result.push((doc_id, tf));
    }
    result
}

#[wasm_bindgen]
pub fn search_index(data: &[u8], query: &str, streams: &str) -> String {
    let mut context = IndexContext::new();
    if let Err(error) = context.LoadFromBytes(data) {
        return format!(r#"{{"error":"Failed to load index: {error:?}"}}"#);
    }
    let tree = context.Compile(query, streams);
    let mut reader = context.GetReader(tree);
    let results = {
        let store = context.GetStore();
        let store = store.read().unwrap();
        let executor = IndexSearchExecutor::new(&store);
        executor.Execute(reader.as_mut(), 0)
    };
    let mut out = String::from("[");
    for (index, result) in results.iter().enumerate() {
        if index > 0 { out.push(','); }
        let path = context.GetDocPath(result.doc_id);
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

#[wasm_bindgen]
pub fn vector_search_index(data: &[u8], query_vector_json: &str, top_k: usize, ef_search: usize) -> String {
    let mut context = IndexContext::new();
    if let Err(error) = context.LoadFromBytes(data) {
        return format!(r#"{{"error":"Failed to load index: {error:?}"}}"#);
    }
    let query_vector: Vec<f32> = match serde_json::from_str(query_vector_json) {
        Ok(vector) => vector,
        Err(error) => return format!(r#"{{"error":"Invalid query vector: {error}"}}"#),
    };
    let results = context.VectorSearch(&query_vector, top_k.max(1), ef_search.max(top_k.max(1)));
    let mut out = String::from("[");
    for (index, result) in results.iter().enumerate() {
        if index > 0 { out.push(','); }
        let path = context.GetDocPath(result.doc_id);
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
    term_mphf_header_off: u64,
    term_mphf_header_count: u64,
    term_mphf_displacement_off: u64,
    term_mphf_displacement_count: u64,
    term_mphf_entry_off: u64,
    term_mphf_entry_page_count: u64,
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
        term_mphf_header_off: u64_at(data, 88),
        term_mphf_header_count: u64_at(data, 96),
        term_mphf_displacement_off: u64_at(data, 104),
        term_mphf_displacement_count: u64_at(data, 112),
        term_mphf_entry_off: u64_at(data, 120),
        term_mphf_entry_page_count: u64_at(data, 128),
    })
}

fn u32_at(data: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}
fn u16_at(data: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(data[offset..offset + 2].try_into().unwrap())
}
fn u64_at(data: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap())
}

fn decode_doc_path(payload: &[u8], prefixes: &[String]) -> String {
    if payload.is_empty() { return String::new(); }
    if payload.len() < DOC_PATH_PREFIX_ID_BYTES {
        return std::str::from_utf8(payload).unwrap_or("").to_string();
    }
    let prefix_id = u16::from_le_bytes(payload[0..2].try_into().unwrap());
    let filename = std::str::from_utf8(&payload[2..]).unwrap_or("");
    if prefix_id == DOC_PATH_PREFIX_INVALID || prefix_id as usize >= prefixes.len() {
        return filename.to_string();
    }
    let prefix = &prefixes[prefix_id as usize];
    if prefix.is_empty() { return filename.to_string(); }
    let separator = if prefix.contains('\\') { '\\' } else { '/' };
    if prefix.ends_with(['/', '\\']) { format!("{prefix}{filename}") }
    else { format!("{prefix}{separator}{filename}") }
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
