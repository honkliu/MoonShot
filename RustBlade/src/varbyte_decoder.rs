use crate::block_table::{IndexBlock, PinnedBlock};

/// Stateful VarByte decoder.  Posting bytes store absolute (docID, tf) pairs.
/// Two modes:
///   OpenRaw — reads from an explicit byte slice (offset + len within a block).
#[allow(non_snake_case)]
pub struct VarByteDecoder {
    m_Block:       Option<PinnedBlock<IndexBlock>>,
    m_RawData:    Vec<u8>,
    m_CurrentPtr: usize,
    m_BlockEnd:   usize,
    m_RawMode:    bool,
    m_CurrentDoc: u64,
    m_CurrentTf:  u32,
    m_HasCurrent: bool,
}

#[allow(non_snake_case)]
impl VarByteDecoder {
    pub fn new() -> Self {
        Self {
            m_Block: None, m_RawData: Vec::new(),
            m_CurrentPtr: 0, m_BlockEnd: 0, m_RawMode: false,
            m_CurrentDoc: 0, m_CurrentTf: 0, m_HasCurrent: false,
        }
    }

    /// Open on the term's posting bytes within a block's IB_Data.
    /// `offset` and `len` are the byte range returned by IndexBlockTable::find_term_data.
    #[allow(non_snake_case)]
    pub fn OpenRaw(&mut self, block: PinnedBlock<IndexBlock>, offset: usize, len: usize, last_doc: u64) {
        let end = (offset + len).min(block.IB_Data.len());
        self.m_RawData    = block.IB_Data[offset..end].to_vec();
        self.m_Block       = Some(block);
        self.m_RawMode    = true;
        self.m_CurrentPtr         = 0;
        self.m_BlockEnd         = self.m_RawData.len();
        self.m_CurrentDoc = last_doc;
        self.m_CurrentTf  = 0;
        self.m_HasCurrent = false;
    }

    fn HasMoreBytes(&self) -> bool {
        self.m_CurrentPtr < self.m_BlockEnd
    }

    #[allow(non_snake_case)]
    pub fn GoNext(&mut self) {
        if !self.HasMoreBytes() {
            self.m_HasCurrent = false;
            return;
        }
        let (docID, n) = VbRead(&self.m_RawData, self.m_CurrentPtr);
        self.m_CurrentPtr += n;
        if self.m_CurrentPtr >= self.m_BlockEnd && n == 0 {
            self.m_HasCurrent = false;
            return;
        }
        let (tf, m) = VbRead(&self.m_RawData, self.m_CurrentPtr);
        self.m_CurrentPtr += m;

        self.m_CurrentDoc  = docID;
        self.m_CurrentTf   = tf as u32;
        self.m_HasCurrent  = true;
    }

    #[allow(non_snake_case)]
    pub fn GoUntil(&mut self, target: u64) {
        if !self.m_HasCurrent && self.HasMoreBytes() { self.GoNext(); }
        while self.m_HasCurrent && self.m_CurrentDoc < target { self.GoNext(); }
    }

    #[allow(non_snake_case)]
    pub fn IsEnd(&self)             -> bool  { !self.m_HasCurrent }
    #[allow(non_snake_case)]
    pub fn GetDocumentID(&self)    -> u64   { self.m_CurrentDoc }
    #[allow(non_snake_case)]
    pub fn GetTermFrequency(&self) -> u32   { self.m_CurrentTf }
    #[allow(non_snake_case)]
    pub fn GetCurrentBlock(&self)  -> Option<&PinnedBlock<IndexBlock>> { self.m_Block.as_ref() }
}

impl Default for VarByteDecoder {
    fn default() -> Self { Self::new() }
}

#[allow(non_snake_case)]
pub fn VbRead(data: &[u8], start: usize) -> (u64, usize) {
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

#[allow(non_snake_case)]
pub fn VbEncode(mut v: u64) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        if v < 0x80 { out.push(v as u8); break; }
        out.push((v as u8 & 0x7F) | 0x80);
        v >>= 7;
    }
    out
}
