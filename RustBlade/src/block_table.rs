use std::cell::RefCell;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::ops::Deref;
use std::sync::Arc;

use crate::pinned_memory::PinnedMemory;

pub const PAGE_SIZE: usize = 4096;
pub const DOC_REC_SIZE: usize = 1024;
pub const DOC_VECTOR_DIM: usize = 512;
pub const DOC_VECTOR_STORAGE_MAX_DIM: usize = DOC_VECTOR_DIM;
pub const DOC_PATH_MAX: usize = 256;
pub const HEAD_TERM_KEY_MAX: usize = 26;
pub const LEAF_TERM_DIRECTORY_COUNT: usize = 96;
pub const LEAF_TERM_DATA_OFFSET: usize = LEAF_TERM_DIRECTORY_COUNT * std::mem::size_of::<u16>();
pub const INDEX_FILE_HEADER_SIZE: usize = 88;
pub const INDEX_FORMAT_VERSION: u32 = 12;
pub const INDEX_BLOCK_CONTINUATION_HEADER_SIZE: usize = 12;

#[derive(Clone, Copy)]
pub struct IndexBlock {
    pub ib_data: [u8; PAGE_SIZE],
}

impl Default for IndexBlock {
    fn default() -> Self { Self { ib_data: [0; PAGE_SIZE] } }
}

#[derive(Clone, Copy)]
pub struct LeafTermBlock {
    pub ltb_directory: [u16; LEAF_TERM_DIRECTORY_COUNT],
    pub ltb_data: [u8; PAGE_SIZE - LEAF_TERM_DATA_OFFSET],
}

impl Default for LeafTermBlock {
    fn default() -> Self {
        Self {
            ltb_directory: [0; LEAF_TERM_DIRECTORY_COUNT],
            ltb_data: [0; PAGE_SIZE - LEAF_TERM_DATA_OFFSET],
        }
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct IndexBlockContinuationHeader {
    pub ibch_max_doc_id: u64,
    pub ibch_data_length: u32,
}

impl IndexBlockContinuationHeader {
    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < INDEX_BLOCK_CONTINUATION_HEADER_SIZE { return None; }
        Some(Self {
            ibch_max_doc_id: u64::from_le_bytes(data[0..8].try_into().ok()?),
            ibch_data_length: u32::from_le_bytes(data[8..12].try_into().ok()?),
        })
    }

    pub fn write_to(&self, data: &mut [u8]) {
        data[0..8].copy_from_slice(&self.ibch_max_doc_id.to_le_bytes());
        data[8..12].copy_from_slice(&self.ibch_data_length.to_le_bytes());
    }
}

#[derive(Debug, Clone)]
pub struct HeadTermEntry {
    pub hte_leaf_term_block_id: u32,
    pub hte_first_term_length: u16,
    pub hte_first_term: [u8; HEAD_TERM_KEY_MAX],
}

impl HeadTermEntry {
    pub fn new(term: &str, block_id: u32) -> Self {
        let bytes = term.as_bytes();
        let mut hte_first_term = [0u8; HEAD_TERM_KEY_MAX];
        hte_first_term[..bytes.len()].copy_from_slice(bytes);
        Self {
            hte_leaf_term_block_id: block_id,
            hte_first_term_length: bytes.len() as u16,
            hte_first_term,
        }
    }

    pub fn first_term(&self) -> &str {
        let len = self.hte_first_term_length as usize;
        std::str::from_utf8(&self.hte_first_term[..len]).unwrap_or("")
    }

    pub fn to_bytes(&self) -> [u8; 32] {
        let mut out = [0u8; 32];
        out[0..4].copy_from_slice(&self.hte_leaf_term_block_id.to_le_bytes());
        out[4..6].copy_from_slice(&self.hte_first_term_length.to_le_bytes());
        out[6..32].copy_from_slice(&self.hte_first_term);
        out
    }

    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < 32 { return None; }
        let mut hte_first_term = [0u8; HEAD_TERM_KEY_MAX];
        hte_first_term.copy_from_slice(&data[6..32]);
        Some(Self {
            hte_leaf_term_block_id: u32::from_le_bytes(data[0..4].try_into().ok()?),
            hte_first_term_length: u16::from_le_bytes(data[4..6].try_into().ok()?),
            hte_first_term,
        })
    }
}

#[derive(Debug, Clone)]
pub struct LeafTermEntry {
    pub lte_term: String,
    pub lte_doc_freq: u32,
    pub lte_index_block_id: u32,
    pub lte_index_offset: u32,
    pub lte_index_length: u32,
    pub lte_continuation_block_count: u32,
    pub lte_flags: u32,
}

