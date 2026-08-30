#![allow(non_snake_case)]

use std::marker::PhantomData;
use std::ops::Deref;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::collections::VecDeque;
#[cfg(not(target_arch = "wasm32"))]
use std::thread::{self, JoinHandle};

use crate::file_access::FileAccess;
use crate::mem_operation::PinnedMemory;
use crate::error::{Result, RustBladeError};
use crate::element_filter::ElementFilter;

pub const PAGE_SIZE: usize = 4096;
pub const DOC_REC_SIZE: usize = 256;
pub const DOC_VECTOR_DIM: usize = 128;
pub const DOC_VECTOR_STORAGE_MAX_DIM: usize = DOC_VECTOR_DIM;
pub const DOC_PATH_MAX: usize = 64;
pub const DOC_PATH_PREFIX_ID_BYTES: usize = 2;
pub const DOC_PATH_FILENAME_MAX: usize = DOC_PATH_MAX - DOC_PATH_PREFIX_ID_BYTES;
pub const DOC_PATH_PREFIX_INVALID: u16 = u16::MAX;
pub const HEAD_TERM_KEY_MAX: usize = 26;
pub const LEAF_TERM_DIRECTORY_COUNT: usize = 161;
pub const LEAF_TERM_DATA_OFFSET: usize = LEAF_TERM_DIRECTORY_COUNT * std::mem::size_of::<u16>();
pub const PATH_PREFIX_SIDECAR_PAGE_COUNT: usize = 20;
pub const PATH_PREFIX_SIDECAR_BYTES: usize = PATH_PREFIX_SIDECAR_PAGE_COUNT * PAGE_SIZE;
pub const PATH_PREFIX_SIDECAR_MAGIC: &[u8; 8] = b"MSPATHS\0";
pub const PATH_PREFIX_SIDECAR_VERSION: u16 = 1;
pub const INDEX_FILE_MAGIC: &[u8; 8] = b"MOONSHOT";
pub const INDEX_FILE_HEADER_SIZE: usize = 136;
pub const INDEX_FORMAT_VERSION: u32 = 20;
pub const INDEX_BLOCK_CONTINUATION_HEADER_SIZE: usize = 12;
pub const TERM_MPHF_MAGIC: u64 = 0x4850464d4d524554;
pub const TERM_MPHF_HEADER_SIZE: usize = 48;
pub const LEAF_TERM_ENTRY_SIZE: usize = 16;
pub const TERM_MPHF_ENTRY_SIZE: usize = 32;
pub const TERM_MPHF_ENTRIES_PER_PAGE: usize = PAGE_SIZE / TERM_MPHF_ENTRY_SIZE;
pub const DOC_VECTOR_OFFSET: usize = 64;
pub const DOC_PATH_OFFSET: usize = 192;
pub const INDEX_BLOCK_CACHE_BYTES: u64 = 100 * 1024 * 1024;
pub const LEAF_TERM_CACHE_BYTES: u64 = 100 * 1024 * 1024;
const BLOCK_REQUEST_RING_SIZE: usize = 1usize << 16;

#[derive(Debug, Clone, Copy)]
#[allow(non_snake_case)]
pub struct IndexFileHeader {
    pub IFH_Magic: [u8; 8],
    pub IFH_Version: u32,
    pub IFH_AvgDocLength: f32,
    pub IFH_NumDocuments: u64,
    pub IFH_NumTerms: u64,
    pub IFH_HeadTermEntryOffset: u64,
    pub IFH_HeadTermEntryCount: u64,
    pub IFH_LeafTermBlockOffset: u64,
    pub IFH_LeafTermBlockCount: u64,
    pub IFH_DocDataOffset: u64,
    pub IFH_IndexBlockOffset: u64,
    pub IFH_IndexBlockCount: u64,
    pub IFH_TermMphfHeaderOffset: u64,
    pub IFH_TermMphfHeaderCount: u64,
    pub IFH_TermMphfDisplacementOffset: u64,
    pub IFH_TermMphfDisplacementCount: u64,
    pub IFH_TermMphfEntryOffset: u64,
    pub IFH_TermMphfEntryPageCount: u64,
}

impl Default for IndexFileHeader {
    fn default() -> Self {
        Self {
            IFH_Magic: *INDEX_FILE_MAGIC,
            IFH_Version: INDEX_FORMAT_VERSION,
            IFH_AvgDocLength: 0.0,
            IFH_NumDocuments: 0,
            IFH_NumTerms: 0,
            IFH_HeadTermEntryOffset: 0,
            IFH_HeadTermEntryCount: 0,
            IFH_LeafTermBlockOffset: 0,
            IFH_LeafTermBlockCount: 0,
            IFH_DocDataOffset: 0,
            IFH_IndexBlockOffset: 0,
            IFH_IndexBlockCount: 0,
            IFH_TermMphfHeaderOffset: 0,
            IFH_TermMphfHeaderCount: 0,
            IFH_TermMphfDisplacementOffset: 0,
            IFH_TermMphfDisplacementCount: 0,
            IFH_TermMphfEntryOffset: 0,
            IFH_TermMphfEntryPageCount: 0,
        }
    }
}

impl IndexFileHeader {
    pub fn zeroed() -> Self {
        Self {
            IFH_Magic: [0; 8],
            IFH_Version: 0,
            IFH_AvgDocLength: 0.0,
            IFH_NumDocuments: 0,
            IFH_NumTerms: 0,
            IFH_HeadTermEntryOffset: 0,
            IFH_HeadTermEntryCount: 0,
            IFH_LeafTermBlockOffset: 0,
            IFH_LeafTermBlockCount: 0,
            IFH_DocDataOffset: 0,
            IFH_IndexBlockOffset: 0,
            IFH_IndexBlockCount: 0,
            IFH_TermMphfHeaderOffset: 0,
            IFH_TermMphfHeaderCount: 0,
            IFH_TermMphfDisplacementOffset: 0,
            IFH_TermMphfDisplacementCount: 0,
            IFH_TermMphfEntryOffset: 0,
            IFH_TermMphfEntryPageCount: 0,
        }
    }

    pub fn is_zeroed(&self) -> bool {
        self.to_bytes().iter().all(|byte| *byte == 0)
    }

    pub fn parse(data: &[u8]) -> Result<Self> {
        if data.len() < INDEX_FILE_HEADER_SIZE || &data[0..8] != INDEX_FILE_MAGIC {
            return Err(RustBladeError::InvalidFormat);
        }
        if u32::from_le_bytes(data[8..12].try_into().unwrap()) != INDEX_FORMAT_VERSION {
            return Err(RustBladeError::InvalidFormat);
        }
        Ok(Self {
            IFH_Magic: data[0..8].try_into().unwrap(),
            IFH_Version: u32::from_le_bytes(data[8..12].try_into().unwrap()),
            IFH_AvgDocLength: f32::from_le_bytes(data[12..16].try_into().unwrap()),
            IFH_NumDocuments: u64::from_le_bytes(data[16..24].try_into().unwrap()),
            IFH_NumTerms: u64::from_le_bytes(data[24..32].try_into().unwrap()),
            IFH_HeadTermEntryOffset: u64::from_le_bytes(data[32..40].try_into().unwrap()),
            IFH_HeadTermEntryCount: u64::from_le_bytes(data[40..48].try_into().unwrap()),
            IFH_LeafTermBlockOffset: u64::from_le_bytes(data[48..56].try_into().unwrap()),
            IFH_LeafTermBlockCount: u64::from_le_bytes(data[56..64].try_into().unwrap()),
            IFH_DocDataOffset: u64::from_le_bytes(data[64..72].try_into().unwrap()),
            IFH_IndexBlockOffset: u64::from_le_bytes(data[72..80].try_into().unwrap()),
            IFH_IndexBlockCount: u64::from_le_bytes(data[80..88].try_into().unwrap()),
            IFH_TermMphfHeaderOffset: u64::from_le_bytes(data[88..96].try_into().unwrap()),
            IFH_TermMphfHeaderCount: u64::from_le_bytes(data[96..104].try_into().unwrap()),
            IFH_TermMphfDisplacementOffset: u64::from_le_bytes(data[104..112].try_into().unwrap()),
            IFH_TermMphfDisplacementCount: u64::from_le_bytes(data[112..120].try_into().unwrap()),
            IFH_TermMphfEntryOffset: u64::from_le_bytes(data[120..128].try_into().unwrap()),
            IFH_TermMphfEntryPageCount: u64::from_le_bytes(data[128..136].try_into().unwrap()),
        })
    }

