use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Write};

use crate::block_table::{
    HeadTermEntry,
    IndexBlock,
    IndexBlockContinuationHeader,
    LeafTermBlock,
    LeafTermEntry,
    DOC_PATH_MAX,
    DOC_REC_SIZE,
    HEAD_TERM_KEY_MAX,
    INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
    INDEX_FILE_HEADER_SIZE,
    INDEX_FORMAT_VERSION,
    LEAF_TERM_DATA_OFFSET,
    LEAF_TERM_DIRECTORY_COUNT,
    PAGE_SIZE,
};
use crate::error::{Result, RustBladeError};
use crate::posting_store::PostingStore;

const MAGIC: &[u8; 8] = b"MOONSHOT";

pub struct BlocksResult {
    pub bbr_index_blocks: Vec<IndexBlock>,
    pub bbr_head_term_entries: Vec<HeadTermEntry>,
    pub bbr_leaf_term_blocks: Vec<LeafTermBlock>,
    pub bbr_total_terms: u64,
}

pub struct IndexSerializer;

impl IndexSerializer {
    pub fn save(store: &mut PostingStore, path: &str) -> Result<()> {
        let bytes = Self::encode(store)?;
        let file = File::create(path)?;
        let mut writer = BufWriter::new(file);
        writer.write_all(&bytes)?;
        writer.flush()?;
        Ok(())
    }

    pub fn encode(store: &mut PostingStore) -> Result<Vec<u8>> {
        let blocks = Self::build_blocks(store);
        let (docdata, document_count) = Self::encode_docdata(store);

        let head_offset = INDEX_FILE_HEADER_SIZE;
        let leaf_offset = head_offset + blocks.bbr_head_term_entries.len() * 32;
        let docdata_offset = leaf_offset + blocks.bbr_leaf_term_blocks.len() * PAGE_SIZE;
        let raw_index_offset = docdata_offset + docdata.len();
        let index_offset = page_aligned_bytes(raw_index_offset);
        let total = index_offset + blocks.bbr_index_blocks.len() * PAGE_SIZE;

        let mut out = vec![0u8; total];
        out[0..8].copy_from_slice(MAGIC);
        write_u32(&mut out, 8, INDEX_FORMAT_VERSION);
        write_f32(&mut out, 12, store.avg_doc_len());
        write_u64(&mut out, 16, document_count);
        write_u64(&mut out, 24, blocks.bbr_total_terms);
        write_u64(&mut out, 32, head_offset as u64);
        write_u64(&mut out, 40, blocks.bbr_head_term_entries.len() as u64);
        write_u64(&mut out, 48, leaf_offset as u64);
        write_u64(&mut out, 56, blocks.bbr_leaf_term_blocks.len() as u64);
        write_u64(&mut out, 64, docdata_offset as u64);
        write_u64(&mut out, 72, index_offset as u64);
        write_u64(&mut out, 80, blocks.bbr_index_blocks.len() as u64);

        let mut cursor = head_offset;
        for entry in &blocks.bbr_head_term_entries {
            out[cursor..cursor + 32].copy_from_slice(&entry.to_bytes());
            cursor += 32;
        }

        cursor = leaf_offset;
        for block in &blocks.bbr_leaf_term_blocks {
            out[cursor..cursor + PAGE_SIZE].copy_from_slice(&block.to_bytes());
            cursor += PAGE_SIZE;
        }

        out[docdata_offset..docdata_offset + docdata.len()].copy_from_slice(&docdata);

        cursor = index_offset;
        for block in &blocks.bbr_index_blocks {
            out[cursor..cursor + PAGE_SIZE].copy_from_slice(&block.ib_data);
            cursor += PAGE_SIZE;
        }

        Ok(out)
    }

    pub fn load(store: &mut PostingStore, path: &str)
        -> Result<(Vec<HeadTermEntry>, Vec<LeafTermBlock>, Vec<IndexBlock>)>
    {
        let file = File::open(path)?;
        let mut reader = BufReader::new(file);
        let mut bytes = Vec::new();
        reader.read_to_end(&mut bytes)?;
        Self::decode(store, &bytes)
    }

