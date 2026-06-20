use std::sync::Arc;

use crate::block_table::{
    IndexBlock,
    IndexBlockContinuationHeader,
    IndexBlockTable,
    INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
    PinnedBlock,
};
use crate::bm25::Bm25Scorer;
use crate::index_reader::{IndexReader, NO_MORE_DOCS};
use crate::varbyte_decoder::VarByteDecoder;

pub struct AdvancedIndexReader {
    #[allow(dead_code)]
    stream_key: String,
    block_table: Arc<IndexBlockTable>,
    doc_freq: u32,
    block_seq: u32,
    remaining_continuation_blocks: u32,
    decoder: VarByteDecoder,
}

impl AdvancedIndexReader {
    pub fn open(stream_key: &str, block_table: Arc<IndexBlockTable>, doc_freq: u32) -> Self {
        let mut reader = Self {
            stream_key: stream_key.to_string(),
            block_table,
            doc_freq,
            block_seq: 0,
            remaining_continuation_blocks: 0,
            decoder: VarByteDecoder::new(),
        };

        if let Some((location, block)) = reader.block_table.find_term_data(stream_key) {
            reader.block_seq = location.index_block_id;
            reader.remaining_continuation_blocks = location.continuation_block_count;
            reader.doc_freq = location.doc_freq;
            reader.decoder.open_raw(block, location.index_offset, location.index_length, 0);
        }

        reader.go_next();
        reader
    }

    fn load_continuation(&mut self) -> bool {
        let next_seq = self.block_seq + 1;
        let Some(block) = self.block_table.get_block_by_seq(next_seq) else { return false; };
        self.block_seq = next_seq;
        self.open_continuation(block);
        self.remaining_continuation_blocks = self.remaining_continuation_blocks.saturating_sub(1);
        true
    }

    fn open_continuation(&mut self, block: PinnedBlock<IndexBlock>) {
        if let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.ib_data) {
            let len = header.ibch_data_length as usize;
            self.decoder.open_raw(
                block,
                INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
                len,
                0);
        } else {
            self.decoder.open_raw(block, 0, 0, 0);
        }
    }
}

impl IndexReader for AdvancedIndexReader {
    fn go_next(&mut self) {
        self.decoder.go_next();
        while self.decoder.is_end() && self.remaining_continuation_blocks > 0 {
            if !self.load_continuation() { break; }
            self.decoder.go_next();
        }
    }

    fn go_until(&mut self, target: u64) {
        loop {
            self.decoder.go_until(target);
            if !self.decoder.is_end() { break; }
            if self.remaining_continuation_blocks == 0 { break; }
            let Some(block) = self.block_table.get_block_by_seq(self.block_seq + 1) else { break; };
            let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.ib_data) else { break; };
            self.block_seq += 1;
            self.remaining_continuation_blocks = self.remaining_continuation_blocks.saturating_sub(1);
            if target > header.ibch_max_doc_id {
                continue;
            }
            self.open_continuation(block);
        }
    }

    fn is_end(&self) -> bool { self.decoder.is_end() }

    fn get_document_id(&self) -> u64 {
        if self.decoder.is_end() { NO_MORE_DOCS } else { self.decoder.get_document_id() }
    }

    fn get_term_freq(&self) -> u32 {
        if self.decoder.is_end() { 0 } else { self.decoder.get_term_frequency() }
    }

    fn get_bm25_score(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        scorer.score(self.get_term_freq(), doc_len, self.doc_freq)
    }

    fn set_debug(&mut self, _label: &str, _depth: usize) {}
}