    pub fn to_bytes(&self) -> [u8; INDEX_FILE_HEADER_SIZE] {
        let mut out = [0u8; INDEX_FILE_HEADER_SIZE];
        out[0..8].copy_from_slice(&self.IFH_Magic);
        out[8..12].copy_from_slice(&self.IFH_Version.to_le_bytes());
        out[12..16].copy_from_slice(&self.IFH_AvgDocLength.to_le_bytes());
        for (index, value) in [
            self.IFH_NumDocuments, self.IFH_NumTerms,
            self.IFH_HeadTermEntryOffset, self.IFH_HeadTermEntryCount,
            self.IFH_LeafTermBlockOffset, self.IFH_LeafTermBlockCount,
            self.IFH_DocDataOffset, self.IFH_IndexBlockOffset, self.IFH_IndexBlockCount,
            self.IFH_TermMphfHeaderOffset, self.IFH_TermMphfHeaderCount,
            self.IFH_TermMphfDisplacementOffset, self.IFH_TermMphfDisplacementCount,
            self.IFH_TermMphfEntryOffset, self.IFH_TermMphfEntryPageCount,
        ].into_iter().enumerate() {
            let begin = 16 + index * 8;
            out[begin..begin + 8].copy_from_slice(&value.to_le_bytes());
        }
        out
    }