    pub fn decode(store: &mut PostingStore, data: &[u8])
        -> Result<(Vec<HeadTermEntry>, Vec<LeafTermBlock>, Vec<IndexBlock>)>
    {
        if data.len() < INDEX_FILE_HEADER_SIZE { return Err(RustBladeError::InvalidFormat); }
        if &data[0..8] != MAGIC { return Err(RustBladeError::InvalidFormat); }
        if u32_at(data, 8) != INDEX_FORMAT_VERSION { return Err(RustBladeError::InvalidFormat); }

        let head_offset = u64_at(data, 32) as usize;
        let head_count = u64_at(data, 40) as usize;
        let leaf_offset = u64_at(data, 48) as usize;
        let leaf_count = u64_at(data, 56) as usize;
        let docdata_offset = u64_at(data, 64) as usize;
        let index_offset = u64_at(data, 72) as usize;
        let index_count = u64_at(data, 80) as usize;
        let num_docs = u64_at(data, 16) as usize;

        let mut head = Vec::with_capacity(head_count);
        for index in 0..head_count {
            let offset = head_offset + index * 32;
            if offset + 32 > data.len() { return Err(RustBladeError::InvalidFormat); }
            head.push(HeadTermEntry::from_bytes(&data[offset..offset + 32]).ok_or(RustBladeError::InvalidFormat)?);
        }

        let mut leaf_blocks = Vec::with_capacity(leaf_count);
        for index in 0..leaf_count {
            let offset = leaf_offset + index * PAGE_SIZE;
            if offset + PAGE_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            leaf_blocks.push(LeafTermBlock::from_bytes(&data[offset..offset + PAGE_SIZE]).ok_or(RustBladeError::InvalidFormat)?);
        }

        for index in 0..num_docs {
            let offset = docdata_offset + index * DOC_REC_SIZE;
            if offset + DOC_REC_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            let doc_id = u64_at(data, offset);
            if doc_id != index as u64 { continue; }
            let doc_len = u32_at(data, offset + 32);
            let importance = f32_at(data, offset + 44);
            let path_len = u16_at(data, offset + 72) as usize;
            store.add_doc_tokens(doc_id, doc_len);
            store.set_doc_importance(doc_id, importance);
            if path_len > 0 && path_len <= DOC_PATH_MAX {
                let path_offset = offset + 768;
                if let Ok(path) = std::str::from_utf8(&data[path_offset..path_offset + path_len]) {
                    store.set_doc_path(doc_id, path.to_string());
                }
            }
        }

        let mut index_blocks = Vec::with_capacity(index_count);
        for index in 0..index_count {
            let offset = index_offset + index * PAGE_SIZE;
            if offset + PAGE_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            let mut block = IndexBlock::default();
            block.ib_data.copy_from_slice(&data[offset..offset + PAGE_SIZE]);
            index_blocks.push(block);
        }

        Ok((head, leaf_blocks, index_blocks))
    }

    pub fn is_valid_index(path: &str) -> bool {
        let Ok(mut file) = File::open(path) else { return false; };
        let mut header = [0u8; 12];
        file.read_exact(&mut header).is_ok()
            && &header[0..8] == MAGIC
            && u32::from_le_bytes(header[8..12].try_into().unwrap()) == INDEX_FORMAT_VERSION
    }

    pub fn is_valid_bytes(data: &[u8]) -> bool {
        data.len() >= 12 && &data[0..8] == MAGIC && u32_at(data, 8) == INDEX_FORMAT_VERSION
    }

    pub fn build_blocks_pub(store: &PostingStore)
        -> (Vec<IndexBlock>, Vec<HeadTermEntry>, Vec<LeafTermBlock>)
    {
        let blocks = Self::build_blocks(store);
        (blocks.bbr_index_blocks, blocks.bbr_head_term_entries, blocks.bbr_leaf_term_blocks)
    }