impl LeafTermEntry {
    pub fn byte_len(&self) -> usize { 25 + self.lte_term.len() }
}

impl LeafTermBlock {
    pub fn entry_count(&self) -> usize {
        self.ltb_directory[LEAF_TERM_DIRECTORY_COUNT - 1] as usize
    }

    pub fn entry(&self, index: usize) -> Option<LeafTermEntry> {
        if index >= self.entry_count() { return None; }
        let block_offset = self.ltb_directory[index] as usize;
        if block_offset < LEAF_TERM_DATA_OFFSET { return None; }
        let offset = block_offset - LEAF_TERM_DATA_OFFSET;
        if offset + 25 > self.ltb_data.len() { return None; }

        let data = &self.ltb_data[offset..];
        let term_len = data[24] as usize;
        if offset + 25 + term_len > self.ltb_data.len() { return None; }
        let lte_term = std::str::from_utf8(&data[25..25 + term_len]).ok()?.to_string();

        Some(LeafTermEntry {
            lte_doc_freq: u32::from_le_bytes(data[0..4].try_into().ok()?),
            lte_index_block_id: u32::from_le_bytes(data[4..8].try_into().ok()?),
            lte_index_offset: u32::from_le_bytes(data[8..12].try_into().ok()?),
            lte_index_length: u32::from_le_bytes(data[12..16].try_into().ok()?),
            lte_continuation_block_count: u32::from_le_bytes(data[16..20].try_into().ok()?),
            lte_flags: u32::from_le_bytes(data[20..24].try_into().ok()?),
            lte_term,
        })
    }

    pub fn entries(&self) -> Vec<LeafTermEntry> {
        (0..self.entry_count()).filter_map(|index| self.entry(index)).collect()
    }

    pub fn to_bytes(&self) -> [u8; PAGE_SIZE] {
        let mut out = [0u8; PAGE_SIZE];
        for (index, value) in self.ltb_directory.iter().enumerate() {
            let offset = index * 2;
            out[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
        }
        out[LEAF_TERM_DATA_OFFSET..].copy_from_slice(&self.ltb_data);
        out
    }

    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < PAGE_SIZE { return None; }
        let mut block = LeafTermBlock::default();
        for index in 0..LEAF_TERM_DIRECTORY_COUNT {
            let offset = index * 2;
            block.ltb_directory[index] = u16::from_le_bytes(data[offset..offset + 2].try_into().ok()?);
        }
        block.ltb_data.copy_from_slice(&data[LEAF_TERM_DATA_OFFSET..PAGE_SIZE]);
        Some(block)
    }
}

pub struct BloomFilter;
impl BloomFilter {
    pub fn can_term_exist(&self, _term: &str) -> bool { true }
}

#[derive(Clone, Copy)]
struct IndexSlotEntry {
    block_id: u32,
    ref_count: u32,
}

impl Default for IndexSlotEntry {
    fn default() -> Self { Self { block_id: u32::MAX, ref_count: 0 } }
}

#[derive(Clone)]
pub struct PinnedBlock<T: Copy> {
    pages: Arc<PinnedMemory<T>>,
    slot: usize,
}

impl<T: Copy> Deref for PinnedBlock<T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { &self.pages[self.slot] }
}

struct BlockCachePool<T: Default + Copy> {
    pages: Option<Arc<PinnedMemory<T>>>,
    path: Option<String>,
    base_offset: u64,
    total_block_count: u32,
    slot_count: u32,
    evict_slot: u32,
    logic_table: Option<PinnedMemory<u32>>,
    slot_table: Option<PinnedMemory<IndexSlotEntry>>,
}

impl<T: Default + Copy> BlockCachePool<T> {
    fn new() -> Self {
        Self {
            pages: None,
            path: None,
            base_offset: 0,
            total_block_count: 0,
            slot_count: 0,
            evict_slot: 0,
            logic_table: None,
            slot_table: None,
        }
    }

    fn reset_tables(&mut self, total_block_count: u32, slot_count: u32) {
        self.total_block_count = total_block_count;
        self.slot_count = slot_count.min(total_block_count);
        self.evict_slot = self.slot_count;
        self.logic_table = Some(PinnedMemory::from_slice(&vec![u32::MAX; self.total_block_count as usize]));
        self.slot_table = Some(PinnedMemory::from_slice(&vec![IndexSlotEntry::default(); self.slot_count as usize]));
    }