    pub fn validate_layout(&self, file_bytes: Option<u64>) -> Result<u64> {
        // Keep strict checked layout validation even where the C++ loader may
        // otherwise reach out-of-bounds data; reproducing UB is not parity.
        if self.IFH_TermMphfHeaderCount > 1
            || self.IFH_HeadTermEntryCount > u32::MAX as u64
            || self.IFH_LeafTermBlockCount > u32::MAX as u64
            || self.IFH_IndexBlockCount > u32::MAX as u64
            || self.IFH_TermMphfDisplacementCount > u32::MAX as u64
            || self.IFH_TermMphfEntryPageCount > u32::MAX as u64
            || (self.IFH_TermMphfHeaderCount == 0
                && (self.IFH_TermMphfDisplacementCount != 0 || self.IFH_TermMphfEntryPageCount != 0))
            || (self.IFH_TermMphfHeaderCount == 1
                && (self.IFH_TermMphfDisplacementCount == 0 || self.IFH_TermMphfEntryPageCount == 0))
        {
            return Err(RustBladeError::InvalidFormat);
        }
        let checked_end = |offset: u64, count: u64, width: u64| {
            count.checked_mul(width).and_then(|bytes| offset.checked_add(bytes))
        };
        let head = (INDEX_FILE_HEADER_SIZE + PATH_PREFIX_SIDECAR_BYTES) as u64;
        if self.IFH_HeadTermEntryOffset != head { return Err(RustBladeError::InvalidFormat); }
        let leaf = checked_end(head, self.IFH_HeadTermEntryCount, 32).ok_or(RustBladeError::InvalidFormat)?;
        if self.IFH_LeafTermBlockOffset != leaf { return Err(RustBladeError::InvalidFormat); }
        let docdata = checked_end(leaf, self.IFH_LeafTermBlockCount, PAGE_SIZE as u64).ok_or(RustBladeError::InvalidFormat)?;
        if self.IFH_DocDataOffset != docdata { return Err(RustBladeError::InvalidFormat); }
        let index = checked_end(docdata, self.IFH_NumDocuments, DOC_REC_SIZE as u64).ok_or(RustBladeError::InvalidFormat)?;
        if self.IFH_IndexBlockOffset != index { return Err(RustBladeError::InvalidFormat); }
        let mphf_header = checked_end(index, self.IFH_IndexBlockCount, PAGE_SIZE as u64).ok_or(RustBladeError::InvalidFormat)?;
        if self.IFH_TermMphfHeaderOffset != mphf_header { return Err(RustBladeError::InvalidFormat); }
        let displacement = checked_end(mphf_header, self.IFH_TermMphfHeaderCount, TERM_MPHF_HEADER_SIZE as u64).ok_or(RustBladeError::InvalidFormat)?;
        if self.IFH_TermMphfDisplacementOffset != displacement { return Err(RustBladeError::InvalidFormat); }
        let entries = checked_end(displacement, self.IFH_TermMphfDisplacementCount, 4).ok_or(RustBladeError::InvalidFormat)?;
        if self.IFH_TermMphfEntryOffset != entries { return Err(RustBladeError::InvalidFormat); }
        let end = checked_end(entries, self.IFH_TermMphfEntryPageCount, PAGE_SIZE as u64).ok_or(RustBladeError::InvalidFormat)?;
        if file_bytes.map(|bytes| end > bytes).unwrap_or(false) { return Err(RustBladeError::InvalidFormat); }
        Ok(end)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlockKind { Index, LeafTerm }

pub fn TermMphfHash(term: &[u8], seed: u64) -> u64 {
    let mut hash = 1469598103934665603u64 ^ seed;
    for byte in term {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash ^= hash >> 32;
    hash = hash.wrapping_mul(0xd6e8feb86659fd93);
    hash ^= hash >> 32;
    hash
}

pub fn TermMphfSlotSeed(seed: u64, displacement: u32) -> u64 {
    let mut x = seed ^ 0x9e3779b97f4a7c15u64.wrapping_mul(displacement as u64 + 1);
    x ^= x >> 30;
    x = x.wrapping_mul(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x = x.wrapping_mul(0x94d049bb133111eb);
    x ^= x >> 31;
    x
}

#[derive(Debug, Clone, Copy)]
pub struct TermMphfHeader {
    pub TMH_Magic: u64,
    pub TMH_TermCount: u64,
    pub TMH_BucketCount: u32,
    pub TMH_SlotCount: u32,
    pub TMH_BucketSeed: u64,
    pub TMH_SlotSeed: u64,
    pub TMH_FingerprintSeed: u64,
}

impl Default for TermMphfHeader {
    fn default() -> Self {
        Self {
            TMH_Magic: TERM_MPHF_MAGIC,
            TMH_TermCount: 0,
            TMH_BucketCount: 0,
            TMH_SlotCount: 0,
            TMH_BucketSeed: 0,
            TMH_SlotSeed: 0,
            TMH_FingerprintSeed: 0,
        }
    }
}

impl TermMphfHeader {
    pub fn to_bytes(&self) -> [u8; TERM_MPHF_HEADER_SIZE] {
        let mut out = [0u8; TERM_MPHF_HEADER_SIZE];
        out[0..8].copy_from_slice(&self.TMH_Magic.to_le_bytes());
        out[8..16].copy_from_slice(&self.TMH_TermCount.to_le_bytes());
        out[16..20].copy_from_slice(&self.TMH_BucketCount.to_le_bytes());
        out[20..24].copy_from_slice(&self.TMH_SlotCount.to_le_bytes());
        out[24..32].copy_from_slice(&self.TMH_BucketSeed.to_le_bytes());
        out[32..40].copy_from_slice(&self.TMH_SlotSeed.to_le_bytes());
        out[40..48].copy_from_slice(&self.TMH_FingerprintSeed.to_le_bytes());
        out
    }

    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < TERM_MPHF_HEADER_SIZE { return None; }
        Some(Self {
            TMH_Magic: u64::from_le_bytes(data[0..8].try_into().ok()?),
            TMH_TermCount: u64::from_le_bytes(data[8..16].try_into().ok()?),
            TMH_BucketCount: u32::from_le_bytes(data[16..20].try_into().ok()?),
            TMH_SlotCount: u32::from_le_bytes(data[20..24].try_into().ok()?),
            TMH_BucketSeed: u64::from_le_bytes(data[24..32].try_into().ok()?),
            TMH_SlotSeed: u64::from_le_bytes(data[32..40].try_into().ok()?),
            TMH_FingerprintSeed: u64::from_le_bytes(data[40..48].try_into().ok()?),
        })
    }
}

#[derive(Debug, Clone, Copy)]
pub struct PathPrefixSidecarHeader {
    pub PPSH_Magic: [u8; 8],
    pub PPSH_Version: u16,
    pub PPSH_PrefixCount: u16,
    pub PPSH_EntryOffset: u32,
    pub PPSH_StringOffset: u32,
    pub PPSH_StringBytes: u32,
    pub PPSH_Reserved: [u8; 8],
}

impl Default for PathPrefixSidecarHeader {
    fn default() -> Self {
        Self {
            PPSH_Magic: *PATH_PREFIX_SIDECAR_MAGIC,
            PPSH_Version: PATH_PREFIX_SIDECAR_VERSION,
            PPSH_PrefixCount: 0,
            PPSH_EntryOffset: 32,
            PPSH_StringOffset: 32,
            PPSH_StringBytes: 0,
            PPSH_Reserved: [0; 8],
        }
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct PathPrefixSidecarEntry {
    pub PPSE_Offset: u32,
    pub PPSE_Length: u16,
    pub PPSE_Flags: u16,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
#[allow(non_snake_case)]
pub struct DocDataEntry {
    pub DDE_DocID: u32,
    pub DDE_StaticRank: u16,
    pub DDE_QualityScore: u16,
    pub DDE_FreshnessScore: u16,
    pub DDE_ClickScore: u16,
    pub DDE_EngagementScore: u16,
    pub DDE_AuthorityScore: u16,
    pub DDE_SpamScore: u16,
    pub DDE_PathLength: u16,
    pub DDE_Language: u16,
    pub DDE_Locale: u16,
    pub DDE_ContentType: u16,
    pub DDE_TitleLength: u32,
    pub DDE_BodyLength: u32,
    pub DDE_UrlLength: u32,
    pub DDE_AnchorLength: u32,
    pub DDE_MetaLength: u32,
    pub DDE_DiversityScore: f32,
    pub DDE_LengthQualityScore: f32,
    pub DDE_VectorDim: u16,
    pub DDE_VectorFormat: u16,
    pub DDE_Reserved: [u8; 6],
    pub DDE_VectorData: [i8; DOC_VECTOR_STORAGE_MAX_DIM],
    pub DDE_Path: [u8; DOC_PATH_MAX],
}
const _: [(); DOC_REC_SIZE] = [(); std::mem::size_of::<DocDataEntry>()];

#[repr(C, align(8))]
#[derive(Clone, Copy)]
pub struct IndexBlock { pub IB_Data: [u8; PAGE_SIZE] }
impl Default for IndexBlock { fn default() -> Self { Self { IB_Data: [0; PAGE_SIZE] } } }

#[repr(C)]
#[derive(Clone, Copy)]
pub struct LeafTermBlock {
    pub LTB_Directory: [u16; LEAF_TERM_DIRECTORY_COUNT],
    pub LTB_Data: [u8; PAGE_SIZE - LEAF_TERM_DATA_OFFSET],
}
impl Default for LeafTermBlock {
    fn default() -> Self { Self { LTB_Directory: [0; LEAF_TERM_DIRECTORY_COUNT], LTB_Data: [0; PAGE_SIZE - LEAF_TERM_DATA_OFFSET] } }
}
const _: [(); PAGE_SIZE] = [(); std::mem::size_of::<IndexBlock>()];
const _: [(); PAGE_SIZE] = [(); std::mem::size_of::<LeafTermBlock>()];

#[derive(Debug, Clone, Copy, Default)]
pub struct IndexBlockContinuationHeader { pub IBCH_MaxDocID: u64, pub IBCH_DataLength: u32 }
impl IndexBlockContinuationHeader {
    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < INDEX_BLOCK_CONTINUATION_HEADER_SIZE { return None; }
        Some(Self { IBCH_MaxDocID: u64::from_le_bytes(data[0..8].try_into().ok()?), IBCH_DataLength: u32::from_le_bytes(data[8..12].try_into().ok()?) })
    }
    pub fn write_to(&self, data: &mut [u8]) {
        data[0..8].copy_from_slice(&self.IBCH_MaxDocID.to_le_bytes());
        data[8..12].copy_from_slice(&self.IBCH_DataLength.to_le_bytes());
    }
}

#[repr(C, align(16))]
#[derive(Debug, Clone)]
pub struct HeadTermEntry {
    pub HTE_LeafTermBlockID: u32,
    pub HTE_FirstTermLength: u16,
    pub HTE_FirstTerm: [u8; HEAD_TERM_KEY_MAX],
}
impl HeadTermEntry {
    pub fn new(term: &str, block_id: u32) -> Self {
        let bytes = term.as_bytes();
        let mut first = [0u8; HEAD_TERM_KEY_MAX];
        first[..bytes.len()].copy_from_slice(bytes);
        Self { HTE_LeafTermBlockID: block_id, HTE_FirstTermLength: bytes.len() as u16, HTE_FirstTerm: first }
    }
    pub fn first_term(&self) -> &str {
        std::str::from_utf8(&self.HTE_FirstTerm[..self.HTE_FirstTermLength as usize]).unwrap_or("")
    }
    pub fn to_bytes(&self) -> [u8; 32] {
        let mut out = [0u8; 32];
        out[0..4].copy_from_slice(&self.HTE_LeafTermBlockID.to_le_bytes());
        out[4..6].copy_from_slice(&self.HTE_FirstTermLength.to_le_bytes());
        out[6..32].copy_from_slice(&self.HTE_FirstTerm);
        out
    }
    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < 32 { return None; }
        let term_length = u16::from_le_bytes(data[4..6].try_into().ok()?);
        if term_length as usize > HEAD_TERM_KEY_MAX { return None; }
        let mut first = [0u8; HEAD_TERM_KEY_MAX];
        first.copy_from_slice(&data[6..32]);
        Some(Self { HTE_LeafTermBlockID: u32::from_le_bytes(data[0..4].try_into().ok()?), HTE_FirstTermLength: term_length, HTE_FirstTerm: first })
    }
}

#[derive(Debug, Clone)]
pub struct LeafTermEntry {
    pub LTE_Term: String,
    pub LTE_DocFreq: u32,
    pub LTE_IndexBlockID: u32,
    pub LTE_IndexOffset: u16,
    pub LTE_IndexLength: u16,
    pub LTE_ContinuationBlockCount: u16,
    pub LTE_Flags: u8,
    pub LTE_TermLength: u8,
}
impl LeafTermEntry { pub fn byte_len(&self) -> usize { LEAF_TERM_ENTRY_SIZE + self.LTE_Term.len() } }

impl LeafTermBlock {
    pub fn entry_count(&self) -> usize {
        (self.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] as usize).min(LEAF_TERM_DIRECTORY_COUNT - 1)
    }
    pub fn entry(&self, index: usize) -> Option<LeafTermEntry> {
        if index >= self.entry_count() { return None; }
        let block_offset = self.LTB_Directory[index] as usize;
        if block_offset < LEAF_TERM_DATA_OFFSET { return None; }
        let offset = block_offset - LEAF_TERM_DATA_OFFSET;
        if offset + LEAF_TERM_ENTRY_SIZE > self.LTB_Data.len() { return None; }
        let data = &self.LTB_Data[offset..];
        let term_len = data[15] as usize;
        if offset + LEAF_TERM_ENTRY_SIZE + term_len > self.LTB_Data.len() { return None; }
        Some(LeafTermEntry {
            LTE_DocFreq: u32::from_le_bytes(data[0..4].try_into().ok()?),
            LTE_IndexBlockID: u32::from_le_bytes(data[4..8].try_into().ok()?),
            LTE_IndexOffset: u16::from_le_bytes(data[8..10].try_into().ok()?),
            LTE_IndexLength: u16::from_le_bytes(data[10..12].try_into().ok()?),
            LTE_ContinuationBlockCount: u16::from_le_bytes(data[12..14].try_into().ok()?),
            LTE_Flags: data[14],
            LTE_TermLength: data[15],
            LTE_Term: std::str::from_utf8(&data[16..16 + term_len]).ok()?.to_string(),
        })
    }
    pub fn entries(&self) -> Vec<LeafTermEntry> { (0..self.entry_count()).filter_map(|i| self.entry(i)).collect() }
    pub fn to_bytes(&self) -> [u8; PAGE_SIZE] {
        let mut out = [0u8; PAGE_SIZE];
        for (i, value) in self.LTB_Directory.iter().enumerate() { out[i * 2..i * 2 + 2].copy_from_slice(&value.to_le_bytes()); }
        out[LEAF_TERM_DATA_OFFSET..].copy_from_slice(&self.LTB_Data);
        out
    }
    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < PAGE_SIZE { return None; }
        let mut block = Self::default();
        for i in 0..LEAF_TERM_DIRECTORY_COUNT { block.LTB_Directory[i] = u16::from_le_bytes(data[i * 2..i * 2 + 2].try_into().ok()?); }
        if block.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] as usize > LEAF_TERM_DIRECTORY_COUNT - 1 { return None; }
        block.LTB_Data.copy_from_slice(&data[LEAF_TERM_DATA_OFFSET..PAGE_SIZE]);
        for index in 0..block.entry_count() { block.entry(index)?; }
        Some(block)
    }
}

pub struct BloomFilter;
impl BloomFilter { pub fn CanTermExist(&self, _term: &[u8]) -> bool { true } }

#[repr(C, packed)]
#[derive(Debug, Clone, Copy, Default)]
pub struct TermMphfEntry {
    pub LTE_DocFreq: u32,
    pub LTE_IndexBlockID: u32,
    pub LTE_IndexOffset: u32,
    pub LTE_IndexLength: u32,
    pub LTE_ContinuationBlockCount: u32,
    pub LTE_Flags: u32,
    pub LTE_Fingerprint: u64,
}

const _: [(); TERM_MPHF_ENTRY_SIZE] = [(); std::mem::size_of::<TermMphfEntry>()];

pub struct RWSpinLock { m_rwSpinlock: AtomicI32 }

#[allow(non_snake_case)]
impl RWSpinLock {
    pub fn new() -> Self { Self { m_rwSpinlock: AtomicI32::new(0) } }
    pub fn ReadLock(&self) {
        self.m_rwSpinlock.fetch_add(2, Ordering::AcqRel);
        while self.m_rwSpinlock.load(Ordering::Acquire) & 1 != 0 { std::hint::spin_loop(); }
    }
    pub fn ReadUnlock(&self) { self.m_rwSpinlock.fetch_sub(2, Ordering::Release); }
    pub fn WriteLock(&self) {
        while self.m_rwSpinlock.compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire).is_err() {
            std::thread::yield_now();
        }
    }
    pub fn WriteUnlock(&self) { self.m_rwSpinlock.fetch_sub(1, Ordering::Release); }
}

