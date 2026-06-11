use std::sync::Arc;
use crate::block_table::IndexBlockTable;
use crate::index_reader::{IndexReader, NO_MORE_DOCS};
use crate::bm25::Bm25Scorer;
use crate::varbyte_decoder::VarByteDecoder;

/*
* AdvancedIndexReader — the leaf ISR.
*
* Reads one (term + stream) posting list from IndexBlockTable using
* VarByteDecoder, spanning multiple blocks via IB_HEADER_HAS_MORE.
*
* Equivalent to REF's ISRWord backed by a PageManager.
* TF and BM25 score live here only; composite readers aggregate from leaves.
*/
pub struct AdvancedIndexReader {
    stream_key:    String,
    block_table:   Arc<IndexBlockTable>,
    doc_freq:      u32,
    block_seq:     u32,
    decoder:       VarByteDecoder,
    debug:         bool,
    debug_depth:   usize,
}

impl AdvancedIndexReader {
    pub fn open(
        stream_key:  &str,
        block_table: Arc<IndexBlockTable>,
        doc_freq:    u32,
    ) -> Self {
        let mut reader = Self {
            stream_key:  stream_key.to_string(),
            block_table,
            doc_freq,
            block_seq:   0,
            decoder:     VarByteDecoder::new(),
            debug:       false,
            debug_depth: 0,
        };

        if let Some(block) = reader.block_table.get_block_by_term(stream_key) {
            reader.block_seq = block.block_seq();
            reader.decoder.open(Arc::clone(&block), 0);
        }

        reader.go_next();
        reader
    }

    fn has_more_blocks(&self) -> bool {
        self.decoder.get_current_block()
            .map(|b| b.has_more())
            .unwrap_or(false)
    }

    fn load_next_block(&mut self) -> bool {
        if let Some(block) = self.block_table.get_block_by_seq(self.block_seq + 1) {
            let last_doc = self.decoder.get_document_id();
            self.block_seq += 1;
            self.decoder.open(Arc::clone(&block), last_doc);
            true
        } else {
            false
        }
    }
}

impl IndexReader for AdvancedIndexReader {
    fn go_next(&mut self) {
        if self.decoder.is_end() && self.has_more_blocks() {
            self.load_next_block();
        }
        self.decoder.go_next();

        if self.debug && !self.decoder.is_end() {
            let ind = " ".repeat(self.debug_depth * 2);
            println!("{}{:<12}  -{}-", ind, self.stream_key, self.decoder.get_document_id());
        }
    }

    fn go_until(&mut self, target: u64) {
        loop {
            self.decoder.go_until(target);
            if !self.decoder.is_end() { break; }
            if !self.has_more_blocks() { break; }
            if !self.load_next_block() { break; }
            self.decoder.go_next();
            if self.decoder.is_end() { break; }
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

    fn set_debug(&mut self, _label: &str, depth: usize) {
        self.debug       = true;
        self.debug_depth = depth;
        let ind = " ".repeat(depth * 2);
        if !self.decoder.is_end() {
            println!("{}[leaf] {:<12}  -{}-", ind, self.stream_key, self.decoder.get_document_id());
        } else {
            println!("{}[leaf] {:<12}  (empty)", ind, self.stream_key);
        }
    }
}
