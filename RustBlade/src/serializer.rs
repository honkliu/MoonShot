use std::fs::File;
use std::io::{BufReader, Read, Seek, Write};

use crate::block_table::{
    HeadTermEntry,
    IndexBlock,
    IndexBlockTable,
    IndexBlockContinuationHeader,
    LeafTermBlock,
    LeafTermEntry,
    DOC_PATH_MAX,
    DOC_REC_SIZE,
    DOC_VECTOR_DIM,
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

#[derive(Debug, Clone, Copy, Default)]
pub struct IndexFileHeader {
    pub ifh_avg_doc_length: f32,
    pub ifh_num_documents: u64,
    pub ifh_num_terms: u64,
    pub ifh_head_term_entry_offset: u64,
    pub ifh_head_term_entry_count: u64,
    pub ifh_leaf_term_block_offset: u64,
    pub ifh_leaf_term_block_count: u64,
    pub ifh_doc_data_offset: u64,
    pub ifh_index_block_offset: u64,
    pub ifh_index_block_count: u64,
}

impl IndexFileHeader {
    pub fn parse(data: &[u8]) -> Result<Self> {
        if data.len() < INDEX_FILE_HEADER_SIZE { return Err(RustBladeError::InvalidFormat); }
        if &data[0..8] != MAGIC { return Err(RustBladeError::InvalidFormat); }
        if u32_at(data, 8) != INDEX_FORMAT_VERSION { return Err(RustBladeError::InvalidFormat); }
        Ok(Self {
            ifh_avg_doc_length: f32_at(data, 12),
            ifh_num_documents: u64_at(data, 16),
            ifh_num_terms: u64_at(data, 24),
            ifh_head_term_entry_offset: u64_at(data, 32),
            ifh_head_term_entry_count: u64_at(data, 40),
            ifh_leaf_term_block_offset: u64_at(data, 48),
            ifh_leaf_term_block_count: u64_at(data, 56),
            ifh_doc_data_offset: u64_at(data, 64),
            ifh_index_block_offset: u64_at(data, 72),
            ifh_index_block_count: u64_at(data, 80),
        })
    }

    pub fn to_bytes(&self) -> [u8; INDEX_FILE_HEADER_SIZE] {
        let mut out = [0u8; INDEX_FILE_HEADER_SIZE];
        out[0..8].copy_from_slice(MAGIC);
        write_u32(&mut out, 8, INDEX_FORMAT_VERSION);
        write_f32(&mut out, 12, self.ifh_avg_doc_length);
        write_u64(&mut out, 16, self.ifh_num_documents);
        write_u64(&mut out, 24, self.ifh_num_terms);
        write_u64(&mut out, 32, self.ifh_head_term_entry_offset);
        write_u64(&mut out, 40, self.ifh_head_term_entry_count);
        write_u64(&mut out, 48, self.ifh_leaf_term_block_offset);
        write_u64(&mut out, 56, self.ifh_leaf_term_block_count);
        write_u64(&mut out, 64, self.ifh_doc_data_offset);
        write_u64(&mut out, 72, self.ifh_index_block_offset);
        write_u64(&mut out, 80, self.ifh_index_block_count);
        out
    }
}

#[allow(non_snake_case)]
pub struct BuildBlocksResult {
    pub BBR_IndexBlocks: Vec<IndexBlock>,
    pub BBR_HeadTermEntries: Vec<HeadTermEntry>,
    pub BBR_LeafTermBlocks: Vec<LeafTermBlock>,
    pub BBR_TotalTerms: u64,
}

pub struct IndexSerializer;

impl IndexSerializer {
    #[allow(non_snake_case)]
    pub fn Save(header: &IndexFileHeader, blockTable: &IndexBlockTable, docData: &[u8], path: &str) -> Result<()> {
        let mut file = File::create(path)?;
        file.write_all(&header.to_bytes())?;

        if header.ifh_head_term_entry_count > 0 {
            let mut bytes = Vec::with_capacity(header.ifh_head_term_entry_count as usize * 32);
            for entry in blockTable.HeadTermEntries() {
                bytes.extend_from_slice(&entry.to_bytes());
            }
            file.write_all(&bytes)?;
        }

        if header.ifh_leaf_term_block_count > 0 {
            let blocks = blockTable.LeafTermBlocks();
            let mut bytes = Vec::with_capacity(blocks.len() * PAGE_SIZE);
            for block in &blocks {
                bytes.extend_from_slice(&block.to_bytes());
            }
            file.write_all(&bytes)?;
        }

        if header.ifh_num_documents > 0 {
            file.write_all(docData)?;
        }

        if header.ifh_index_block_count > 0 {
            let blocks = blockTable.IndexBlocks();
            let mut bytes = Vec::with_capacity(blocks.len() * PAGE_SIZE);
            for block in &blocks {
                bytes.extend_from_slice(&block.ib_data);
            }
            file.write_all(&bytes)?;
        }

        file.flush()?;
        Ok(())
    }

    pub fn load_file_tables(store: &mut PostingStore, path: &str)
        -> Result<(IndexFileHeader, Vec<HeadTermEntry>, Vec<u8>)>
    {
        let file = File::open(path)?;
        let mut reader = BufReader::new(file);
        let mut header_bytes = [0u8; INDEX_FILE_HEADER_SIZE];
        reader.read_exact(&mut header_bytes)?;
        let header = IndexFileHeader::parse(&header_bytes)?;

        let mut head = Vec::with_capacity(header.ifh_head_term_entry_count as usize);
        reader.seek(std::io::SeekFrom::Start(header.ifh_head_term_entry_offset))?;
        for _ in 0..header.ifh_head_term_entry_count {
            let mut bytes = [0u8; 32];
            reader.read_exact(&mut bytes)?;
            head.push(HeadTermEntry::from_bytes(&bytes).ok_or(RustBladeError::InvalidFormat)?);
        }

        let docdata = Self::load_docdata(store, &mut reader, &header)?;
        Ok((header, head, docdata))
    }

    pub fn decode(store: &mut PostingStore, data: &[u8])
        -> Result<(Vec<HeadTermEntry>, Vec<LeafTermBlock>, Vec<IndexBlock>, Vec<u8>)>
    {
        let header = IndexFileHeader::parse(data)?;

        let head_offset = header.ifh_head_term_entry_offset as usize;
        let head_count = header.ifh_head_term_entry_count as usize;
        let leaf_offset = header.ifh_leaf_term_block_offset as usize;
        let leaf_count = header.ifh_leaf_term_block_count as usize;
        let docdata_offset = header.ifh_doc_data_offset as usize;
        let index_offset = header.ifh_index_block_offset as usize;
        let index_count = header.ifh_index_block_count as usize;
        let num_docs = header.ifh_num_documents as usize;

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

        let docdata_size = num_docs * DOC_REC_SIZE;
        if docdata_offset + docdata_size > data.len() { return Err(RustBladeError::InvalidFormat); }
        let docdata = data[docdata_offset..docdata_offset + docdata_size].to_vec();

        for index in 0..num_docs {
            let offset = docdata_offset + index * DOC_REC_SIZE;
            Self::decode_docdata_record(store, index as u64, &data[offset..offset + DOC_REC_SIZE])?;
        }

        let mut index_blocks = Vec::with_capacity(index_count);
        for index in 0..index_count {
            let offset = index_offset + index * PAGE_SIZE;
            if offset + PAGE_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            let mut block = IndexBlock::default();
            block.ib_data.copy_from_slice(&data[offset..offset + PAGE_SIZE]);
            index_blocks.push(block);
        }

        Ok((head, leaf_blocks, index_blocks, docdata))
    }

    fn load_docdata<R: Read + std::io::Seek>(store: &mut PostingStore, reader: &mut R, header: &IndexFileHeader) -> Result<Vec<u8>> {
        reader.seek(std::io::SeekFrom::Start(header.ifh_doc_data_offset))?;
        let mut docdata = vec![0u8; header.ifh_num_documents as usize * DOC_REC_SIZE];
        reader.read_exact(&mut docdata)?;
        for index in 0..header.ifh_num_documents as usize {
            let offset = index * DOC_REC_SIZE;
            Self::decode_docdata_record(store, index as u64, &docdata[offset..offset + DOC_REC_SIZE])?;
        }
        Ok(docdata)
    }

    fn decode_docdata_record(store: &mut PostingStore, index: u64, record: &[u8]) -> Result<()> {
        let doc_id = u64_at(record, 0);
        if doc_id != index { return Ok(()); }
        let doc_len = u32_at(record, 32);
        let importance = f32_at(record, 44);
        let path_len = u16_at(record, 72) as usize;
        store.AddDocTokens(doc_id, doc_len);
        store.SetDocImportance(doc_id, importance);
        store.SetDocVectorBytes(doc_id, &record[256..256 + DOC_VECTOR_DIM]);
        if path_len > 0 && path_len <= DOC_PATH_MAX {
            if let Ok(path) = std::str::from_utf8(&record[768..768 + path_len]) {
                store.SetDocPath(doc_id, path.to_string());
            }
        }
        Ok(())
    }

    pub fn is_valid_index(path: &str) -> bool {
        let Ok(mut file) = File::open(path) else { return false; };
        let mut header = [0u8; 12];
        file.read_exact(&mut header).is_ok()
            && &header[0..8] == MAGIC
            && u32::from_le_bytes(header[8..12].try_into().unwrap()) == INDEX_FORMAT_VERSION
    }

    #[allow(non_snake_case)]
    pub fn BuildBlocks(store: &PostingStore) -> BuildBlocksResult {
        let mut terms: Vec<(&String, &crate::posting_store::PostingList)> = store.AllPostings().iter().collect();
        terms.sort_by_key(|(term, _)| term.as_str());

        let mut IndexBlocks = Vec::new();
        let mut cur = IndexBlock::default();
        let mut wptr = 0usize;
        let mut seq = 0u32;

        let mut leaf_blocks = Vec::new();
        let mut leaf_block = LeafTermBlock::default();
        let mut leaf_write_offset = 0usize;
        let mut leaf_entry_count = 0usize;
        let mut first_leaf_term = String::new();

        let flush_index_block = |IndexBlocks: &mut Vec<IndexBlock>, cur: &mut IndexBlock, wptr: &mut usize, seq: &mut u32| {
            IndexBlocks.push(cur.clone());
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
            let bytes = posting_list.get_bytes();
            if bytes.is_empty() { continue; }

            if wptr >= PAGE_SIZE {
                flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);
            }

            let mut src = 0usize;
            let mut remaining = bytes.len();
            let mut data_offset = wptr;
            let mut data_here = posting_prefix_bytes(&bytes[src..], PAGE_SIZE - wptr);
            if data_here == 0 {
                flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);
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
                flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);

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
                        flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);
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

        if wptr > 0 { flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq); }
        flush_leaf_block(&mut leaf_blocks, &mut head_entries, &mut leaf_block, &mut leaf_write_offset, &mut leaf_entry_count, &mut first_leaf_term);

        BuildBlocksResult {
            BBR_IndexBlocks: IndexBlocks,
            BBR_HeadTermEntries: head_entries,
            BBR_LeafTermBlocks: leaf_blocks,
            BBR_TotalTerms: total_terms,
        }
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
