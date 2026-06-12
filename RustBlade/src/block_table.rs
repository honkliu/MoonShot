use std::cell::RefCell;
use std::sync::Arc;

pub const PAGE_SIZE:          usize = 4096;
pub const IB_SKIP_SLOTS:      usize = 50;
pub const IB_DATA_OFFSET:     usize = 8 + IB_SKIP_SLOTS * 4;   /* 208 bytes */
pub const DATA_SIZE:          usize = PAGE_SIZE - IB_DATA_OFFSET; /* 3888 bytes */
pub const IB_HEADER_HAS_MORE: u64   = 1u64 << 63;
pub const CONT_MARKER:        u16   = 0xFFFF;

/// One 4 KB page — identical layout to C++ IndexBlock.
///   ib_header bit 63: HAS_MORE (posting list continues in block_seq + 1)
///   ib_header bits 62..0: block sequence number
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

// ── SubIndex ────────────────────────────────────────────────────────────────
/// Sparse in-memory index: one entry per block, sorted by lead term.
/// Mirrors Tiger's WordToPageMap / MoonShot's SubIndex.
#[derive(Debug, Clone)]
pub struct SubIndexEntry {
    pub term:      String,
    pub block_seq: u32,
}

// ── Location of a term's posting data within a block ────────────────────────
pub struct TermLocation {
    pub block_seq:     u32,
    pub data_offset:   usize,  // byte offset within ib_data
    pub data_len:      usize,  // posting bytes in THIS block
    pub doc_freq:      u32,
    pub is_last_entry: bool,   // true → HAS_MORE on block applies to this term
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
            let i = self.hand;
            self.hand = (self.hand + 1) % n;
            if !self.slots[i].valid   { return i; }
            if !self.slots[i].touched { self.slots[i].valid = false; return i; }
            self.slots[i].touched = false;
        }
        let v = self.hand; self.hand = (self.hand + 1) % n; v
    }
}

// ── IndexBlockTable ──────────────────────────────────────────────────────────
/// Page manager + SubIndex.  Mirrors C++ IndexBlockTable.
pub struct IndexBlockTable {
    cache:    RefCell<BlockCache>,
    subindex: Vec<SubIndexEntry>,   // sorted by term; replaces term_to_block
}

impl IndexBlockTable {
    pub fn new(capacity: usize) -> Self {
        Self { cache: RefCell::new(BlockCache::new(capacity)), subindex: Vec::new() }
    }

    // ── write path ───────────────────────────────────────────────────────────

    /// Store a fully-constructed IndexBlock (multi-term packed) into the cache.
    pub fn insert_block(&self, seq: u32, block: IndexBlock) {
        self.cache.borrow_mut().insert(seq, block);
    }

    /// Register the lead term of a new block.
    pub fn add_subindex_entry(&mut self, lead_term: &str, block_seq: u32) {
        self.subindex.push(SubIndexEntry { term: lead_term.to_string(), block_seq });
    }

    /// Replace the entire SubIndex (called by LoadIndex).
    pub fn set_subindex(&mut self, entries: Vec<SubIndexEntry>) {
        self.subindex = entries;
    }

    // ── read path ────────────────────────────────────────────────────────────

    /// Binary-search SubIndex for the block containing `term`, load block,
    /// then linear-scan IB_Data to find the exact entry.
    pub fn find_term_data(&self, term: &str) -> Option<(TermLocation, Arc<IndexBlock>)> {
        if self.subindex.is_empty() { return None; }

        // upper_bound: first entry whose lead term > term
        let pos = self.subindex.partition_point(|e| e.term.as_str() <= term);
        if pos == 0 { return None; }

        let block_seq = self.subindex[pos - 1].block_seq;
        let block     = self.get_block_by_seq(block_seq)?;

        let loc = Self::scan_block(&block, term, block_seq)?;
        Some((loc, block))
    }

    /// Load a block by seq (continuation blocks, cache or disk).
    pub fn get_block_by_seq(&self, seq: u32) -> Option<Arc<IndexBlock>> {
        self.cache.borrow_mut().get(seq)
    }

    pub fn subindex(&self) -> &[SubIndexEntry] { &self.subindex }

    // ── block scanner ────────────────────────────────────────────────────────

    fn scan_block(block: &IndexBlock, term: &str, block_seq: u32) -> Option<TermLocation> {
        let data = &block.ib_data;
        let len  = data.len();

        // Continuation blocks have no term entries.
        if len >= 2 && u16::from_le_bytes([data[0], data[1]]) == CONT_MARKER {
            return None;
        }

        let mut ptr = 0usize;
        while ptr + 2 <= len {
            let key_len = u16::from_le_bytes([data[ptr], data[ptr + 1]]) as usize;
            ptr += 2;

            if key_len == 0               { break; }  // sentinel
            if key_len == CONT_MARKER as usize { break; }  // shouldn't appear mid-block

            if ptr + key_len + 8 > len { break; }

            let key = match std::str::from_utf8(&data[ptr..ptr + key_len]) {
                Ok(s)  => s,
                Err(_) => { ptr += key_len + 8; continue; }
            };
            ptr += key_len;

            let doc_freq = u32::from_le_bytes([data[ptr], data[ptr+1], data[ptr+2], data[ptr+3]]);
            let data_len = u32::from_le_bytes([data[ptr+4], data[ptr+5], data[ptr+6], data[ptr+7]]) as usize;
            ptr += 8;

            if key == term {
                // Detect if this is the last term entry (determines HAS_MORE applicability).
                let next_ptr   = ptr + data_len;
                let next_kl    = if next_ptr + 2 <= len {
                    u16::from_le_bytes([data[next_ptr], data[next_ptr + 1]])
                } else { 0 };
                let is_last = next_ptr + 2 > len || next_kl == 0;

                return Some(TermLocation {
                    block_seq,
                    data_offset: ptr,
                    data_len,
                    doc_freq,
                    is_last_entry: is_last,
                });
            }

            // Bounds check before skipping posting data.
            if ptr + data_len > len { break; }
            ptr += data_len;
        }
        None
    }
}