impl Default for RWSpinLock { fn default() -> Self { Self::new() } }

pub struct ReaderSpinLock<'a> { m_lock: &'a RWSpinLock }
impl<'a> ReaderSpinLock<'a> {
    pub fn new(lock: &'a RWSpinLock) -> Self { lock.ReadLock(); Self { m_lock: lock } }
}
impl Drop for ReaderSpinLock<'_> { fn drop(&mut self) { self.m_lock.ReadUnlock(); } }

pub struct WriterSpinLock<'a> { m_lock: &'a RWSpinLock }
impl<'a> WriterSpinLock<'a> {
    pub fn new(lock: &'a RWSpinLock) -> Self { lock.WriteLock(); Self { m_lock: lock } }
}
impl Drop for WriterSpinLock<'_> { fn drop(&mut self) { self.m_lock.WriteUnlock(); } }

pub fn DocDataEncodeScore(value: f32) -> u16 {
    if !(value > 0.0) { 0 } else if value >= 1.0 { u16::MAX } else { (value * 65535.0 + 0.5) as u16 }
}
pub fn DocDataDecodeScore(value: u16) -> f32 { value as f32 / 65535.0 }

#[derive(Debug, Clone, Copy, Default)]
pub struct BlockAccessStats {
    pub DirectGets: u64,
    pub DirectReleases: u64,
    pub WorkerGets: u64,
    pub WorkerReleases: u64,
    pub CacheHits: u64,
    pub CacheMisses: u64,
    pub DiskReads: u64,
}

#[derive(Clone, Copy)]
pub struct IndexSlotEntry { pub BlockID: u32, pub Ref: u32, pub Loading: bool }
impl Default for IndexSlotEntry { fn default() -> Self { Self { BlockID: u32::MAX, Ref: 0, Loading: false } } }

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum BlockRequestType { Get, Release }

pub struct BlockRequest {
    pub Type: BlockRequestType,
    pub BlockSeq: u32,
    pub Slot: AtomicU32,
    pub Address: AtomicUsize,
    pub Completion: (Mutex<bool>, Condvar),
}
impl BlockRequest {
    fn new(requestType: BlockRequestType, blockSeq: u32, slot: u32) -> Self {
        Self { Type: requestType, BlockSeq: blockSeq, Slot: AtomicU32::new(slot), Address: AtomicUsize::new(0), Completion: (Mutex::new(false), Condvar::new()) }
    }
    fn Wait(&self) {
        let (lock, cv) = &self.Completion;
        let mut complete = lock.lock().unwrap();
        while !*complete { complete = cv.wait(complete).unwrap(); }
    }
    fn Complete(&self) {
        let (lock, cv) = &self.Completion;
        *lock.lock().unwrap() = true;
        cv.notify_one();
    }
}

struct BlockCacheState {
    BCP_Pages: Option<PinnedMemory<u8>>,
    BCP_BaseOffset: u64,
    BCP_TotalBlockCount: u32,
    BCP_SlotCount: u32,
    BCP_EvictSlot: u32,
    BCP_LogicTable: Option<PinnedMemory<u32>>,
    BCP_SlotTable: Option<PinnedMemory<IndexSlotEntry>>,
    BCP_File: Option<Arc<FileAccess>>,
}
impl BlockCacheState {
    fn new() -> Self {
        Self { BCP_Pages: None, BCP_BaseOffset: 0, BCP_TotalBlockCount: 0, BCP_SlotCount: 0, BCP_EvictSlot: 0, BCP_LogicTable: None, BCP_SlotTable: None, BCP_File: None }
    }
}

pub struct BlockCachePool {
    BCP_State: Mutex<BlockCacheState>,
    BCP_StateCv: Condvar,
    BCP_Requests: Mutex<VecDeque<Arc<BlockRequest>>>,
    BCP_RequestCv: Condvar,
    BCP_ExitThread: AtomicBool,
    #[cfg(not(target_arch = "wasm32"))]
    BCP_Thread: Mutex<Option<JoinHandle<()>>>,
}
impl BlockCachePool {
    fn new() -> Self {
        Self {
            BCP_State: Mutex::new(BlockCacheState::new()),
            BCP_StateCv: Condvar::new(),
            BCP_Requests: Mutex::new(VecDeque::new()),
            BCP_RequestCv: Condvar::new(),
            BCP_ExitThread: AtomicBool::new(false),
            #[cfg(not(target_arch = "wasm32"))]
            BCP_Thread: Mutex::new(None),
        }
    }
}

#[derive(Default)]
struct BlockAccessCounters {
    m_DirectGets: AtomicU64,
    m_DirectReleases: AtomicU64,
    m_WorkerGets: AtomicU64,
    m_WorkerReleases: AtomicU64,
    m_CacheHits: AtomicU64,
    m_CacheMisses: AtomicU64,
    m_DiskReads: AtomicU64,
}

pub struct BlockHandle<T> {
    page: NonNull<T>,
    slot: u32,
    kind: BlockKind,
    _marker: PhantomData<T>,
}
unsafe impl<T: Send> Send for BlockHandle<T> {}
unsafe impl<T: Sync> Sync for BlockHandle<T> {}
impl<T> BlockHandle<T> {
    pub fn Slot(&self) -> u32 { self.slot }
    pub fn Kind(&self) -> BlockKind { self.kind }
}
impl<T> Deref for BlockHandle<T> {
    type Target = T;
    fn deref(&self) -> &T { unsafe { self.page.as_ref() } }
}

pub struct IndexLocation {
    pub index_block_id: u32,
    pub index_offset: usize,
    pub index_length: usize,
    pub doc_freq: u32,
    pub continuation_block_count: u32,
}

pub struct IndexBlockTable {
    pub m_ElementFilter: Option<Arc<Mutex<ElementFilter>>>,
    pub m_IndexPool: Arc<BlockCachePool>,
    pub m_LeafTermPool: Arc<BlockCachePool>,
    pub m_BloomFilter: BloomFilter,
    m_TermMphfHeader: TermMphfHeader,
    m_TermMphfDisplacements: Vec<i32>,
    m_TermMphfDisplacementCount: u32,
    m_TermMphfEntryPages: Vec<IndexBlock>,
    m_TermMphfEntryPageCount: u32,
    m_TermMphfEnabled: AtomicBool,
    m_DirectBlockAccess: AtomicBool,
    m_AccessCounters: Arc<BlockAccessCounters>,
    m_HeadTermEntries: Vec<HeadTermEntry>,
    m_HeadTermEntryCount: u32,
}

impl IndexBlockTable {
    pub fn new(_capacity: usize) -> Self {
        Self {
            m_ElementFilter: None,
            m_IndexPool: Arc::new(BlockCachePool::new()),
            m_LeafTermPool: Arc::new(BlockCachePool::new()),
            m_BloomFilter: BloomFilter,
            m_TermMphfHeader: TermMphfHeader::default(),
            m_TermMphfDisplacements: Vec::new(),
            m_TermMphfDisplacementCount: 0,
            m_TermMphfEntryPages: Vec::new(),
            m_TermMphfEntryPageCount: 0,
            m_TermMphfEnabled: AtomicBool::new(true),
            m_DirectBlockAccess: AtomicBool::new(true),
            m_AccessCounters: Arc::new(BlockAccessCounters::default()),
            m_HeadTermEntries: Vec::new(),
            m_HeadTermEntryCount: 0,
        }
    }

