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

        if let Some((location, block)) = reader.block_table.FindTermData(stream_key) {
            reader.block_seq = location.index_block_id;
            reader.remaining_continuation_blocks = location.continuation_block_count;
            reader.doc_freq = location.doc_freq;
            reader.decoder.OpenRaw(block, location.index_offset, location.index_length, 0);
        }

        reader.GoNext();
        reader
    }

    fn load_continuation(&mut self) -> bool {
        let next_seq = self.block_seq + 1;
        let Some(block) = self.block_table.GetBlockBySeq(next_seq) else { return false; };
        self.block_seq = next_seq;
        self.open_continuation(block);
        self.remaining_continuation_blocks = self.remaining_continuation_blocks.saturating_sub(1);
        true
    }

    fn open_continuation(&mut self, block: PinnedBlock<IndexBlock>) {
        if let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.ib_data) {
            let len = header.ibch_data_length as usize;
            self.decoder.OpenRaw(
                block,
                INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
                len,
                0);
        } else {
            self.decoder.OpenRaw(block, 0, 0, 0);
        }
    }
}

impl IndexReader for AdvancedIndexReader {
    fn GoNext(&mut self) {
        self.decoder.GoNext();
        while self.decoder.IsEnd() && self.remaining_continuation_blocks > 0 {
            if !self.load_continuation() { break; }
            self.decoder.GoNext();
        }
    }

    fn GoUntil(&mut self, target: u64) {
        loop {
            self.decoder.GoUntil(target);
            if !self.decoder.IsEnd() { break; }
            if self.remaining_continuation_blocks == 0 { break; }
            let Some(block) = self.block_table.GetBlockBySeq(self.block_seq + 1) else { break; };
            let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.ib_data) else { break; };
            self.block_seq += 1;
            self.remaining_continuation_blocks = self.remaining_continuation_blocks.saturating_sub(1);
            if target > header.ibch_max_doc_id {
                continue;
            }
            self.open_continuation(block);
        }
    }

    fn IsEnd(&self) -> bool { self.decoder.IsEnd() }

    fn GetDocumentID(&self) -> u64 {
        if self.decoder.IsEnd() { NO_MORE_DOCS } else { self.decoder.GetDocumentID() }
    }

    fn GetTermFreq(&self) -> u32 {
        if self.decoder.IsEnd() { 0 } else { self.decoder.GetTermFrequency() }
    }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        scorer.score(self.GetTermFreq(), doc_len, self.doc_freq)
    }

    fn SetDebug(&mut self, _label: &str, _depth: usize) {}
}
