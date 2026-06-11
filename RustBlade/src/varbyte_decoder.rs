use std::sync::Arc;
use crate::block_table::{IndexBlock, IB_HEADER_HAS_MORE, DATA_SIZE};

/*
* VarByteDecoder — stateful decoder for VarByte-delta-encoded (docId, tf) pairs.
* Two open modes:
*   open(block)       — block-based; zero sentinel ends the stream
*   open_raw(data)    — raw byte slice; ptr >= end ends the stream
*
* Mirrors MoonShot's UnifiedDecoder.
*/
pub struct VarByteDecoder {
    block:       Option<Arc<IndexBlock>>,
    raw_data:    Vec<u8>,
    pos:         usize,
    end:         usize,
    current_doc: u64,
    current_tf:  u32,
    has_current: bool,
    raw_mode:    bool,
}

impl VarByteDecoder {
    pub fn new() -> Self {
        Self {
            block:       None,
            raw_data:    Vec::new(),
            pos:         0,
            end:         0,
            current_doc: 0,
            current_tf:  0,
            has_current: false,
            raw_mode:    false,
        }
    }

    pub fn open(&mut self, block: Arc<IndexBlock>, last_doc: u64) {
        self.block       = Some(block);
        self.raw_mode    = false;
        self.pos         = 0;
        self.end         = DATA_SIZE;
        self.current_doc = last_doc;
        self.current_tf  = 0;
        self.has_current = false;
    }

    pub fn open_raw(&mut self, data: Vec<u8>, last_doc: u64) {
        self.end         = data.len();
        self.raw_data    = data;
        self.block       = None;
        self.raw_mode    = true;
        self.pos         = 0;
        self.current_doc = last_doc;
        self.current_tf  = 0;
        self.has_current = false;
    }

    fn data_byte(&self, pos: usize) -> u8 {
        if self.raw_mode {
            self.raw_data[pos]
        } else {
            self.block.as_ref().unwrap().ib_data[pos]
        }
    }

    fn has_more_bytes(&self) -> bool {
        if self.pos >= self.end { return false; }
        if !self.raw_mode && self.data_byte(self.pos) == 0 { return false; }
        true
    }

    pub fn go_next(&mut self) {
        if !self.has_more_bytes() {
            self.has_current = false;
            return;
        }

        let mut delta = 0u64;
        let mut shift = 0u8;
        loop {
            let b = self.data_byte(self.pos); self.pos += 1;
            delta |= ((b & 0x7F) as u64) << shift;
            if (b & 0x80) == 0 { break; }
            shift += 7;
        }
        self.current_doc += delta;

        let mut tf    = 0u32;
        let mut shift = 0u8;
        loop {
            let b = self.data_byte(self.pos); self.pos += 1;
            tf |= ((b & 0x7F) as u32) << shift;
            if (b & 0x80) == 0 { break; }
            shift += 7;
        }
        self.current_tf  = tf;
        self.has_current = true;
    }

    pub fn go_until(&mut self, target: u64) {
        if !self.has_current && self.has_more_bytes() {
            self.go_next();
        }
        while self.has_current && self.current_doc < target {
            self.go_next();
        }
    }

    pub fn is_end(&self)             -> bool  { !self.has_current }
    pub fn get_document_id(&self)    -> u64   { self.current_doc }
    pub fn get_term_frequency(&self) -> u32   { self.current_tf }
    pub fn get_current_block(&self)  -> Option<&Arc<IndexBlock>> { self.block.as_ref() }
}
