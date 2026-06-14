use std::cell::RefCell;
use std::sync::Arc;

pub const PAGE_SIZE:          usize = 4096;
pub const IB_SKIP_SLOTS:      usize = 50;
pub const IB_DATA_OFFSET:     usize = 8 + IB_SKIP_SLOTS * 4;
pub const DATA_SIZE:          usize = PAGE_SIZE - IB_DATA_OFFSET;
pub const IB_HEADER_HAS_MORE: u64   = 1u64 << 63;
pub const CONT_MARKER:        u16   = 0xFFFF;

/// Number of TermHeader records per Level-2 header block.
pub const TERM_HEADERS_PER_BLOCK: usize = 32;

#[derive(Clone)]
pub struct IndexBlock {
    pub ib_header: u64,
    pub ib_skip:   [u32; IB_SKIP_SLOTS],
    pub ib_data:   [u8; DATA_SIZE],
}

impl IndexBlock {
    pub fn block_seq(&self)  -> u32  { (self.ib_header & !IB_HEADER_HAS_MORE) as u32 }
    pub fn has_more(&self)   -> bool { (self.ib_header & IB_HEADER_HAS_MORE) != 0 }
    pub fn is_continuation(&self) -> bool {
        self.ib_data.len() >= 2 && u16::from_le_bytes([self.ib_data[0], self.ib_data[1]]) == CONT_MARKER
    }
}

impl Default for IndexBlock {
    fn default() -> Self {
        Self { ib_header: 0, ib_skip: [0; IB_SKIP_SLOTS], ib_data: [0; DATA_SIZE] }
    }
}

// ── BloomFilter placeholder ─────────────────────────────────────────────────
pub struct BloomFilter;
impl BloomFilter {
    pub fn can_term_exist(&self, _term: &str) -> bool { true }
}

// ── Term lookup two-level structure ──────────────────────────────────────────
//
// Level-1  TermDirectoryEntry — sparse directory, memory resident.
//   Sorted by first_term. Binary search finds the candidate TermHeaderBlock.
//
// Level-2  TermHeaderBlock — fixed-size group of TermHeader records.
//   Each TermHeader describes where posting bytes are stored; it never stores
//   posting bytes directly.

/// Level-1 directory entry.
#[derive(Debug, Clone)]
pub struct TermDirectoryEntry {
    pub first_term:           String,
    pub term_header_block_id: u32,
}

/// Level-2 per-term posting descriptor (lives inside a TermHeaderBlock).
#[derive(Debug, Clone)]
pub struct TermHeader {
    pub term:                     String,
    pub doc_freq:                 u32,
    pub posting_block_id:         u32,
    pub posting_offset:           u32,
    pub posting_length:           u32,
    pub skip_list_offset:         u32,
    pub continuation_block_count: u32,
    pub flags:                    u32,
}

#[derive(Debug, Clone, Default)]
pub struct TermHeaderBlock {
    pub headers: Vec<TermHeader>,
}

// ── TermLocation (returned by find_term_data) ────────────────────────────────
pub struct TermLocation {
    pub posting_block_id:         u32,
    pub posting_offset:           usize,
    pub posting_length:           usize,
    pub doc_freq:                 u32,
    pub continuation_block_count: u32,
    pub skip_list_offset:         u32,
}

// ── BlockCache ───────────────────────────────────────────────────────────────
struct CacheSlot {
    block_seq: u32,
    valid:     bool,
    touched:   bool,
    data:      Arc<IndexBlock>,
}

impl Default for CacheSlot {
    fn default() -> Self {
        Self { block_seq: u32::MAX, valid: false, touched: false,
               data: Arc::new(IndexBlock::default()) }
    }
}

pub struct BlockCache {
    slots: Vec<CacheSlot>,
    hand:  usize,
}

impl BlockCache {
    pub fn new(capacity: usize) -> Self {
        Self { slots: (0..capacity).map(|_| CacheSlot::default()).collect(), hand: 0 }
    }

    pub fn get(&mut self, seq: u32) -> Option<Arc<IndexBlock>> {
        for s in &mut self.slots {
            if s.valid && s.block_seq == seq { s.touched = true; return Some(Arc::clone(&s.data)); }
        }
        None
    }

    pub fn insert(&mut self, seq: u32, block: IndexBlock) -> Arc<IndexBlock> {
        let v   = self.pick_victim();
        let arc = Arc::new(block);
        self.slots[v] = CacheSlot { block_seq: seq, valid: true, touched: true, data: Arc::clone(&arc) };
        arc
    }