    pub fn SetHeadTermEntries(&mut self, head: Vec<HeadTermEntry>) {
        self.m_HeadTermEntryCount = head.len().min(u32::MAX as usize) as u32;
        self.m_HeadTermEntries = head;
    }

    pub fn SetTermMphf(&mut self, header: TermMphfHeader, displacements: Vec<i32>, entryPages: Vec<IndexBlock>) {
        self.ClearTermMphf();
        if header.TMH_TermCount == 0 || header.TMH_BucketCount == 0 || header.TMH_SlotCount == 0 || displacements.is_empty() || entryPages.is_empty() { return; }
        let requiredBytes = header.TMH_SlotCount as usize * TERM_MPHF_ENTRY_SIZE;
        let availableBytes = entryPages.len() * PAGE_SIZE;
        if header.TMH_Magic != TERM_MPHF_MAGIC || header.TMH_SlotCount as u64 != header.TMH_TermCount || displacements.len() != header.TMH_BucketCount as usize || requiredBytes > availableBytes { return; }
        self.m_TermMphfHeader = header;
        self.m_TermMphfDisplacements = displacements;
        self.m_TermMphfDisplacementCount = self.m_TermMphfDisplacements.len() as u32;
        self.m_TermMphfEntryPages = entryPages;
        self.m_TermMphfEntryPageCount = self.m_TermMphfEntryPages.len() as u32;
    }

    pub fn SetTermMphfEnabled(&self, enabled: bool) { self.m_TermMphfEnabled.store(enabled, Ordering::Relaxed); }

    pub fn SetDirectBlockAccessEnabled(&self, enabled: bool) {
        if self.m_DirectBlockAccess.load(Ordering::Relaxed) == enabled { return; }
        #[cfg(target_arch = "wasm32")]
        if !enabled { panic!("IndexBlockTable worker mode is not supported on wasm32"); }
        self.m_DirectBlockAccess.store(enabled, Ordering::Relaxed);
        if enabled {
            Self::ExitBlockThread(&self.m_IndexPool);
            Self::ExitBlockThread(&self.m_LeafTermPool);
        } else {
            self.StartBlockThread(&self.m_IndexPool);
            self.StartBlockThread(&self.m_LeafTermPool);
        }
    }

    pub fn HandOverBlockTable(&mut self, source: &mut IndexBlockTable) {
        if std::ptr::eq(self, source) { return; }
        Self::ExitBlockThread(&self.m_IndexPool);
        Self::ExitBlockThread(&self.m_LeafTermPool);
        Self::ExitBlockThread(&source.m_IndexPool);
        Self::ExitBlockThread(&source.m_LeafTermPool);
        self.m_IndexPool = Self::HandOverPool(&mut source.m_IndexPool);
        self.m_LeafTermPool = Self::HandOverPool(&mut source.m_LeafTermPool);
        self.m_ElementFilter = source.m_ElementFilter.take();
        self.m_HeadTermEntries = std::mem::take(&mut source.m_HeadTermEntries);
        self.m_HeadTermEntryCount = source.m_HeadTermEntryCount;
        source.m_HeadTermEntryCount = 0;
        self.m_TermMphfHeader = source.m_TermMphfHeader;
        self.m_TermMphfDisplacements = std::mem::take(&mut source.m_TermMphfDisplacements);
        self.m_TermMphfDisplacementCount = source.m_TermMphfDisplacementCount;
        source.m_TermMphfDisplacementCount = 0;
        self.m_TermMphfEntryPages = std::mem::take(&mut source.m_TermMphfEntryPages);
        self.m_TermMphfEntryPageCount = source.m_TermMphfEntryPageCount;
        source.m_TermMphfEntryPageCount = 0;
        source.m_TermMphfHeader = TermMphfHeader::default();
        self.StartBlockThread(&self.m_IndexPool);
        self.StartBlockThread(&self.m_LeafTermPool);
    }

    pub fn FindTermData(&self, term: &str) -> Option<IndexLocation> {
        let bytes = term.as_bytes();
        let termLength = bytes.iter().position(|byte| *byte == 0).unwrap_or(bytes.len());
        let termBytes = &bytes[..termLength];
        let term = &term[..termLength];
        if !self.m_BloomFilter.CanTermExist(termBytes) { return None; }
        if self.HasTermMphf() {
            if let Some(location) = self.FindTermDataMphf(termBytes) { return Some(location); }
        }
        self.FindTermDataHeadLeaf(term)
    }

    pub fn GetBlock<T>(&self, kind: BlockKind, blockSeq: u32, sequential: bool) -> Option<BlockHandle<T>> {
        if std::mem::size_of::<T>() != PAGE_SIZE { return None; }
        let pool = self.Pool(kind);
        if sequential {
            let mut state = pool.BCP_State.lock().ok()?;
            if state.BCP_Pages.is_some() && state.BCP_LogicTable.is_some() && state.BCP_SlotTable.is_some() && blockSeq < state.BCP_TotalBlockCount {
                let slot = state.BCP_LogicTable.as_ref()?.as_slice()[blockSeq as usize];
                if slot != u32::MAX && slot < state.BCP_SlotCount {
                    state.BCP_SlotTable.as_mut()?.as_mut_slice()[slot as usize].Ref += 1;
                    return Self::MakeHandle(&state, kind, slot);
                }
            }
            if !Self::LoadSequentialWindow(&mut state, blockSeq) { return None; }
            let slot = state.BCP_LogicTable.as_ref()?.as_slice()[blockSeq as usize];
            state.BCP_SlotTable.as_mut()?.as_mut_slice()[slot as usize].Ref += 1;
            return Self::MakeHandle(&state, kind, slot);
        }

        if self.m_DirectBlockAccess.load(Ordering::Relaxed) {
            self.m_AccessCounters.m_DirectGets.fetch_add(1, Ordering::Relaxed);
            let request = BlockRequest::new(BlockRequestType::Get, blockSeq, u32::MAX);
            Self::ProcessGetBlockLocked(pool, &request, &self.m_AccessCounters);
            return Self::RequestHandle(kind, &request);
        }

        if let Some(handle) = self.TryPinReadyOrWait::<T>(pool, kind, blockSeq) { return Some(handle); }
        self.m_AccessCounters.m_WorkerGets.fetch_add(1, Ordering::Relaxed);
        let request = Arc::new(BlockRequest::new(BlockRequestType::Get, blockSeq, u32::MAX));
        Self::SubmitBlockRequest(pool, Arc::clone(&request));
        request.Wait();
        Self::RequestHandle(kind, &request)
    }

