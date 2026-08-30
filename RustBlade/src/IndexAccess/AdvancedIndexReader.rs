use std::sync::Arc;

use crate::block_table::{
    BlockHandle,
    BlockKind,
    IndexBlock,
    IndexBlockContinuationHeader,
    IndexBlockTable,
    INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
};
use crate::block_table::DocDataEntry;
use crate::index_reader::{IndexReader, ReaderSourceMaskForStream};
use crate::unified_decoder::UnifiedDecoder;

#[allow(non_snake_case)]
pub struct AdvancedIndexReader {
    #[allow(dead_code)]
    m_Word: String,
    m_BlockTable: Arc<IndexBlockTable>,
    m_DocFreq: u32,
    m_Stream: char,
    m_SourceMask: u8,
    #[allow(dead_code)]
    m_WordSpan: u32,
    m_SpanWeight: f32,
    m_Idf: f32,
    m_Bm25LengthBias: f32,
    m_Bm25LengthScale: f32,
    m_BlockSeqNumber: u32,
    m_RemainingContinuationBlocks: u32,
    m_Decoder: UnifiedDecoder,
    m_Debug: bool,
    m_DebugDepth: usize,
}

#[allow(non_snake_case)]
impl AdvancedIndexReader {
    pub fn Open(
        stream_key: &str,
        block_table: Arc<IndexBlockTable>,
        num_documents: u64,
        average_stream_length: f32,
        span_weight: f32,
        word_span: u32,
    ) -> Self {
        let mut reader = Self {
            m_Word: stream_key.to_string(),
            m_BlockTable: block_table,
            m_DocFreq: 0,
            m_Stream: stream_key.chars().last().unwrap_or('B'),
            m_SourceMask: stream_key.chars().last().map(ReaderSourceMaskForStream).unwrap_or(0),
            m_WordSpan: word_span.max(1),
            m_SpanWeight: span_weight,
            m_Idf: 0.0,
            m_Bm25LengthBias: 0.0,
            m_Bm25LengthScale: 0.0,
            m_BlockSeqNumber: 0,
            m_RemainingContinuationBlocks: 0,
            m_Decoder: UnifiedDecoder::new(),
            m_Debug: false,
            m_DebugDepth: 0,
        };

        if let Some(location) = reader.m_BlockTable.FindTermData(stream_key) {
            reader.m_BlockSeqNumber = location.index_block_id;
            reader.m_RemainingContinuationBlocks = location.continuation_block_count;
            reader.m_DocFreq = location.doc_freq;
            const K1: f32 = 1.2;
            const B: f32 = 0.75;
            assert!(num_documents > 0);
            assert!(average_stream_length > 0.0);
            let totalDocs = num_documents as f32;
            let docFreq = reader.m_DocFreq.max(1) as f32;
            reader.m_Idf = (((totalDocs - docFreq + 0.5) / (docFreq + 0.5)) + 1.0).ln().max(0.0);
            reader.m_Bm25LengthBias = K1 * (1.0 - B);
            reader.m_Bm25LengthScale = K1 * B / average_stream_length.max(1.0);
            if let Some(block) = reader.m_BlockTable.GetBlock(BlockKind::Index, location.index_block_id, false) {
                reader.m_Decoder.OpenRaw(block, location.index_offset, location.index_length);
            }
        }

        reader.GoNext();
        reader
    }

    fn LoadContinuation(&mut self) -> bool {
        let nextSeq = self.m_BlockSeqNumber + 1;
        self.ReleaseCurrentBlock();
        let Some(block) = self.m_BlockTable.GetBlock(BlockKind::Index, nextSeq, false) else { return false; };
        self.m_BlockSeqNumber = nextSeq;
        self.OpenContinuation(block);
        self.m_RemainingContinuationBlocks = self.m_RemainingContinuationBlocks.saturating_sub(1);
        true
    }

    fn OpenContinuation(&mut self, block: BlockHandle<IndexBlock>) {
        if let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.IB_Data) {
            let len = header.IBCH_DataLength as usize;
            self.m_Decoder.OpenRaw(
                block,
                INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
                len);
        } else {
            self.m_Decoder.OpenRaw(block, 0, 0);
        }
    }

    fn ReleaseCurrentBlock(&mut self) {
        if let Some(block) = self.m_Decoder.TakeBlock() {
            self.m_BlockTable.ReleaseBlock(block.Kind(), block.Slot(), false);
        }
    }
}

