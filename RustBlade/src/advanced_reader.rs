use std::sync::Arc;
use crate::block_table::{IndexBlockTable, CONT_MARKER};
use crate::index_reader::{IndexReader, NO_MORE_DOCS};
use crate::bm25::Bm25Scorer;
use crate::varbyte_decoder::VarByteDecoder;

/// Leaf ISR with PageSkipList support for O(log P) GoUntil across blocks.
pub struct AdvancedIndexReader {
    #[allow(dead_code)]
    stream_key:       String,
    block_table:      Arc<IndexBlockTable>,
    doc_freq:         u32,
    block_seq:        u32,
    initial_block_seq: u32,   // first block of this term
    total_continuation_blocks: u32,
    remaining_continuation_blocks: u32,
    page_skip_offset: u32,    // 0 = single-block term
    decoder:          VarByteDecoder,
}

impl AdvancedIndexReader {
    pub fn open(stream_key: &str, block_table: Arc<IndexBlockTable>, doc_freq: u32) -> Self {
        let mut reader = Self {
            stream_key: stream_key.to_string(),
            block_table,
            doc_freq,
            block_seq: 0,
            initial_block_seq: 0,
            total_continuation_blocks: 0,
            remaining_continuation_blocks: 0,
            page_skip_offset: 0,
            decoder: VarByteDecoder::new(),
        };

        if let Some((loc, block)) = reader.block_table.find_term_data(stream_key) {
            reader.block_seq         = loc.index_block_id;
            reader.initial_block_seq = loc.index_block_id;
            reader.total_continuation_blocks = loc.continuation_block_count;
            reader.remaining_continuation_blocks = loc.continuation_block_count;
            reader.page_skip_offset  = loc.page_skip_offset;
            /* Use doc_freq from LeafTermEntry — correct after load(), PostingStore has none */
            reader.doc_freq          = loc.doc_freq;
            reader.decoder.open_raw(block, loc.index_offset, loc.index_length, 0);
        }

        reader.go_next();
        reader
    }

    fn load_continuation(&mut self, seq: u32) -> bool {
        if let Some(block) = self.block_table.get_block_by_seq(seq) {
            let last_doc = self.decoder.get_document_id();
            self.block_seq = seq;
            self.open_continuation(block, last_doc);
            self.remaining_continuation_blocks = self.remaining_continuation_blocks.saturating_sub(1);
            true
        } else { false }
    }

    /// Open decoder on a continuation block with the new cont_len format.
    fn open_continuation(&mut self, block: Arc<crate::block_table::IndexBlock>, last_doc: u64) {
        let d = &block.ib_data;
        if d.len() >= 4 && u16::from_le_bytes([d[0], d[1]]) == CONT_MARKER {
            let cont_len = u16::from_le_bytes([d[2], d[3]]) as usize;
            let end      = (4 + cont_len).min(d.len());
            self.decoder.open_raw(block.clone(), 4, end - 4, last_doc);
        } else {
            // fallback: whole ib_data is continuation (old-format blocks)
            self.decoder.open_raw(block.clone(), 0, d.len(), last_doc);
        }
    }
}

impl IndexReader for AdvancedIndexReader {
    fn go_next(&mut self) {
        if self.decoder.is_end() && self.remaining_continuation_blocks > 0 {
            self.load_continuation(self.block_seq + 1);
        }
        self.decoder.go_next();
    }

    fn go_until(&mut self, target: u64) {
        // Fast path: use PageSkipList to jump to the right block
        if self.page_skip_offset > 0 && self.remaining_continuation_blocks > 0 {
            if let Some(skip) = self.block_table.get_page_skip_ptr(self.page_skip_offset) {
                let cur_idx = (self.block_seq - self.initial_block_seq) as usize;
                let mut tgt_idx = cur_idx;
                while tgt_idx + 1 < skip.len()
                    && skip[tgt_idx + 1] != u64::MAX
                    && skip[tgt_idx + 1] <= target
                {
                    tgt_idx += 1;
                }
                if tgt_idx > cur_idx {
                    let target_block = self.initial_block_seq + tgt_idx as u32;
                    let base_loc     = skip[tgt_idx];
                    if let Some(block) = self.block_table.get_block_by_seq(target_block) {
                        self.block_seq = target_block;
                        self.remaining_continuation_blocks = self.total_continuation_blocks.saturating_sub(tgt_idx as u32);
                        self.open_continuation(block, base_loc);
                        self.decoder.go_next();
                        if !self.decoder.is_end() {
                            self.decoder.go_until(target);
                            if !self.decoder.is_end() { return; }
                        }
                    }
                }
            }
        }

        // Sequential fallback
        loop {
            self.decoder.go_until(target);
            if !self.decoder.is_end() { break; }
            if self.remaining_continuation_blocks == 0 { break; }
            if !self.load_continuation(self.block_seq + 1) { break; }
            self.decoder.go_next();
            if self.decoder.is_end()   { break; }
        }
    }

    fn is_end(&self)          -> bool { self.decoder.is_end() }
    fn get_document_id(&self) -> u64  {
        if self.decoder.is_end() { NO_MORE_DOCS } else { self.decoder.get_document_id() }
    }
    fn get_term_freq(&self)   -> u32  {
        if self.decoder.is_end() { 0 } else { self.decoder.get_term_frequency() }
    }
    fn get_bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        scorer.score(self.get_term_freq(), doc_len, self.doc_freq)
    }
    fn set_debug(&mut self, _label: &str, _depth: usize) {}
}
