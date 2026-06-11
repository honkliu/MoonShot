use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::Arc;

pub const PAGE_SIZE:         usize = 4096;
pub const SKIP_SLOTS:        usize = 50;
pub const DATA_SIZE:         usize = PAGE_SIZE - 8 - SKIP_SLOTS * 4;  /* 3888 bytes */
pub const IB_HEADER_HAS_MORE: u64  = 1u64 << 63;

/*
* IndexBlock — one 4 KB page, equivalent to REF's Page.
*   ib_header bit 63 : HAS_MORE — posting list continues in block_seq + 1
*   ib_header bits 62..0 : block sequence number
*/
#[derive(Clone)]
pub struct IndexBlock {
    pub ib_header: u64,
    pub ib_skip:   [u32; SKIP_SLOTS],
    pub ib_data:   [u8; DATA_SIZE],
}

impl IndexBlock {
    pub fn block_seq(&self) -> u32 {
        (self.ib_header & !IB_HEADER_HAS_MORE) as u32
    }

    pub fn has_more(&self) -> bool {
        (self.ib_header & IB_HEADER_HAS_MORE) != 0
    }
}

impl Default for IndexBlock {
    fn default() -> Self {
        Self {
            ib_header: 0,
            ib_skip:   [0u32; SKIP_SLOTS],
            ib_data:   [0u8; DATA_SIZE],
        }
    }
}

struct CacheSlot {
    block_seq: u32,
    valid:     bool,
    touched:   bool,
    data:      Arc<IndexBlock>,
}

impl Default for CacheSlot {
    fn default() -> Self {
        Self {
            block_seq: u32::MAX,
            valid:     false,
            touched:   false,
            data:      Arc::new(IndexBlock::default()),
        }
    }
}

/*
* BlockCache — clock-replacement buffer pool.
* Equivalent to REF's HashCacheProxy / MoonShot's BlockCache.
*/
pub struct BlockCache {
    slots: Vec<CacheSlot>,
    hand:  usize,
}

impl BlockCache {
    pub fn new(capacity: usize) -> Self {
        let slots = (0..capacity).map(|_| CacheSlot::default()).collect();
        Self { slots, hand: 0 }
    }

    pub fn get(&mut self, block_seq: u32) -> Option<Arc<IndexBlock>> {
        for slot in &mut self.slots {
            if slot.valid && slot.block_seq == block_seq {
                slot.touched = true;
                return Some(Arc::clone(&slot.data));
            }
        }
        None
    }

    pub fn insert(&mut self, block_seq: u32, block: IndexBlock) -> Arc<IndexBlock> {
        let v = self.pick_victim();
        let arc = Arc::new(block);
        self.slots[v] = CacheSlot {
            block_seq,
            valid:   true,
            touched: true,
            data:    Arc::clone(&arc),
        };
        arc
    }

    fn pick_victim(&mut self) -> usize {
        let n = self.slots.len();
        for _ in 0..n * 2 {
            let i = self.hand;
            self.hand = (self.hand + 1) % n;
            if !self.slots[i].valid  { return i; }
            if !self.slots[i].touched { self.slots[i].valid = false; return i; }
            self.slots[i].touched = false;
        }
        let v = self.hand;
        self.hand = (self.hand + 1) % n;
        v
    }
}

/*
* IndexBlockTable — page manager + term→first-block mapping.
* Equivalent to REF's PageManager + MoonShot's IndexBlockTable.
* RefCell gives interior mutability for the cache during read-only traversal.
*/
pub struct IndexBlockTable {
    cache:          RefCell<BlockCache>,
    term_to_block:  HashMap<String, u32>,
}

impl IndexBlockTable {
    pub fn new(capacity: usize) -> Self {
        Self {
            cache:         RefCell::new(BlockCache::new(capacity)),
            term_to_block: HashMap::new(),
        }
    }

    pub fn insert_block(&self, block_seq: u32, data: &[u8], has_more: bool) {
        let mut block    = IndexBlock::default();
        block.ib_header  = block_seq as u64;
        if has_more { block.ib_header |= IB_HEADER_HAS_MORE; }
        let copy_len = data.len().min(DATA_SIZE - 1);
        block.ib_data[..copy_len].copy_from_slice(&data[..copy_len]);
        self.cache.borrow_mut().insert(block_seq, block);
    }

    pub fn add_term_mapping(&mut self, term: &str, block_seq: u32) {
        self.term_to_block.insert(term.to_string(), block_seq);
    }

    pub fn get_block_by_term(&self, term: &str) -> Option<Arc<IndexBlock>> {
        let seq = *self.term_to_block.get(term)?;
        self.get_block_by_seq(seq)
    }

    pub fn get_block_by_seq(&self, block_seq: u32) -> Option<Arc<IndexBlock>> {
        self.cache.borrow_mut().get(block_seq)
    }

    pub fn has_term(&self, term: &str) -> bool {
        self.term_to_block.contains_key(term)
    }
}
