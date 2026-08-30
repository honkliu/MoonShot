//! Direct translation of the C++ unified decoder; symbol names stay aligned for debugging.
#![allow(non_snake_case, non_upper_case_globals)]

use crate::block_table::{BlockHandle, IndexBlock};

/// Stateful VarByte decoder.  Posting bytes store absolute (docID, tf) pairs.
/// Two modes:
///   OpenRaw — reads from an explicit byte slice (offset + len within a block).
#[allow(non_snake_case)]
pub struct UnifiedDecoder {
    m_Block: Option<BlockHandle<IndexBlock>>,
    m_CurrentPtr: usize,
    m_BlockEnd: usize,
    m_CurrentDoc: u64,
    m_CurrentTf8: u8,
    m_HasCurrent: bool,
}

#[allow(non_snake_case)]
impl UnifiedDecoder {
    pub fn new() -> Self {
        Self {
            m_Block: None,
            m_CurrentPtr: 0,
            m_BlockEnd: 0,
            m_CurrentDoc: 0,
            m_CurrentTf8: 0,
            m_HasCurrent: false,
        }
    }

    /// Open on the term's posting bytes within a block's IB_Data.
    /// `offset` and `len` are the byte range returned by IndexBlockTable::find_term_data.
    #[allow(non_snake_case)]
    pub fn OpenRaw(&mut self, block: BlockHandle<IndexBlock>, offset: usize, len: usize) {
        // Malformed posting ranges terminate as empty instead of wrapping or
        // indexing out of bounds as an unchecked native decoder could.
        let end = offset
            .checked_add(len)
            .filter(|end| *end <= block.IB_Data.len())
            .unwrap_or(0);
        self.m_Block = Some(block);
        self.m_CurrentPtr = offset.min(end);
        self.m_BlockEnd = end;
        self.m_CurrentDoc = 0;
        self.m_CurrentTf8 = 0;
        self.m_HasCurrent = false;
    }

    fn HasMoreBytes(&self) -> bool {
        self.m_CurrentPtr < self.m_BlockEnd
    }

    #[allow(non_snake_case)]
    pub fn GoNext(&mut self) {
        self.DecodeNext();
    }

    fn DecodeNext(&mut self) {
        if !self.HasMoreBytes() {
            self.m_HasCurrent = false;
            return;
        }
        let Some(block) = self.m_Block.as_ref() else {
            self.m_HasCurrent = false;
            return;
        };
        let mut byte = block.IB_Data[self.m_CurrentPtr];
        self.m_CurrentPtr += 1;
        let mut docID = (byte & 0x7f) as u64;
        let mut shift = 7u32;
        while byte & 0x80 != 0 {
            if self.m_CurrentPtr >= self.m_BlockEnd || shift >= 64 {
                self.m_HasCurrent = false;
                return;
            }
            byte = block.IB_Data[self.m_CurrentPtr];
            self.m_CurrentPtr += 1;
            if shift == 63 && byte & 0x7e != 0 {
                self.m_HasCurrent = false;
                return;
            }
            docID |= ((byte & 0x7f) as u64) << shift;
            shift += 7;
        }
        self.m_CurrentDoc = docID;
        if self.m_CurrentPtr >= self.m_BlockEnd {
            self.m_HasCurrent = false;
            return;
        }
        let tf = block.IB_Data[self.m_CurrentPtr];
        self.m_CurrentPtr += 1;

        self.m_CurrentTf8 = tf;
        self.m_HasCurrent = true;
    }

    #[allow(non_snake_case)]
    pub fn GoUntil(&mut self, target: u64) {
        if self.m_HasCurrent && self.m_CurrentDoc >= target {
            return;
        }
        loop {
            self.DecodeNext();
            if !self.m_HasCurrent || self.m_CurrentDoc >= target {
                return;
            }
        }
    }

    #[allow(non_snake_case)]
    pub fn IsEnd(&self) -> bool {
        !self.m_HasCurrent
    }
    #[allow(non_snake_case)]
    pub fn GetDocumentID(&self) -> u64 {
        self.m_CurrentDoc
    }
    #[allow(non_snake_case)]
    pub fn GetTermFreq(&self) -> u8 {
        self.m_CurrentTf8
    }
    #[allow(non_snake_case)]
    pub fn GetCurrentBlock(&self) -> Option<&BlockHandle<IndexBlock>> {
        self.m_Block.as_ref()
    }
    pub fn TakeBlock(&mut self) -> Option<BlockHandle<IndexBlock>> {
        self.m_Block.take()
    }
    #[allow(non_snake_case)]
    pub fn Close(&mut self) {
        self.m_Block = None;
        self.m_CurrentPtr = 0;
        self.m_BlockEnd = 0;
        self.m_HasCurrent = false;
    }
}

impl Default for UnifiedDecoder {
    fn default() -> Self {
        Self::new()
    }
}