impl Drop for AdvancedIndexReader {
    fn drop(&mut self) { self.ReleaseCurrentBlock(); }
}

impl IndexReader for AdvancedIndexReader {
    fn GoNext(&mut self) {
        self.m_Decoder.GoNext();
        while self.m_Decoder.IsEnd() && self.m_RemainingContinuationBlocks > 0 {
            if !self.LoadContinuation() { break; }
            self.m_Decoder.GoNext();
        }
        if self.m_Decoder.IsEnd() && self.m_RemainingContinuationBlocks == 0 {
            self.ReleaseCurrentBlock();
            self.m_Decoder.Close();
        }
    }

    fn GoUntil(&mut self, target: u64, _limit: u64) {
        loop {
            self.m_Decoder.GoUntil(target);
            if !self.m_Decoder.IsEnd() { break; }
            if self.m_RemainingContinuationBlocks == 0 {
                self.ReleaseCurrentBlock();
                break;
            }
            self.ReleaseCurrentBlock();
            let Some(block) = self.m_BlockTable.GetBlock(BlockKind::Index, self.m_BlockSeqNumber + 1, false) else { break; };
            let Some(header) = IndexBlockContinuationHeader::from_bytes(&block.IB_Data) else {
                self.m_BlockTable.ReleaseBlock(block.Kind(), block.Slot(), false);
                break;
            };
            self.m_BlockSeqNumber += 1;
            self.m_RemainingContinuationBlocks = self.m_RemainingContinuationBlocks.saturating_sub(1);
            if target > header.IBCH_MaxDocID {
                self.m_BlockTable.ReleaseBlock(block.Kind(), block.Slot(), false);
                continue;
            }
            self.OpenContinuation(block);
            self.m_Decoder.GoNext();
            if self.m_Decoder.IsEnd() {
                self.ReleaseCurrentBlock();
                break;
            }
        }
        if self.m_Decoder.IsEnd() && self.m_RemainingContinuationBlocks == 0 {
            self.ReleaseCurrentBlock();
            self.m_Decoder.Close();
        }
    }

    fn IsEnd(&self) -> bool { self.m_Decoder.IsEnd() }

    fn GetDocumentID(&self) -> u64 {
        self.m_Decoder.GetDocumentID()
    }

    fn GetTermFreq(&self) -> u32 {
        self.m_Decoder.GetTermFreq() as u32
    }

    fn GetScore(&mut self, entry: &DocDataEntry) -> f32 {
        const K1_PLUS_ONE: f32 = 2.2;
        let tf = self.GetTermFreq() as f32;
        let docLength = match self.m_Stream {
            'T' => entry.DDE_TitleLength,
            'B' => entry.DDE_BodyLength,
            'U' => entry.DDE_UrlLength,
            'A' => entry.DDE_AnchorLength,
            'M' => entry.DDE_MetaLength,
            _ => 0,
        };
        let docLength = if docLength > 0 { docLength } else { entry.DDE_BodyLength.max(1) } as f32;
        self.m_SpanWeight * self.m_Idf * tf * K1_PLUS_ONE /
            (tf + self.m_Bm25LengthBias + self.m_Bm25LengthScale * docLength)
    }

    fn GetSourceMask(&mut self) -> u8 { self.m_SourceMask }

    fn SetDebug(&mut self, _label: &str, depth: usize) {
        self.m_Debug = true;
        self.m_DebugDepth = depth;
        if !self.m_Decoder.IsEnd() {
            println!("{}[leaf] {:<12}  -{}-", " ".repeat(depth * 2), self.m_Word, self.m_Decoder.GetDocumentID());
        } else {
            println!("{}[leaf] {:<12}  (empty)", " ".repeat(depth * 2), self.m_Word);
        }
    }
    fn Close(&mut self) {
        self.ReleaseCurrentBlock();
        self.m_Decoder.Close();
        self.m_Word.clear();
    }
}