    fn pick_victim(&mut self) -> usize {
        let n = self.slots.len();
        for _ in 0..n * 2 {
            let i = self.hand; self.hand = (self.hand + 1) % n;
            if !self.slots[i].valid   { return i; }
            if !self.slots[i].touched { self.slots[i].valid = false; return i; }
            self.slots[i].touched = false;
        }
        let v = self.hand; self.hand = (self.hand + 1) % n; v
    }
}

// ── IndexBlockTable ──────────────────────────────────────────────────────────
/// Posting block manager + two-level term header table.
///
/// Lookup path:
///   BloomFilter.can_term_exist()              → reject obviously absent terms
///   Level-1 binary search on term_directory   → term_header_block_id
///   Level-2 binary search in TermHeaderBlock  → TermHeader
///   get_block_by_seq(header.posting_block_id) → load posting block
pub struct IndexBlockTable {
    cache:            RefCell<BlockCache>,
    /// Level-1: directory sorted by first_term
    term_directory:   Vec<TermDirectoryEntry>,
    /// Level-2: fixed-size TermHeader blocks
    term_header_blocks: Vec<TermHeaderBlock>,
    page_skip_data:   Vec<u64>,
    bloom:            BloomFilter,
}

impl IndexBlockTable {
    pub fn new(capacity: usize) -> Self {
        Self {
            cache:           RefCell::new(BlockCache::new(capacity)),
            term_directory:    Vec::new(),
            term_header_blocks: Vec::new(),
            page_skip_data:  Vec::new(),
            bloom:           BloomFilter,
        }
    }

    // ── write path ───────────────────────────────────────────────────────────

    pub fn insert_block(&self, seq: u32, block: IndexBlock) {
        self.cache.borrow_mut().insert(seq, block);
    }

    pub fn set_term_header_table(&mut self,
                                 dir:    Vec<TermDirectoryEntry>,
                                 blocks: Vec<TermHeaderBlock>)
    {
        self.term_directory     = dir;
        self.term_header_blocks = blocks;
    }

    pub fn set_page_skip_data(&mut self, data: Vec<u64>) { self.page_skip_data = data; }

    pub fn get_page_skip_ptr(&self, offset: u32) -> Option<&[u64]> {
        let o = offset as usize;
        if offset == 0 || o >= self.page_skip_data.len() { None }
        else { Some(&self.page_skip_data[o..]) }
    }

    // ── read path ────────────────────────────────────────────────────────────

    /// Two-level lookup through TermDirectory + TermHeaderBlock:
    ///   Step 1 — BloomFilter check (placeholder).
    ///   Step 2 — Level-1: binary search term_directory for last entry with
    ///             first_term <= term.
    ///   Step 3 — Level-2: binary search within that TermHeaderBlock for exact term.
    pub fn find_term_data(&self, term: &str) -> Option<(TermLocation, Arc<IndexBlock>)> {
        if !self.bloom.can_term_exist(term) { return None; }
        if self.term_directory.is_empty()   { return None; }

        // Level-1: find last dir entry whose first_term <= term
        let pos = self.term_directory.partition_point(|e| e.first_term.as_str() <= term);
        if pos == 0 { return None; }
        let block_idx = self.term_directory[pos - 1].term_header_block_id as usize;
        if block_idx >= self.term_header_blocks.len() { return None; }

        // Level-2: binary search for exact term in the TermHeaderBlock
        let blk = &self.term_header_blocks[block_idx].headers;
        let ep  = blk.partition_point(|e| e.term.as_str() < term);
        if ep >= blk.len() || blk[ep].term != term { return None; }
        let header = &blk[ep];

        let block = self.get_block_by_seq(header.posting_block_id)?;
        Some((TermLocation {
            posting_block_id:         header.posting_block_id,
            posting_offset:           header.posting_offset as usize,
            posting_length:           header.posting_length as usize,
            doc_freq:                 header.doc_freq,
            continuation_block_count: header.continuation_block_count,
            skip_list_offset:         header.skip_list_offset,
        }, block))
    }

    pub fn get_block_by_seq(&self, seq: u32) -> Option<Arc<IndexBlock>> {
        self.cache.borrow_mut().get(seq)
    }

    pub fn term_directory(&self)      -> &[TermDirectoryEntry] { &self.term_directory }
    pub fn term_header_blocks(&self)  -> &[TermHeaderBlock]    { &self.term_header_blocks }
    pub fn page_skip_data(&self)      -> &[u64]                { &self.page_skip_data }

    /// Iterate all TermHeader records (flattened) — used by the WASM viewer.
    pub fn all_headers(&self) -> impl Iterator<Item = &TermHeader> {
        self.term_header_blocks.iter().flat_map(|blk| blk.headers.iter())
    }
}