    fn set_pages(&mut self, pages: Vec<T>) {
        self.path = None;
        self.base_offset = 0;
        self.reset_tables(pages.len() as u32, pages.len() as u32);
        self.pages = Some(Arc::new(PinnedMemory::from_slice(&pages)));

        for block in 0..self.slot_count {
            self.logic_table.as_mut().unwrap()[block as usize] = block;
            self.slot_table.as_mut().unwrap()[block as usize].block_id = block;
        }
    }

    fn init_file_backed(&mut self,
                        path: String,
                        base_offset: u64,
                        total_block_count: u32,
                        slot_count: u32) -> std::io::Result<()>
    {
        self.path = Some(path.clone());
        self.base_offset = base_offset;
        self.reset_tables(total_block_count, slot_count);
        let mut pages = PinnedMemory::<T>::new_zeroed(self.slot_count as usize);

        let mut file = File::open(path)?;
        for block in 0..self.slot_count {
            file.seek(SeekFrom::Start(self.base_offset + block as u64 * PAGE_SIZE as u64))?;
            let page = &mut pages.as_mut_slice()[block as usize];
            let bytes = unsafe {
                std::slice::from_raw_parts_mut(page as *mut T as *mut u8, PAGE_SIZE)
            };
            file.read_exact(bytes)?;
            self.logic_table.as_mut().unwrap()[block as usize] = block;
            self.slot_table.as_mut().unwrap()[block as usize].block_id = block;
        }

        self.pages = Some(Arc::new(pages));
        Ok(())
    }

    fn get(&mut self, block_seq: u32) -> Option<(u32, PinnedBlock<T>)> {
        let logic_table = self.logic_table.as_ref()?;
        let slot = *logic_table.as_slice().get(block_seq as usize)?;
        if slot == u32::MAX { return None; }
        let entry = self.slot_table.as_mut()?.as_mut_slice().get_mut(slot as usize)?;
        entry.ref_count += 1;
        let pages = Arc::clone(self.pages.as_ref()?);
        Some((slot, PinnedBlock { pages, slot: slot as usize }))
    }

    fn read_miss(&mut self, block_seq: u32) -> Option<(u32, PinnedBlock<T>)> {
        let path = self.path.as_ref()?.clone();
        let mut found = u32::MAX;
        for _ in 0..self.slot_count {
            let candidate = self.evict_slot % self.slot_count;
            self.evict_slot = self.evict_slot.wrapping_add(1);
            if self.slot_table.as_ref()?.as_slice()[candidate as usize].ref_count == 0 {
                found = candidate;
                break;
            }
        }
        if found == u32::MAX { return None; }

        let old_block = self.slot_table.as_ref()?.as_slice()[found as usize].block_id;
        if old_block != u32::MAX {
            self.logic_table.as_mut()?.as_mut_slice()[old_block as usize] = u32::MAX;
        }

        let mut file = File::open(path).ok()?;
        file.seek(SeekFrom::Start(self.base_offset + block_seq as u64 * PAGE_SIZE as u64)).ok()?;
        let pages = Arc::get_mut(self.pages.as_mut()?)?;
        let page = &mut pages.as_mut_slice()[found as usize];
        let bytes = unsafe {
            std::slice::from_raw_parts_mut(page as *mut T as *mut u8, PAGE_SIZE)
        };
        file.read_exact(bytes).ok()?;

        self.slot_table.as_mut()?.as_mut_slice()[found as usize].block_id = block_seq;
        self.slot_table.as_mut()?.as_mut_slice()[found as usize].ref_count = 1;
        self.logic_table.as_mut()?.as_mut_slice()[block_seq as usize] = found;
        let pages = Arc::clone(self.pages.as_ref()?);
        Some((found, PinnedBlock { pages, slot: found as usize }))
    }

    fn get_or_read(&mut self, block_seq: u32) -> Option<(u32, PinnedBlock<T>)> {
        if let Some(hit) = self.get(block_seq) { return Some(hit); }
        self.read_miss(block_seq)
    }
}

pub struct IndexLocation {
    pub index_block_id: u32,
    pub index_offset: usize,
    pub index_length: usize,
    pub doc_freq: u32,
    pub continuation_block_count: u32,
}

pub struct IndexBlockTable {
    index_pool: RefCell<BlockCachePool<IndexBlock>>,
    leaf_term_pool: RefCell<BlockCachePool<LeafTermBlock>>,
    head_term_entries: Vec<HeadTermEntry>,
    bloom: BloomFilter,
}