    pub fn ReleaseBlock(&self, kind: BlockKind, slot: u32, sequential: bool) {
        if slot == u32::MAX { return; }
        let pool = self.Pool(kind);
        if sequential {
            if let Ok(mut state) = pool.BCP_State.lock() {
                if let Some(table) = state.BCP_SlotTable.as_mut() {
                    if slot < state.BCP_SlotCount && table[slot as usize].Ref > 0 { table[slot as usize].Ref -= 1; }
                }
            }
            return;
        }
        if self.m_DirectBlockAccess.load(Ordering::Relaxed) {
            self.m_AccessCounters.m_DirectReleases.fetch_add(1, Ordering::Relaxed);
        } else {
            self.m_AccessCounters.m_WorkerReleases.fetch_add(1, Ordering::Relaxed);
        }
        let request = BlockRequest::new(BlockRequestType::Release, 0, slot);
        Self::ProcessReleaseBlockLocked(pool, &request);
    }

    pub fn SetBlockMemory(&mut self, indexBlocks: Option<Vec<IndexBlock>>, leafTermBlocks: Option<Vec<LeafTermBlock>>) {
        Self::ExitBlockThread(&self.m_IndexPool);
        Self::ExitBlockThread(&self.m_LeafTermPool);
        Self::SetPoolMemory(&self.m_IndexPool, indexBlocks.as_deref());
        Self::SetPoolMemory(&self.m_LeafTermPool, leafTermBlocks.as_deref());
        self.StartBlockThread(&self.m_IndexPool);
        self.StartBlockThread(&self.m_LeafTermPool);
        if indexBlocks.is_none() && leafTermBlocks.is_none() { self.ClearTermMphf(); }
    }

    pub fn Init(&mut self, kind: BlockKind, path: Option<&str>, baseOffset: u64, blockCount: u32, slotCount: u32) -> std::io::Result<()> {
        let pool = self.Pool(kind);
        Self::ExitBlockThread(pool);
        let mut state = pool.BCP_State.lock().unwrap();
        *state = BlockCacheState::new();
        state.BCP_BaseOffset = baseOffset;
        state.BCP_TotalBlockCount = blockCount;
        state.BCP_SlotCount = slotCount.min(blockCount);
        state.BCP_EvictSlot = state.BCP_SlotCount;
        if let Some(path) = path.filter(|path| !path.is_empty()) {
            let mut file = FileAccess::new(path);
            if !file.Init() { return Err(std::io::Error::new(std::io::ErrorKind::NotFound, "index file open failed")); }
            state.BCP_File = Some(Arc::new(file));
        }
        state.BCP_Pages = Some(PinnedMemory::new_zeroed(state.BCP_SlotCount as usize * PAGE_SIZE));
        state.BCP_LogicTable = Some(PinnedMemory::from_slice(&vec![u32::MAX; blockCount as usize]));
        state.BCP_SlotTable = Some(PinnedMemory::from_slice(&vec![IndexSlotEntry::default(); state.BCP_SlotCount as usize]));
        if state.BCP_SlotCount > 0 {
            for block in 0..state.BCP_SlotCount {
                state.BCP_LogicTable.as_mut().unwrap()[block as usize] = block;
                state.BCP_SlotTable.as_mut().unwrap()[block as usize].BlockID = block;
            }
        }
        drop(state);
        self.StartBlockThread(pool);
        Ok(())
    }

    pub fn GetBlockAccessStats(&self) -> BlockAccessStats {
        BlockAccessStats {
            DirectGets: self.m_AccessCounters.m_DirectGets.load(Ordering::Relaxed),
            DirectReleases: self.m_AccessCounters.m_DirectReleases.load(Ordering::Relaxed),
            WorkerGets: self.m_AccessCounters.m_WorkerGets.load(Ordering::Relaxed),
            WorkerReleases: self.m_AccessCounters.m_WorkerReleases.load(Ordering::Relaxed),
            CacheHits: self.m_AccessCounters.m_CacheHits.load(Ordering::Relaxed),
            CacheMisses: self.m_AccessCounters.m_CacheMisses.load(Ordering::Relaxed),
            DiskReads: self.m_AccessCounters.m_DiskReads.load(Ordering::Relaxed),
        }
    }

    pub fn HeadTermEntries(&self) -> &[HeadTermEntry] { &self.m_HeadTermEntries }
    pub fn TermMphfHeader(&self) -> &TermMphfHeader { &self.m_TermMphfHeader }
    pub fn TermMphfDisplacements(&self) -> &[i32] { &self.m_TermMphfDisplacements }
    pub fn TermMphfEntryPages(&self) -> &[IndexBlock] { &self.m_TermMphfEntryPages }
    pub fn LeafTermBlockCount(&self) -> u32 { self.m_LeafTermPool.BCP_State.lock().map(|s| s.BCP_TotalBlockCount).unwrap_or(0) }

    pub fn IndexBlocks(&self) -> Vec<IndexBlock> { Self::CopyPoolBlocks(&self.m_IndexPool) }
    pub fn LeafTermBlocks(&self) -> Vec<LeafTermBlock> { Self::CopyPoolBlocks(&self.m_LeafTermPool) }

    fn Pool(&self, kind: BlockKind) -> &Arc<BlockCachePool> {
        match kind { BlockKind::Index => &self.m_IndexPool, BlockKind::LeafTerm => &self.m_LeafTermPool }
    }

    fn MakeHandle<T>(state: &BlockCacheState, kind: BlockKind, slot: u32) -> Option<BlockHandle<T>> {
        let address = unsafe { state.BCP_Pages.as_ref()?.as_slice().as_ptr().add(slot as usize * PAGE_SIZE) as *mut T };
        Some(BlockHandle { page: NonNull::new(address)?, slot, kind, _marker: PhantomData })
    }

    fn RequestHandle<T>(kind: BlockKind, request: &BlockRequest) -> Option<BlockHandle<T>> {
        let address = request.Address.load(Ordering::Acquire) as *mut T;
        Some(BlockHandle { page: NonNull::new(address)?, slot: request.Slot.load(Ordering::Acquire), kind, _marker: PhantomData })
    }

    fn SetPoolMemory<T: Copy>(pool: &Arc<BlockCachePool>, blocks: Option<&[T]>) {
        let mut state = pool.BCP_State.lock().unwrap();
        *state = BlockCacheState::new();
        let Some(blocks) = blocks else { return; };
        if std::mem::size_of::<T>() != PAGE_SIZE { return; }
        state.BCP_TotalBlockCount = blocks.len() as u32;
        state.BCP_SlotCount = blocks.len() as u32;
        state.BCP_EvictSlot = blocks.len() as u32;
        let bytes = unsafe { std::slice::from_raw_parts(blocks.as_ptr() as *const u8, blocks.len() * PAGE_SIZE) };
        state.BCP_Pages = Some(PinnedMemory::from_slice(bytes));
        state.BCP_LogicTable = Some(PinnedMemory::from_slice(&(0..blocks.len() as u32).collect::<Vec<_>>()));
        state.BCP_SlotTable = Some(PinnedMemory::from_slice(&(0..blocks.len() as u32).map(|block| IndexSlotEntry { BlockID: block, Ref: 0, Loading: false }).collect::<Vec<_>>()));
    }

