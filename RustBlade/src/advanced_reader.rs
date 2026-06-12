use std::sync::Arc;
use crate::block_table::IndexBlockTable;
use crate::index_reader::{IndexReader, NO_MORE_DOCS};
use crate::bm25::Bm25Scorer;
use crate::varbyte_decoder::VarByteDecoder;

/// Leaf ISR — reads one (term+stream) posting list via SubIndex binary search
/// and in-block scan.  Mirrors MoonShot's AdvancedIndexReader.
pub struct AdvancedIndexReader {
    #[allow(dead_code)]
    stream_key:  String,
    block_table: Arc<IndexBlockTable>,
    doc_freq:    u32,
    block_seq:   u32,
    has_more:    bool,   // true only when THIS term's data continues in block_seq+1
    decoder:     VarByteDecoder,
}

impl AdvancedIndexReader {
    pub fn open(
        stream_key:  &str,
        block_table: Arc<IndexBlockTable>,
        doc_freq:    u32,
    ) -> Self {
        let mut reader = Self {
            stream_key: stream_key.to_string(),
            block_table,
            doc_freq,
            block_seq: 0,
            has_more:  false,
            decoder:   VarByteDecoder::new(),
        };

        if let Some((loc, block)) = reader.block_table.find_term_data(stream_key) {
            reader.block_seq = loc.block_seq;
            reader.has_more  = loc.is_last_entry && block.has_more();
            reader.decoder.open_raw(block, loc.data_offset, loc.data_len, 0);
        }

        reader.go_next();
        reader
    }

    fn load_next_block(&mut self) -> bool {
        if let Some(block) = self.block_table.get_block_by_seq(self.block_seq + 1) {
            let last_doc = self.decoder.get_document_id();
            self.block_seq += 1;
            self.has_more   = block.has_more();   // continuation chains further if set
            self.decoder.open_continuation(block, last_doc);
            true
        } else {
            false
        }
    }
}

impl IndexReader for AdvancedIndexReader {
    fn go_next(&mut self) {
        if self.decoder.is_end() && self.has_more {
            self.load_next_block();
        }
        self.decoder.go_next();
    }

    fn go_until(&mut self, target: u64) {
        loop {
            self.decoder.go_until(target);
            if !self.decoder.is_end() { break; }
            if !self.has_more         { break; }
            if !self.load_next_block() { break; }
            self.decoder.go_next();
            if self.decoder.is_end()  { break; }
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