impl IndexBlockTable {
    pub fn new(_capacity: usize) -> Self {
        Self {
            index_pool: RefCell::new(BlockCachePool::new()),
            leaf_term_pool: RefCell::new(BlockCachePool::new()),
            head_term_entries: Vec::new(),
            bloom: BloomFilter,
        }
    }

    pub fn set_index_blocks(&mut self, blocks: Vec<IndexBlock>) {
        self.index_pool.borrow_mut().set_pages(blocks);
    }

    pub fn set_leaf_term_blocks(&mut self, blocks: Vec<LeafTermBlock>) {
        self.leaf_term_pool.borrow_mut().set_pages(blocks);
    }

    pub fn init_file_backed(&mut self,
                            path: &str,
                            index_base_offset: u64,
                            index_block_count: u32,
                            index_slot_count: u32,
                            leaf_base_offset: u64,
                            leaf_block_count: u32,
                            leaf_slot_count: u32) -> std::io::Result<()> {
        self.index_pool.borrow_mut().init_file_backed(
            path.to_string(), index_base_offset, index_block_count, index_slot_count)?;
        self.leaf_term_pool.borrow_mut().init_file_backed(
            path.to_string(), leaf_base_offset, leaf_block_count, leaf_slot_count)?;
        Ok(())
    }

    pub fn insert_block(&mut self, seq: u32, block: IndexBlock) {
        let mut pages: Vec<IndexBlock> = self.index_pool
            .borrow()
            .pages
            .as_ref()
            .map(|pages| pages.as_slice().to_vec())
            .unwrap_or_default();
        if pages.len() <= seq as usize { pages.resize_with(seq as usize + 1, IndexBlock::default); }
        pages[seq as usize] = block;
        self.set_index_blocks(pages);
    }

    pub fn set_head_leaf_term_table(&mut self, head: Vec<HeadTermEntry>, blocks: Vec<LeafTermBlock>) {
        self.head_term_entries = head;
        self.set_leaf_term_blocks(blocks);
    }

    pub fn set_head_entries(&mut self, head: Vec<HeadTermEntry>) {
        self.head_term_entries = head;
    }

    pub fn find_term_data(&self, term: &str) -> Option<(IndexLocation, PinnedBlock<IndexBlock>)> {
        if !self.bloom.can_term_exist(term) { return None; }
        if self.head_term_entries.is_empty() { return None; }

        let pos = self.head_term_entries.partition_point(|entry| entry.first_term() <= term);
        if pos == 0 { return None; }
        let leaf_block_id = self.head_term_entries[pos - 1].hte_leaf_term_block_id;
        let (_leaf_slot, leaf_block) = self.leaf_term_pool.borrow_mut().get_or_read(leaf_block_id)?;

        let entry_count = leaf_block.entry_count();
        let mut left = 0usize;
        let mut right = entry_count;
        while left < right {
            let mid = left + (right - left) / 2;
            let entry = leaf_block.entry(mid)?;
            if entry.lte_term.as_str() < term { left = mid + 1; }
            else { right = mid; }
        }

        if left == entry_count { return None; }
        let entry = leaf_block.entry(left)?;
        if entry.lte_term != term { return None; }

        let (_index_slot, index_block) = self.index_pool.borrow_mut().get_or_read(entry.lte_index_block_id)?;
        Some((IndexLocation {
            index_block_id: entry.lte_index_block_id,
            index_offset: entry.lte_index_offset as usize,
            index_length: entry.lte_index_length as usize,
            doc_freq: entry.lte_doc_freq,
            continuation_block_count: entry.lte_continuation_block_count,
        }, index_block))
    }

    pub fn get_block_by_seq(&self, seq: u32) -> Option<PinnedBlock<IndexBlock>> {
        self.index_pool.borrow_mut().get_or_read(seq).map(|(_, block)| block)
    }

    pub fn head_term_entries(&self) -> &[HeadTermEntry] { &self.head_term_entries }

    pub fn leaf_term_blocks(&self) -> Vec<LeafTermBlock> {
        self.leaf_term_pool
            .borrow()
            .pages
            .as_ref()
            .map(|pages| pages.as_slice().to_vec())
            .unwrap_or_default()
    }

    pub fn all_leaf_term_entries(&self) -> Vec<LeafTermEntry> {
        self.leaf_term_blocks().iter().flat_map(|block| block.entries()).collect()
    }
}
