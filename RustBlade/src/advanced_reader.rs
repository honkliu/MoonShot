use std::sync::Arc;

use crate::block_table::{
    IndexBlock,
    IndexBlockContinuationHeader,
    IndexBlockTable,
    INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
    PinnedBlock,
};
use crate::bm25::Bm25Scorer;
use crate::index_reader::{IndexReader, NO_MORE_DOCS, ReaderSourceMaskForStream};
use crate::varbyte_decoder::VarByteDecoder;

#[allow(non_snake_case)]
pub struct AdvancedIndexReader {
    #[allow(dead_code)]
    m_Word: String,
    m_BlockTable: Arc<IndexBlockTable>,
    m_DocFreq: u32,
    m_SourceMask: u8,
    m_BlockSeqNumber: u32,
    m_RemainingContinuationBlocks: u32,
    m_Decoder: VarByteDecoder,
}

#[allow(non_snake_case)]
impl AdvancedIndexReader {
    pub fn open(stream_key: &str, block_table: Arc<IndexBlockTable>, doc_freq: u32) -> Self {
        let mut reader = Self {
            m_Word: stream_key.to_string(),
            m_BlockTable: block_table,
            m_DocFreq: doc_freq,
            m_SourceMask: stream_key.chars().last().map(ReaderSourceMaskForStream).unwrap_or(0),
            m_BlockSeqNumber: 0,
            m_RemainingContinuationBlocks: 0,
            m_Decoder: VarByteDecoder::new(),
        };

        if let Some((location, block)) = reader.m_BlockTable.FindTermData(stream_key) {
            reader.m_BlockSeqNumber = location.index_block_id;
            reader.m_RemainingContinuationBlocks = location.continuation_block_count;
            reader.m_DocFreq = location.doc_freq;
            reader.m_Decoder.OpenRaw(block, location.index_offset, location.index_length, 0);
        }

        reader.GoNext();
        reader
    }

    fn LoadContinuation(&mut self) -> bool {
        let nextSeq = self.m_BlockSeqNumber + 1;
        let Some(block) = self.m_BlockTable.GetBlockBySeq(nextSeq) else { return false; };
        self.m_BlockSeqNumber = nextSeq;
        self.OpenContinuation(block);
        self.m_RemainingContinuationBlocks = self.m_RemainingContinuationBlocks.saturating_sub(1);
        true
    }

    fn OpenContinuation(&mut self, block: PinnedBlock<IndexBlock>) {
        if let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.IB_Data) {
            let len = header.IBCH_DataLength as usize;
            self.m_Decoder.OpenRaw(
                block,
                INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
                len,
                0);
        } else {
            self.m_Decoder.OpenRaw(block, 0, 0, 0);
        }
    }
}

impl IndexReader for AdvancedIndexReader {
    fn GoNext(&mut self) {
        self.m_Decoder.GoNext();
        while self.m_Decoder.IsEnd() && self.m_RemainingContinuationBlocks > 0 {
            if !self.LoadContinuation() { break; }
            self.m_Decoder.GoNext();
        }
    }

    fn GoUntil(&mut self, target: u64, limit: u64) {
        loop {
            self.m_Decoder.GoUntil(target);
            if !self.m_Decoder.IsEnd() || self.GetDocumentID() >= limit { break; }
            if self.m_RemainingContinuationBlocks == 0 { break; }
            let Some(block) = self.m_BlockTable.GetBlockBySeq(self.m_BlockSeqNumber + 1) else { break; };
            let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.IB_Data) else { break; };
            self.m_BlockSeqNumber += 1;
            self.m_RemainingContinuationBlocks = self.m_RemainingContinuationBlocks.saturating_sub(1);
            if target > header.IBCH_MaxDocID {
                continue;
            }
            self.OpenContinuation(block);
        }
    }

    fn IsEnd(&self) -> bool { self.m_Decoder.IsEnd() }

    fn GetDocumentID(&self) -> u64 {
        if self.m_Decoder.IsEnd() { NO_MORE_DOCS } else { self.m_Decoder.GetDocumentID() }
    }

    fn GetTermFreq(&self) -> u32 {
        if self.m_Decoder.IsEnd() { 0 } else { self.m_Decoder.GetTermFrequency() }
    }

    fn GetScore(&self, scorer: &Bm25Scorer, doc_len: u32) -> f32 {
        scorer.score(self.GetTermFreq(), doc_len, self.m_DocFreq)
    }

    fn GetSourceMask(&self) -> u8 { self.m_SourceMask }

    fn SetDebug(&mut self, _label: &str, _depth: usize) {}
}