    fn build_blocks(store: &PostingStore) -> BlocksResult {
        let mut terms: Vec<(&String, &crate::posting_store::PostingList)> = store.all_postings().iter().collect();
        terms.sort_by_key(|(term, _)| term.as_str());

        let mut index_blocks = Vec::new();
        let mut cur = IndexBlock::default();
        let mut wptr = 0usize;
        let mut seq = 0u32;

        let mut leaf_blocks = Vec::new();
        let mut leaf_block = LeafTermBlock::default();
        let mut leaf_write_offset = 0usize;
        let mut leaf_entry_count = 0usize;
        let mut first_leaf_term = String::new();

        let flush_index_block = |index_blocks: &mut Vec<IndexBlock>, cur: &mut IndexBlock, wptr: &mut usize, seq: &mut u32| {
            index_blocks.push(cur.clone());
            *seq += 1;
            *cur = IndexBlock::default();
            *wptr = 0;
        };

        let flush_leaf_block = |leaf_blocks: &mut Vec<LeafTermBlock>,
                                head: &mut Vec<HeadTermEntry>,
                                leaf_block: &mut LeafTermBlock,
                                leaf_write_offset: &mut usize,
                                leaf_entry_count: &mut usize,
                                first_leaf_term: &mut String| {
            if *leaf_entry_count == 0 { return; }
            leaf_block.ltb_directory[LEAF_TERM_DIRECTORY_COUNT - 1] = *leaf_entry_count as u16;
            head.push(HeadTermEntry::new(first_leaf_term, leaf_blocks.len() as u32));
            leaf_blocks.push(leaf_block.clone());
            *leaf_block = LeafTermBlock::default();
            *leaf_write_offset = 0;
            *leaf_entry_count = 0;
            first_leaf_term.clear();
        };

        let mut head_entries = Vec::new();
        let mut total_terms = 0u64;

        for (term, posting_list) in terms {
            if term.len() > HEAD_TERM_KEY_MAX { continue; }
            let bytes = posting_list.get_bytes_ref();
            if bytes.is_empty() { continue; }

            if wptr >= PAGE_SIZE {
                flush_index_block(&mut index_blocks, &mut cur, &mut wptr, &mut seq);
            }

            let mut src = 0usize;
            let mut remaining = bytes.len();
            let mut data_offset = wptr;
            let mut data_here = posting_prefix_bytes(&bytes[src..], PAGE_SIZE - wptr);
            if data_here == 0 {
                flush_index_block(&mut index_blocks, &mut cur, &mut wptr, &mut seq);
                data_offset = wptr;
                data_here = posting_prefix_bytes(&bytes[src..], PAGE_SIZE);
                if data_here == 0 { continue; }
            }

            let index_block_id = seq;
            cur.ib_data[wptr..wptr + data_here].copy_from_slice(&bytes[src..src + data_here]);
            wptr += data_here;
            src += data_here;
            remaining -= data_here;
            let mut continuation_block_count = 0u32;

            if remaining > 0 {
                flush_index_block(&mut index_blocks, &mut cur, &mut wptr, &mut seq);

                while remaining > 0 {
                    let cont_cap = PAGE_SIZE - INDEX_BLOCK_CONTINUATION_HEADER_SIZE;
                    let cont_here = posting_prefix_bytes(&bytes[src..], cont_cap);
                    if cont_here == 0 { break; }
                    let more_cont = cont_here < remaining;
                    let cont_max_doc_id = max_doc_id_in_pairs(&bytes[src..src + cont_here]);
                    IndexBlockContinuationHeader {
                        ibch_max_doc_id: cont_max_doc_id,
                        ibch_data_length: cont_here as u32,
                    }.write_to(&mut cur.ib_data[wptr..wptr + INDEX_BLOCK_CONTINUATION_HEADER_SIZE]);
                    wptr += INDEX_BLOCK_CONTINUATION_HEADER_SIZE;
                    cur.ib_data[wptr..wptr + cont_here].copy_from_slice(&bytes[src..src + cont_here]);
                    wptr += cont_here;
                    src += cont_here;
                    remaining -= cont_here;
                    continuation_block_count += 1;
                    if more_cont {
                        flush_index_block(&mut index_blocks, &mut cur, &mut wptr, &mut seq);
                    }
                }
            }

            let leaf_entry = LeafTermEntry {
                lte_term: term.clone(),
                lte_doc_freq: posting_list.doc_freq(),
                lte_index_block_id: index_block_id,
                lte_index_offset: data_offset as u32,
                lte_index_length: data_here as u32,
                lte_continuation_block_count: continuation_block_count,
                lte_flags: 0,
            };
            let entry_bytes = leaf_entry.byte_len();
            if leaf_entry_count > 0
                && (leaf_entry_count >= LEAF_TERM_DIRECTORY_COUNT - 1
                    || leaf_write_offset + entry_bytes > PAGE_SIZE - LEAF_TERM_DATA_OFFSET)
            {
                flush_leaf_block(&mut leaf_blocks, &mut head_entries, &mut leaf_block, &mut leaf_write_offset, &mut leaf_entry_count, &mut first_leaf_term);
            }
            if leaf_entry_count == 0 { first_leaf_term = term.clone(); }
            write_leaf_entry(&mut leaf_block, leaf_entry_count, leaf_write_offset, &leaf_entry);
            leaf_write_offset += entry_bytes;
            leaf_entry_count += 1;
            total_terms += 1;
        }

        if wptr > 0 { flush_index_block(&mut index_blocks, &mut cur, &mut wptr, &mut seq); }
        flush_leaf_block(&mut leaf_blocks, &mut head_entries, &mut leaf_block, &mut leaf_write_offset, &mut leaf_entry_count, &mut first_leaf_term);

        BlocksResult {
            bbr_index_blocks: index_blocks,
            bbr_head_term_entries: head_entries,
            bbr_leaf_term_blocks: leaf_blocks,
            bbr_total_terms: total_terms,
        }
    }

