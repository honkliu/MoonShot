use crate::block_table::{IndexBlock, PinnedBlock};

/// Stateful VarByte decoder.  Posting bytes store absolute (docID, tf) pairs.
/// Two modes:
///   OpenRaw — reads from an explicit byte slice (offset + len within a block).
pub struct VarByteDecoder {
    block:       Option<PinnedBlock<IndexBlock>>,
    raw_data:    Vec<u8>,
    pos:         usize,
    end:         usize,
    raw_mode:    bool,
    current_doc: u64,
    current_tf:  u32,
    has_current: bool,
}

impl VarByteDecoder {
    pub fn new() -> Self {
        Self {
            block: None, raw_data: Vec::new(),
            pos: 0, end: 0, raw_mode: false,
            current_doc: 0, current_tf: 0, has_current: false,
        }
    }

    /// Open on the term's posting bytes within a block's ib_data.
    /// `offset` and `len` are the byte range returned by IndexBlockTable::find_term_data.
    #[allow(non_snake_case)]
    pub fn OpenRaw(&mut self, block: PinnedBlock<IndexBlock>, offset: usize, len: usize, last_doc: u64) {
        let end = (offset + len).min(block.ib_data.len());
        self.raw_data    = block.ib_data[offset..end].to_vec();
        self.block       = Some(block);
        self.raw_mode    = true;
        self.pos         = 0;
        self.end         = self.raw_data.len();
        self.current_doc = last_doc;
        self.current_tf  = 0;
        self.has_current = false;
    }

    fn has_more_bytes(&self) -> bool {
        self.pos < self.end
    }

    #[allow(non_snake_case)]
    pub fn GoNext(&mut self) {
        if !self.has_more_bytes() {
            self.has_current = false;
            return;
        }
        let (doc_id, n) = vb_read(&self.raw_data, self.pos);
        self.pos += n;
        if self.pos >= self.end && n == 0 {
            self.has_current = false;
            return;
        }
        let (tf, m) = vb_read(&self.raw_data, self.pos);
        self.pos += m;

        self.current_doc  = doc_id;
        self.current_tf   = tf as u32;
        self.has_current  = true;
    }

    #[allow(non_snake_case)]
    pub fn GoUntil(&mut self, target: u64) {
        if !self.has_current && self.has_more_bytes() { self.GoNext(); }
        while self.has_current && self.current_doc < target { self.GoNext(); }
    }

    #[allow(non_snake_case)]
    pub fn IsEnd(&self)             -> bool  { !self.has_current }
    #[allow(non_snake_case)]
    pub fn GetDocumentID(&self)    -> u64   { self.current_doc }
    #[allow(non_snake_case)]
    pub fn GetTermFrequency(&self) -> u32   { self.current_tf }
    #[allow(non_snake_case)]
    pub fn GetCurrentBlock(&self)  -> Option<&PinnedBlock<IndexBlock>> { self.block.as_ref() }
}

impl Default for VarByteDecoder {
    fn default() -> Self { Self::new() }
}

pub fn vb_read(data: &[u8], start: usize) -> (u64, usize) {
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

pub fn vb_encode(mut v: u64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        if v < 0x80 { out.push(v as u8); break; }
        out.push((v as u8 & 0x7F) | 0x80);
        v >>= 7;
    }
    out
}
