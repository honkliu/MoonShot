use std::cell::RefCell;
use std::sync::Arc;

pub const PAGE_SIZE:          usize = 4096;
pub const IB_SKIP_SLOTS:      usize = 50;
pub const IB_DATA_OFFSET:     usize = 8 + IB_SKIP_SLOTS * 4;
pub const DATA_SIZE:          usize = PAGE_SIZE - IB_DATA_OFFSET;
pub const IB_HEADER_HAS_MORE: u64   = 1u64 << 63;
pub const CONT_MARKER:        u16   = 0xFFFF;

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
// Level-1  HeadTermEntry — sparse head table, memory resident.
//   Sorted by HTE_FirstTerm. Binary search finds the candidate LeafTermBlock.
//
// Level-2  LeafTermBlock — fixed-size group of LeafTermEntry records.
//   Each LeafTermEntry describes where index bytes are stored; it never stores
//   index bytes directly.

/// Level-1 head entry.
#[derive(Debug, Clone)]
pub struct HeadTermEntry {
    pub hte_first_term:          String,
    pub hte_leaf_term_block_id:  u32,
}

/// Level-2 per-term index descriptor (lives inside a LeafTermBlock).
#[derive(Debug, Clone)]
pub struct LeafTermEntry {
    pub lte_term:                       String,
    pub lte_doc_freq:                   u32,
    pub lte_index_block_id:             u32,
    pub lte_index_offset:               u32,
    pub lte_index_length:               u32,
    pub lte_page_skip_offset:           u32,
    pub lte_continuation_block_count:   u32,
    pub lte_flags:                      u32,
}

#[derive(Debug, Clone, Default)]
pub struct LeafTermBlock {
    pub ltb_entries: Vec<LeafTermEntry>,
}

// ── IndexLocation (returned by find_term_data) ───────────────────────────────
pub struct IndexLocation {
    pub index_block_id:             u32,
    pub index_offset:               usize,
    pub index_length:               usize,
    pub doc_freq:                   u32,
    pub continuation_block_count:   u32,
    pub page_skip_offset:           u32,
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
/// IndexBlock manager + two-level Head/Leaf term table.
///
/// Lookup path:
///   BloomFilter.can_term_exist()              → reject obviously absent terms
///   Level-1 binary search on head_term_entries → leaf term block id
///   Level-2 binary search in LeafTermBlock     → LeafTermEntry
///   get_block_by_seq(entry.lte_index_block_id) → load IndexBlock
pub struct IndexBlockTable {
    cache:            RefCell<BlockCache>,
    /// Level-1: head table sorted by HTE_FirstTerm
    head_term_entries: Vec<HeadTermEntry>,
    /// Level-2: fixed-size LeafTermBlock entries
    leaf_term_blocks:  Vec<LeafTermBlock>,
    page_skip_data:   Vec<u64>,
    bloom:            BloomFilter,
}

impl IndexBlockTable {
    pub fn new(capacity: usize) -> Self {
        Self {
            cache:           RefCell::new(BlockCache::new(capacity)),
            head_term_entries: Vec::new(),
            leaf_term_blocks:  Vec::new(),
            page_skip_data:  Vec::new(),
            bloom:           BloomFilter,
        }
    }

    // ── write path ───────────────────────────────────────────────────────────

    pub fn insert_block(&self, seq: u32, block: IndexBlock) {
        self.cache.borrow_mut().insert(seq, block);
    }

    pub fn set_head_leaf_term_table(&mut self,
                                    head:   Vec<HeadTermEntry>,
                                    blocks: Vec<LeafTermBlock>)
    {
        self.head_term_entries = head;
        self.leaf_term_blocks  = blocks;
    }

    pub fn set_page_skip_data(&mut self, data: Vec<u64>) { self.page_skip_data = data; }

    pub fn get_page_skip_ptr(&self, offset: u32) -> Option<&[u64]> {
        let o = offset as usize;
        if offset == 0 || o >= self.page_skip_data.len() { None }
        else { Some(&self.page_skip_data[o..]) }
    }

    // ── read path ────────────────────────────────────────────────────────────

    /// Two-level lookup through HeadTermEntry + LeafTermBlock:
    ///   Step 1 — BloomFilter check (placeholder).
    ///   Step 2 — Level-1: binary search head table for last entry with
    ///             HTE_FirstTerm <= term.
    ///   Step 3 — Level-2: binary search within that LeafTermBlock for exact term.
    pub fn find_term_data(&self, term: &str) -> Option<(IndexLocation, Arc<IndexBlock>)> {
        if !self.bloom.can_term_exist(term) { return None; }
        if self.head_term_entries.is_empty() { return None; }

        // Level-1: find last head entry whose HTE_FirstTerm <= term
        let pos = self.head_term_entries.partition_point(|e| e.hte_first_term.as_str() <= term);
        if pos == 0 { return None; }
        let block_idx = self.head_term_entries[pos - 1].hte_leaf_term_block_id as usize;
        if block_idx >= self.leaf_term_blocks.len() { return None; }

        // Level-2: binary search for exact term in the LeafTermBlock
        let blk = &self.leaf_term_blocks[block_idx].ltb_entries;
        let ep  = blk.partition_point(|e| e.lte_term.as_str() < term);
        if ep >= blk.len() || blk[ep].lte_term != term { return None; }
        let entry = &blk[ep];

        let block = self.get_block_by_seq(entry.lte_index_block_id)?;
        Some((IndexLocation {
            index_block_id:             entry.lte_index_block_id,
            index_offset:               entry.lte_index_offset as usize,
            index_length:               entry.lte_index_length as usize,
            doc_freq:                   entry.lte_doc_freq,
            continuation_block_count:   entry.lte_continuation_block_count,
            page_skip_offset:           entry.lte_page_skip_offset,
        }, block))
    }

    pub fn get_block_by_seq(&self, seq: u32) -> Option<Arc<IndexBlock>> {
        self.cache.borrow_mut().get(seq)
    }

    pub fn head_term_entries(&self)   -> &[HeadTermEntry] { &self.head_term_entries }
    pub fn leaf_term_blocks(&self)    -> &[LeafTermBlock] { &self.leaf_term_blocks }
    pub fn page_skip_data(&self)      -> &[u64]                { &self.page_skip_data }

    /// Iterate all LeafTermEntry records (flattened) — used by the WASM viewer.
    pub fn all_leaf_term_entries(&self) -> impl Iterator<Item = &LeafTermEntry> {
        self.leaf_term_blocks.iter().flat_map(|blk| blk.ltb_entries.iter())
    }
}