    fn CopyPoolBlocks<T: Copy>(pool: &Arc<BlockCachePool>) -> Vec<T> {
        let Ok(state) = pool.BCP_State.lock() else { return Vec::new(); };
        let Some(pages) = state.BCP_Pages.as_ref() else { return Vec::new(); };
        if std::mem::size_of::<T>() != PAGE_SIZE { return Vec::new(); }
        let count = state.BCP_TotalBlockCount.min(state.BCP_SlotCount) as usize;
        unsafe { std::slice::from_raw_parts(pages.as_slice().as_ptr() as *const T, count).to_vec() }
    }

    fn FindTermDataMphf(&self, term: &[u8]) -> Option<IndexLocation> {
        if !self.HasTermMphf() { return None; }
        let bucket = (TermMphfHash(term, self.m_TermMphfHeader.TMH_BucketSeed) % self.m_TermMphfHeader.TMH_BucketCount as u64) as usize;
        let displacement = *self.m_TermMphfDisplacements.get(bucket)?;
        let slot = if displacement < 0 { (-(displacement as i64) - 1) as u64 } else { TermMphfHash(term, TermMphfSlotSeed(self.m_TermMphfHeader.TMH_SlotSeed, displacement as u32)) % self.m_TermMphfHeader.TMH_SlotCount as u64 };
        if slot >= self.m_TermMphfHeader.TMH_SlotCount as u64 { return None; }
        let byteOffset = slot as usize * TERM_MPHF_ENTRY_SIZE;
        let pageID = byteOffset / PAGE_SIZE;
        if pageID >= self.m_TermMphfEntryPageCount as usize { return None; }
        let data = &self.m_TermMphfEntryPages.get(pageID)?.IB_Data[byteOffset % PAGE_SIZE..byteOffset % PAGE_SIZE + TERM_MPHF_ENTRY_SIZE];
        let mut fingerprint = TermMphfHash(term, self.m_TermMphfHeader.TMH_FingerprintSeed);
        if fingerprint == 0 { fingerprint = 1; }
        if u64::from_le_bytes(data[24..32].try_into().ok()?) != fingerprint { return None; }
        Some(IndexLocation {
            doc_freq: u32::from_le_bytes(data[0..4].try_into().ok()?),
            index_block_id: u32::from_le_bytes(data[4..8].try_into().ok()?),
            index_offset: u32::from_le_bytes(data[8..12].try_into().ok()?) as usize,
            index_length: u32::from_le_bytes(data[12..16].try_into().ok()?) as usize,
            continuation_block_count: u32::from_le_bytes(data[16..20].try_into().ok()?),
        })
    }

    fn FindTermDataHeadLeaf(&self, term: &str) -> Option<IndexLocation> {
        if self.m_HeadTermEntryCount == 0 || term.len() > HEAD_TERM_KEY_MAX { return None; }
        let entries = self.m_HeadTermEntries.get(..self.m_HeadTermEntryCount as usize)?;
        let pos = entries.partition_point(|entry| entry.first_term() <= term);
        if pos == 0 { return None; }
        let blockID = entries[pos - 1].HTE_LeafTermBlockID;
        let block = self.GetBlock::<LeafTermBlock>(BlockKind::LeafTerm, blockID, false)?;
        let slot = block.Slot();
        let result = (|| {
            let mut left = 0usize;
            let mut right = block.entry_count();
            while left < right {
                let mid = left + (right - left) / 2;
                if block.entry(mid)?.LTE_Term.as_str() < term { left = mid + 1; } else { right = mid; }
            }
            if left == block.entry_count() { return None; }
            let entry = block.entry(left)?;
            if entry.LTE_Term != term { return None; }
            Some(IndexLocation { index_block_id: entry.LTE_IndexBlockID, index_offset: entry.LTE_IndexOffset as usize, index_length: entry.LTE_IndexLength as usize, doc_freq: entry.LTE_DocFreq, continuation_block_count: entry.LTE_ContinuationBlockCount as u32 })
        })();
        self.ReleaseBlock(BlockKind::LeafTerm, slot, false);
        result
    }

    fn HasTermMphf(&self) -> bool {
        self.m_TermMphfEnabled.load(Ordering::Relaxed)
            && self.m_TermMphfHeader.TMH_Magic == TERM_MPHF_MAGIC
            && self.m_TermMphfHeader.TMH_TermCount > 0
            && self.m_TermMphfHeader.TMH_BucketCount > 0
            && self.m_TermMphfHeader.TMH_SlotCount > 0
            && self.m_TermMphfDisplacementCount == self.m_TermMphfHeader.TMH_BucketCount
            && self.m_TermMphfDisplacements.len() == self.m_TermMphfDisplacementCount as usize
            && self.m_TermMphfEntryPages.len() == self.m_TermMphfEntryPageCount as usize
            && self.m_TermMphfEntryPageCount as usize * PAGE_SIZE >= self.m_TermMphfHeader.TMH_SlotCount as usize * TERM_MPHF_ENTRY_SIZE
    }

    fn ClearTermMphf(&mut self) {
        self.m_TermMphfHeader = TermMphfHeader::default();
        self.m_TermMphfDisplacements.clear();
        self.m_TermMphfDisplacementCount = 0;
        self.m_TermMphfEntryPages.clear();
        self.m_TermMphfEntryPageCount = 0;
    }

    fn HandOverPool(source: &mut Arc<BlockCachePool>) -> Arc<BlockCachePool> {
        std::mem::replace(source, Arc::new(BlockCachePool::new()))
    }

    fn StartBlockThread(&self, pool: &Arc<BlockCachePool>) {
        if self.m_DirectBlockAccess.load(Ordering::Relaxed) { return; }
        #[cfg(not(target_arch = "wasm32"))]
        {
            let ready = pool.BCP_State.lock().map(|state| state.BCP_Pages.is_some() && state.BCP_SlotCount > 0).unwrap_or(false);
            let mut handle = pool.BCP_Thread.lock().unwrap();
            if !ready || handle.is_some() { return; }
            pool.BCP_ExitThread.store(false, Ordering::Release);
            let target = Arc::clone(pool);
            let counters = Arc::clone(&self.m_AccessCounters);
            *handle = Some(thread::spawn(move || Self::BlockThreadMain(target, counters)));
        }
    }

    fn ExitBlockThread(pool: &Arc<BlockCachePool>) {
        #[cfg(not(target_arch = "wasm32"))]
        {
            let handle = pool.BCP_Thread.lock().unwrap().take();
            let Some(handle) = handle else { return; };
            pool.BCP_ExitThread.store(true, Ordering::Release);
            pool.BCP_RequestCv.notify_one();
            let _ = handle.join();
        }
    }

    fn LoadSequentialWindow(state: &mut BlockCacheState, startBlock: u32) -> bool {
        if state.BCP_File.is_none() || state.BCP_Pages.is_none() || state.BCP_LogicTable.is_none() || state.BCP_SlotTable.is_none() || startBlock >= state.BCP_TotalBlockCount || state.BCP_SlotCount == 0 { return false; }
        if state.BCP_SlotTable.as_ref().unwrap().as_slice().iter().any(|slot| slot.Ref > 0) { return false; }
        for slot in state.BCP_SlotTable.as_ref().unwrap().as_slice() {
            if slot.BlockID != u32::MAX { state.BCP_LogicTable.as_mut().unwrap()[slot.BlockID as usize] = u32::MAX; }
        }
        for slot in state.BCP_SlotTable.as_mut().unwrap().as_mut_slice() { *slot = IndexSlotEntry::default(); }
        let blockCount = state.BCP_SlotCount.min(state.BCP_TotalBlockCount - startBlock);
        let bytes = blockCount as usize * PAGE_SIZE;
        if bytes > i32::MAX as usize { return false; }
        let file = Arc::clone(state.BCP_File.as_ref().unwrap());
        if !file.SetPosition(state.BCP_BaseOffset + startBlock as u64 * PAGE_SIZE as u64) || file.GetData(&mut state.BCP_Pages.as_mut().unwrap().as_mut_slice()[..bytes], bytes as i32) != bytes as i32 { return false; }
        for offset in 0..blockCount {
            let block = startBlock + offset;
            state.BCP_SlotTable.as_mut().unwrap()[offset as usize] = IndexSlotEntry { BlockID: block, Ref: 0, Loading: false };
            state.BCP_LogicTable.as_mut().unwrap()[block as usize] = offset;
        }
        state.BCP_EvictSlot = blockCount;
        state.BCP_LogicTable.as_ref().unwrap()[startBlock as usize] != u32::MAX
    }