    fn encode_docdata(store: &PostingStore) -> (Vec<u8>, u64) {
        let document_count = store.all_doc_stats().keys().copied().max().map(|id| id + 1).unwrap_or(0);
        let mut out = vec![0u8; document_count as usize * DOC_REC_SIZE];
        for (doc_id, stats) in store.all_doc_stats() {
            let offset = *doc_id as usize * DOC_REC_SIZE;
            out[offset..offset + 8].copy_from_slice(&doc_id.to_le_bytes());
            out[offset + 32..offset + 36].copy_from_slice(&stats.doc_len.to_le_bytes());
            out[offset + 44..offset + 48].copy_from_slice(&stats.importance.to_le_bytes());
            let path_len = stats.path.len().min(DOC_PATH_MAX);
            out[offset + 72..offset + 74].copy_from_slice(&(path_len as u16).to_le_bytes());
            out[offset + 768..offset + 768 + path_len].copy_from_slice(&stats.path.as_bytes()[..path_len]);
        }
        (out, document_count)
    }
}

fn write_leaf_entry(block: &mut LeafTermBlock, entry_index: usize, offset: usize, entry: &LeafTermEntry) {
    block.ltb_directory[entry_index] = (LEAF_TERM_DATA_OFFSET + offset) as u16;
    let data = &mut block.ltb_data[offset..];
    data[0..4].copy_from_slice(&entry.lte_doc_freq.to_le_bytes());
    data[4..8].copy_from_slice(&entry.lte_index_block_id.to_le_bytes());
    data[8..12].copy_from_slice(&entry.lte_index_offset.to_le_bytes());
    data[12..16].copy_from_slice(&entry.lte_index_length.to_le_bytes());
    data[16..20].copy_from_slice(&entry.lte_continuation_block_count.to_le_bytes());
    data[20..24].copy_from_slice(&entry.lte_flags.to_le_bytes());
    data[24] = entry.lte_term.len() as u8;
    data[25..25 + entry.lte_term.len()].copy_from_slice(entry.lte_term.as_bytes());
}

fn read_vbc_pair_end(data: &[u8], offset: &mut usize) -> bool {
    let read_one = |data: &[u8], offset: &mut usize| -> bool {
        while *offset < data.len() {
            let byte = data[*offset];
            *offset += 1;
            if byte & 0x80 == 0 { return true; }
        }
        false
    };
    read_one(data, offset) && read_one(data, offset)
}

fn posting_prefix_bytes(data: &[u8], capacity: usize) -> usize {
    let mut cursor = 0usize;
    let mut last_pair_end = 0usize;
    let limit = data.len().min(capacity);
    while cursor < limit {
        if !read_vbc_pair_end(data, &mut cursor) || cursor > limit {
            break;
        }
        last_pair_end = cursor;
    }
    last_pair_end
}

fn max_doc_id_in_pairs(data: &[u8]) -> u64 {
    let mut cursor = 0usize;
    let mut max_doc_id = 0u64;
    while cursor < data.len() {
        let (doc_id, doc_bytes) = vb_read(data, cursor);
        cursor += doc_bytes;
        if cursor >= data.len() { break; }
        let (_tf, tf_bytes) = vb_read(data, cursor);
        cursor += tf_bytes;
        max_doc_id = doc_id;
    }
    max_doc_id
}

fn page_aligned_bytes(bytes: usize) -> usize {
    ((bytes + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE
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

fn u16_at(data: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(data[offset..offset + 2].try_into().unwrap())
}
fn u32_at(data: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}
fn u64_at(data: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap())
}
fn f32_at(data: &[u8], offset: usize) -> f32 {
    f32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}
fn write_u32(data: &mut [u8], offset: usize, value: u32) {
    data[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn write_u64(data: &mut [u8], offset: usize, value: u64) {
    data[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
fn write_f32(data: &mut [u8], offset: usize, value: f32) {
    data[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