    fn BlockThreadMain(pool: Arc<BlockCachePool>, counters: Arc<BlockAccessCounters>) {
        loop {
            let request = {
                let mut queue = pool.BCP_Requests.lock().unwrap();
                while !pool.BCP_ExitThread.load(Ordering::Acquire) && queue.is_empty() { queue = pool.BCP_RequestCv.wait(queue).unwrap(); }
                if pool.BCP_ExitThread.load(Ordering::Acquire) && queue.is_empty() { break; }
                let request = queue.pop_front();
                pool.BCP_RequestCv.notify_all();
                request
            };
            let Some(request) = request else { continue; };
            if request.Type == BlockRequestType::Get { Self::ProcessGetBlockLocked(&pool, &request, &counters); }
            else { Self::ProcessReleaseBlockLocked(&pool, &request); }
            request.Complete();
        }
    }

    fn SubmitBlockRequest(pool: &Arc<BlockCachePool>, request: Arc<BlockRequest>) {
        let mut queue = pool.BCP_Requests.lock().unwrap();
        while queue.len() == BLOCK_REQUEST_RING_SIZE {
            queue = pool.BCP_RequestCv.wait(queue).unwrap();
        }
        queue.push_back(request);
        pool.BCP_RequestCv.notify_one();
    }

    fn TryPinReadyOrWait<T>(&self, pool: &Arc<BlockCachePool>, kind: BlockKind, blockSeq: u32) -> Option<BlockHandle<T>> {
        let mut state = pool.BCP_State.lock().ok()?;
        loop {
            if blockSeq >= state.BCP_TotalBlockCount { return None; }
            let slot = state.BCP_LogicTable.as_ref()?.as_slice()[blockSeq as usize];
            if slot == u32::MAX || slot >= state.BCP_SlotCount { return None; }
            let entry = state.BCP_SlotTable.as_ref()?.as_slice()[slot as usize];
            if !entry.Loading {
                if entry.BlockID != blockSeq { return None; }
                self.m_AccessCounters.m_CacheHits.fetch_add(1, Ordering::Relaxed);
                state.BCP_SlotTable.as_mut()?.as_mut_slice()[slot as usize].Ref += 1;
                return Self::MakeHandle(&state, kind, slot);
            }
            state = pool.BCP_StateCv.wait(state).ok()?;
        }
    }

    fn ProcessGetBlockLocked(pool: &Arc<BlockCachePool>, request: &BlockRequest, counters: &Arc<BlockAccessCounters>) {
        let (found, address, file, baseOffset) = {
            let mut state = match pool.BCP_State.lock() { Ok(state) => state, Err(_) => return };
            let blockSeq = request.BlockSeq;
            if state.BCP_Pages.is_none() || state.BCP_LogicTable.is_none() || state.BCP_SlotTable.is_none() || blockSeq >= state.BCP_TotalBlockCount || state.BCP_SlotCount == 0 { return; }
            loop {
                let slot = state.BCP_LogicTable.as_ref().unwrap()[blockSeq as usize];
                if slot == u32::MAX || slot >= state.BCP_SlotCount { break; }
                let entry = state.BCP_SlotTable.as_ref().unwrap()[slot as usize];
                if !entry.Loading {
                    if entry.BlockID == blockSeq {
                        counters.m_CacheHits.fetch_add(1, Ordering::Relaxed);
                        state.BCP_SlotTable.as_mut().unwrap()[slot as usize].Ref += 1;
                        let address = unsafe { state.BCP_Pages.as_ref().unwrap().as_slice().as_ptr().add(slot as usize * PAGE_SIZE) as usize };
                        request.Slot.store(slot, Ordering::Release);
                        request.Address.store(address, Ordering::Release);
                        return;
                    }
                    break;
                }
                state = pool.BCP_StateCv.wait(state).unwrap();
            }
            counters.m_CacheMisses.fetch_add(1, Ordering::Relaxed);
            let mut found = u32::MAX;
            for _ in 0..state.BCP_SlotCount {
                let candidate = state.BCP_EvictSlot % state.BCP_SlotCount;
                state.BCP_EvictSlot = state.BCP_EvictSlot.wrapping_add(1);
                let entry = state.BCP_SlotTable.as_ref().unwrap()[candidate as usize];
                if entry.Ref == 0 && !entry.Loading { found = candidate; break; }
            }
            if found == u32::MAX { return; }
            let oldBlock = state.BCP_SlotTable.as_ref().unwrap()[found as usize].BlockID;
            if oldBlock != u32::MAX { state.BCP_LogicTable.as_mut().unwrap()[oldBlock as usize] = u32::MAX; }
            state.BCP_SlotTable.as_mut().unwrap()[found as usize] = IndexSlotEntry { BlockID: blockSeq, Ref: 1, Loading: true };
            state.BCP_LogicTable.as_mut().unwrap()[blockSeq as usize] = found;
            let address = unsafe { state.BCP_Pages.as_ref().unwrap().as_slice().as_ptr().add(found as usize * PAGE_SIZE) as usize };
            request.Slot.store(found, Ordering::Release);
            request.Address.store(address, Ordering::Release);
            (found, address, state.BCP_File.as_ref().map(Arc::clone), state.BCP_BaseOffset)
        };

        let ok = if let Some(file) = file {
            let bytes = unsafe { std::slice::from_raw_parts_mut(address as *mut u8, PAGE_SIZE) };
            file.ReadBlock(request.BlockSeq, bytes, PAGE_SIZE, baseOffset)
        } else { false };
        if ok { counters.m_DiskReads.fetch_add(1, Ordering::Relaxed); }
        if let Ok(mut state) = pool.BCP_State.lock() {
            if !ok {
                if request.BlockSeq < state.BCP_TotalBlockCount && state.BCP_LogicTable.as_ref().map(|table| table[request.BlockSeq as usize] == found).unwrap_or(false) { state.BCP_LogicTable.as_mut().unwrap()[request.BlockSeq as usize] = u32::MAX; }
                if found < state.BCP_SlotCount { state.BCP_SlotTable.as_mut().unwrap()[found as usize] = IndexSlotEntry::default(); }
                request.Slot.store(u32::MAX, Ordering::Release);
                request.Address.store(0, Ordering::Release);
            } else if found < state.BCP_SlotCount {
                state.BCP_SlotTable.as_mut().unwrap()[found as usize].Loading = false;
            }
        }
        pool.BCP_StateCv.notify_all();
    }

    fn ProcessReleaseBlockLocked(pool: &Arc<BlockCachePool>, request: &BlockRequest) {
        if let Ok(mut state) = pool.BCP_State.lock() { Self::ProcessReleaseBlock(&mut state, request); }
    }

    fn ProcessReleaseBlock(state: &mut BlockCacheState, request: &BlockRequest) {
        let slot = request.Slot.load(Ordering::Acquire);
        let slotCount = state.BCP_SlotCount;
        if let Some(table) = state.BCP_SlotTable.as_mut() {
            if slot != u32::MAX && slot < slotCount && table[slot as usize].Ref > 0 { table[slot as usize].Ref -= 1; }
        }
    }
}

impl Drop for IndexBlockTable {
    fn drop(&mut self) {
        Self::ExitBlockThread(&self.m_IndexPool);
        Self::ExitBlockThread(&self.m_LeafTermPool);
        self.ClearTermMphf();
    }
}

pub const MAX_DOCID: u64 = u64::MAX;
pub const MAX_BLOCK_SIZE: u32 = PAGE_SIZE as u32;
