use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::ops::Deref;
use std::sync::{mpsc, Arc, Mutex};
use std::thread::{self, JoinHandle};

#[cfg(target_os = "linux")]
use std::os::fd::AsRawFd;

#[cfg(target_os = "linux")]
use std::sync::atomic::{AtomicU64, Ordering};

use crate::pinned_memory::PinnedMemory;

#[cfg(target_os = "linux")]
static IO_URING_READS: AtomicU64 = AtomicU64::new(0);
#[cfg(target_os = "linux")]
static PREAD_FALLBACK_READS: AtomicU64 = AtomicU64::new(0);
#[cfg(target_os = "linux")]
static IO_URING_SETUP_OK: AtomicU64 = AtomicU64::new(0);
#[cfg(target_os = "linux")]
static IO_URING_SETUP_FAILED: AtomicU64 = AtomicU64::new(0);

#[cfg(target_os = "linux")]
const IORING_OFF_SQ_RING: libc::off_t = 0;
#[cfg(target_os = "linux")]
const IORING_OFF_CQ_RING: libc::off_t = 0x8000000;
#[cfg(target_os = "linux")]
const IORING_OFF_SQES: libc::off_t = 0x10000000;
#[cfg(target_os = "linux")]
const IORING_FEAT_SINGLE_MMAP: u32 = 1;
#[cfg(target_os = "linux")]
const IORING_ENTER_GETEVENTS: u32 = 1;
#[cfg(target_os = "linux")]
const IORING_OP_READ: u8 = 22;

#[cfg(target_os = "linux")]
#[repr(C)]
#[derive(Default, Clone, Copy)]
#[allow(dead_code)]
struct IoSqringOffsets {
    head: u32,
    tail: u32,
    ring_mask: u32,
    ring_entries: u32,
    flags: u32,
    dropped: u32,
    array: u32,
    resv1: u32,
    user_addr: u64,
}

#[cfg(target_os = "linux")]
#[repr(C)]
#[derive(Default, Clone, Copy)]
#[allow(dead_code)]
struct IoCqringOffsets {
    head: u32,
    tail: u32,
    ring_mask: u32,
    ring_entries: u32,
    overflow: u32,
    cqes: u32,
    flags: u32,
    resv1: u32,
    user_addr: u64,
}

#[cfg(target_os = "linux")]
#[repr(C)]
#[derive(Default, Clone, Copy)]
#[allow(dead_code)]
struct IoUringParams {
    sq_entries: u32,
    cq_entries: u32,
    flags: u32,
    sq_thread_cpu: u32,
    sq_thread_idle: u32,
    features: u32,
    wq_fd: u32,
    resv: [u32; 3],
    sq_off: IoSqringOffsets,
    cq_off: IoCqringOffsets,
}

#[cfg(target_os = "linux")]
#[repr(C)]
#[derive(Default, Clone, Copy)]
#[allow(dead_code)]
struct IoUringSqe {
    opcode: u8,
    flags: u8,
    ioprio: u16,
    fd: i32,
    off: u64,
    addr: u64,
    len: u32,
    rw_flags: u32,
    user_data: u64,
    buf_index: u16,
    personality: u16,
    splice_fd_in: i32,
    pad2: [u64; 2],
}

#[cfg(target_os = "linux")]
#[repr(C)]
#[derive(Default, Clone, Copy)]
#[allow(dead_code)]
struct IoUringCqe {
    user_data: u64,
    res: i32,
    flags: u32,
}

#[cfg(target_os = "linux")]
struct LinuxIoUring {
    ring_fd: i32,
    sq_ring_ptr: *mut libc::c_void,
    cq_ring_ptr: *mut libc::c_void,
    sqes_ptr: *mut libc::c_void,
    sq_ring_size: usize,
    cq_ring_size: usize,
    sqes_size: usize,
    sq_head: *mut u32,
    sq_tail: *mut u32,
    sq_ring_mask: *mut u32,
    sq_ring_entries: *mut u32,
    sq_array: *mut u32,
    sqes: *mut IoUringSqe,
    cq_head: *mut u32,
    cq_tail: *mut u32,
    cq_ring_mask: *mut u32,
    cqes: *mut IoUringCqe,
}

#[cfg(target_os = "linux")]
unsafe impl Send for LinuxIoUring {}

#[cfg(target_os = "linux")]
impl LinuxIoUring {
    fn init(entries: u32) -> Option<Self> {
        let mut params = IoUringParams::default();
        let ring_fd = unsafe { libc::syscall(libc::SYS_io_uring_setup, entries, &mut params) as i32 };
        if ring_fd < 0 {
            IO_URING_SETUP_FAILED.fetch_add(1, Ordering::Relaxed);
            return None;
        }

        let mut sq_ring_size = params.sq_off.array as usize + params.sq_entries as usize * std::mem::size_of::<u32>();
        let mut cq_ring_size = params.cq_off.cqes as usize + params.cq_entries as usize * std::mem::size_of::<IoUringCqe>();
        if params.features & IORING_FEAT_SINGLE_MMAP != 0 {
            sq_ring_size = sq_ring_size.max(cq_ring_size);
            cq_ring_size = sq_ring_size;
        }

        let sq_ring_ptr = unsafe { libc::mmap(std::ptr::null_mut(), sq_ring_size, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED | libc::MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING) };
        if sq_ring_ptr == libc::MAP_FAILED {
            unsafe { libc::close(ring_fd); }
            IO_URING_SETUP_FAILED.fetch_add(1, Ordering::Relaxed);
            return None;
        }

        let cq_ring_ptr = if params.features & IORING_FEAT_SINGLE_MMAP != 0 {
            sq_ring_ptr
        } else {
            let ptr = unsafe { libc::mmap(std::ptr::null_mut(), cq_ring_size, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED | libc::MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING) };
            if ptr == libc::MAP_FAILED {
                unsafe { libc::munmap(sq_ring_ptr, sq_ring_size); libc::close(ring_fd); }
                IO_URING_SETUP_FAILED.fetch_add(1, Ordering::Relaxed);
                return None;
            }
            ptr
        };

        let sqes_size = params.sq_entries as usize * std::mem::size_of::<IoUringSqe>();
        let sqes_ptr = unsafe { libc::mmap(std::ptr::null_mut(), sqes_size, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_SHARED | libc::MAP_POPULATE, ring_fd, IORING_OFF_SQES) };
        if sqes_ptr == libc::MAP_FAILED {
            unsafe {
                if cq_ring_ptr != sq_ring_ptr { libc::munmap(cq_ring_ptr, cq_ring_size); }
                libc::munmap(sq_ring_ptr, sq_ring_size);
                libc::close(ring_fd);
            }
            IO_URING_SETUP_FAILED.fetch_add(1, Ordering::Relaxed);
            return None;
        }

        let sq = sq_ring_ptr as *mut u8;
        let cq = cq_ring_ptr as *mut u8;
        IO_URING_SETUP_OK.fetch_add(1, Ordering::Relaxed);
        Some(Self {
            ring_fd,
            sq_ring_ptr,
            cq_ring_ptr,
            sqes_ptr,
            sq_ring_size,
            cq_ring_size,
            sqes_size,
            sq_head: unsafe { sq.add(params.sq_off.head as usize) as *mut u32 },
            sq_tail: unsafe { sq.add(params.sq_off.tail as usize) as *mut u32 },
            sq_ring_mask: unsafe { sq.add(params.sq_off.ring_mask as usize) as *mut u32 },
            sq_ring_entries: unsafe { sq.add(params.sq_off.ring_entries as usize) as *mut u32 },
            sq_array: unsafe { sq.add(params.sq_off.array as usize) as *mut u32 },
            sqes: sqes_ptr as *mut IoUringSqe,
            cq_head: unsafe { cq.add(params.cq_off.head as usize) as *mut u32 },
            cq_tail: unsafe { cq.add(params.cq_off.tail as usize) as *mut u32 },
            cq_ring_mask: unsafe { cq.add(params.cq_off.ring_mask as usize) as *mut u32 },
            cqes: unsafe { cq.add(params.cq_off.cqes as usize) as *mut IoUringCqe },
        })
    }

    fn read(&mut self, fd: i32, buffer: &mut [u8], offset: u64) -> bool {
        if self.ring_fd < 0 || buffer.is_empty() { return false; }
        unsafe {
            let head = std::ptr::read_volatile(self.sq_head);
            let tail = std::ptr::read_volatile(self.sq_tail);
            if tail.wrapping_sub(head) >= std::ptr::read_volatile(self.sq_ring_entries) { return false; }

            let index = tail & std::ptr::read_volatile(self.sq_ring_mask);
            let sqe = self.sqes.add(index as usize);
            std::ptr::write_bytes(sqe, 0, 1);
            (*sqe).opcode = IORING_OP_READ;
            (*sqe).fd = fd;
            (*sqe).addr = buffer.as_mut_ptr() as u64;
            (*sqe).len = buffer.len() as u32;
            (*sqe).off = offset;
            (*sqe).user_data = 1;
            *self.sq_array.add(index as usize) = index;
            std::sync::atomic::fence(Ordering::Release);
            std::ptr::write_volatile(self.sq_tail, tail.wrapping_add(1));

            if libc::syscall(libc::SYS_io_uring_enter, self.ring_fd, 1u32, 1u32, IORING_ENTER_GETEVENTS, std::ptr::null::<libc::c_void>(), 0usize) < 0 {
                return false;
            }

            loop {
                let cq_head = std::ptr::read_volatile(self.cq_head);
                let cq_tail = std::ptr::read_volatile(self.cq_tail);
                if cq_head != cq_tail {
                    let cqe = self.cqes.add((cq_head & std::ptr::read_volatile(self.cq_ring_mask)) as usize);
                    let result = (*cqe).res;
                    std::sync::atomic::fence(Ordering::Release);
                    std::ptr::write_volatile(self.cq_head, cq_head.wrapping_add(1));
                    return result == buffer.len() as i32;
                }
                if libc::syscall(libc::SYS_io_uring_enter, self.ring_fd, 0u32, 1u32, IORING_ENTER_GETEVENTS, std::ptr::null::<libc::c_void>(), 0usize) < 0 {
                    return false;
                }
            }
        }
    }
}

#[cfg(target_os = "linux")]
impl Drop for LinuxIoUring {
    fn drop(&mut self) {
        unsafe {
            if self.sqes_ptr != libc::MAP_FAILED { libc::munmap(self.sqes_ptr, self.sqes_size); }
            if self.cq_ring_ptr != libc::MAP_FAILED && self.cq_ring_ptr != self.sq_ring_ptr { libc::munmap(self.cq_ring_ptr, self.cq_ring_size); }
            if self.sq_ring_ptr != libc::MAP_FAILED { libc::munmap(self.sq_ring_ptr, self.sq_ring_size); }
            if self.ring_fd >= 0 { libc::close(self.ring_fd); }
        }
    }
}

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
pub const PATH_PREFIX_SIDECAR_PAGE_COUNT: usize = 10;
pub const PATH_PREFIX_SIDECAR_BYTES: usize = PATH_PREFIX_SIDECAR_PAGE_COUNT * PAGE_SIZE;
pub const PATH_PREFIX_SIDECAR_MAGIC: &[u8; 8] = b"MSPATHS\0";
pub const PATH_PREFIX_SIDECAR_VERSION: u16 = 1;
pub const INDEX_FILE_HEADER_SIZE: usize = 136;
pub const INDEX_FORMAT_VERSION: u32 = 20;
pub const INDEX_BLOCK_CONTINUATION_HEADER_SIZE: usize = 12;
pub const TERM_MPHF_MAGIC: u64 = 0x4850464d4d524554u64;
pub const TERM_MPHF_HEADER_SIZE: usize = 48;
pub const LEAF_TERM_ENTRY_SIZE: usize = 16;
pub const TERM_MPHF_ENTRY_SIZE: usize = 32;
pub const TERM_MPHF_ENTRIES_PER_PAGE: usize = PAGE_SIZE / TERM_MPHF_ENTRY_SIZE;
pub const DOC_VECTOR_OFFSET: usize = 64;
pub const DOC_PATH_OFFSET: usize = 192;

#[allow(non_snake_case)]
pub fn TermMphfHash(term: &[u8], seed: u64) -> u64 {
    let mut hash = 1469598103934665603u64 ^ seed;
    for byte in term {
        hash ^= *byte as u64;
        hash = hash.wrapping_mul(1099511628211u64);
    }
    hash ^= hash >> 32;
    hash = hash.wrapping_mul(0xd6e8feb86659fd93u64);
    hash ^= hash >> 32;
    hash
}

#[allow(non_snake_case)]
pub fn TermMphfSlotSeed(seed: u64, displacement: u32) -> u64 {
    let mut x = seed ^ 0x9e3779b97f4a7c15u64.wrapping_mul(displacement as u64 + 1);
    x ^= x >> 30;
    x = x.wrapping_mul(0xbf58476d1ce4e5b9u64);
    x ^= x >> 27;
    x = x.wrapping_mul(0x94d049bb133111ebu64);
    x ^= x >> 31;
    x
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(non_snake_case)]
pub struct TermMphfHeader {
    pub TMH_Magic: u64,
    pub TMH_TermCount: u64,
    pub TMH_BucketCount: u32,
    pub TMH_SlotCount: u32,
    pub TMH_BucketSeed: u64,
    pub TMH_SlotSeed: u64,
    pub TMH_FingerprintSeed: u64,
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(non_snake_case)]
pub struct PathPrefixSidecarHeader {
    pub PPSH_Magic: [u8; 8],
    pub PPSH_Version: u16,
    pub PPSH_PrefixCount: u16,
    pub PPSH_EntryOffset: u32,
    pub PPSH_StringOffset: u32,
    pub PPSH_StringBytes: u32,
    pub PPSH_Reserved: [u8; 8],
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(non_snake_case)]
pub struct PathPrefixSidecarEntry {
    pub PPSE_Offset: u32,
    pub PPSE_Length: u16,
    pub PPSE_Flags: u16,
}

#[allow(non_snake_case)]
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

#[repr(C)]
#[derive(Clone, Copy)]
#[allow(non_snake_case)]
pub struct IndexBlock {
    pub IB_Data: [u8; PAGE_SIZE],
}

impl Default for IndexBlock {
    fn default() -> Self { Self { IB_Data: [0; PAGE_SIZE] } }
}

#[repr(C)]
#[derive(Clone, Copy)]
#[allow(non_snake_case)]
pub struct LeafTermBlock {
    pub LTB_Directory: [u16; LEAF_TERM_DIRECTORY_COUNT],
    pub LTB_Data: [u8; PAGE_SIZE - LEAF_TERM_DATA_OFFSET],
}

const _: [(); PAGE_SIZE] = [(); std::mem::size_of::<IndexBlock>()];
const _: [(); PAGE_SIZE] = [(); std::mem::size_of::<LeafTermBlock>()];

impl Default for LeafTermBlock {
    fn default() -> Self {
        Self {
            LTB_Directory: [0; LEAF_TERM_DIRECTORY_COUNT],
            LTB_Data: [0; PAGE_SIZE - LEAF_TERM_DATA_OFFSET],
        }
    }
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(non_snake_case)]
pub struct IndexBlockContinuationHeader {
    pub IBCH_MaxDocID: u64,
    pub IBCH_DataLength: u32,
}

impl IndexBlockContinuationHeader {
    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < INDEX_BLOCK_CONTINUATION_HEADER_SIZE { return None; }
        Some(Self {
            IBCH_MaxDocID: u64::from_le_bytes(data[0..8].try_into().ok()?),
            IBCH_DataLength: u32::from_le_bytes(data[8..12].try_into().ok()?),
        })
    }

    pub fn write_to(&self, data: &mut [u8]) {
        data[0..8].copy_from_slice(&self.IBCH_MaxDocID.to_le_bytes());
        data[8..12].copy_from_slice(&self.IBCH_DataLength.to_le_bytes());
    }
}

#[derive(Debug, Clone)]
#[allow(non_snake_case)]
pub struct HeadTermEntry {
    pub HTE_LeafTermBlockID: u32,
    pub HTE_FirstTermLength: u16,
    pub HTE_FirstTerm: [u8; HEAD_TERM_KEY_MAX],
}

#[allow(non_snake_case)]
impl HeadTermEntry {
    pub fn new(term: &str, block_id: u32) -> Self {
        let bytes = term.as_bytes();
        let mut hteFirstTerm = [0u8; HEAD_TERM_KEY_MAX];
        hteFirstTerm[..bytes.len()].copy_from_slice(bytes);
        Self {
            HTE_LeafTermBlockID: block_id,
            HTE_FirstTermLength: bytes.len() as u16,
            HTE_FirstTerm: hteFirstTerm,
        }
    }

    pub fn first_term(&self) -> &str {
        let len = self.HTE_FirstTermLength as usize;
        std::str::from_utf8(&self.HTE_FirstTerm[..len]).unwrap_or("")
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
        let mut hteFirstTerm = [0u8; HEAD_TERM_KEY_MAX];
        hteFirstTerm.copy_from_slice(&data[6..32]);
        Some(Self {
            HTE_LeafTermBlockID: u32::from_le_bytes(data[0..4].try_into().ok()?),
            HTE_FirstTermLength: u16::from_le_bytes(data[4..6].try_into().ok()?),
            HTE_FirstTerm: hteFirstTerm,
        })
    }
}

#[derive(Debug, Clone)]
#[allow(non_snake_case)]
pub struct LeafTermEntry {
    pub LTE_Term: String,
    pub LTE_DocFreq: u32,
    pub LTE_IndexBlockID: u32,
    pub LTE_IndexOffset: u16,
    pub LTE_IndexLength: u16,
    pub LTE_ContinuationBlockCount: u16,
    pub LTE_Flags: u8,
}

impl LeafTermEntry {
    pub fn byte_len(&self) -> usize { LEAF_TERM_ENTRY_SIZE + self.LTE_Term.len() }
}

#[allow(non_snake_case)]
impl LeafTermBlock {
    pub fn entry_count(&self) -> usize {
        self.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] as usize
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
        let lteTerm = std::str::from_utf8(&data[16..16 + term_len]).ok()?.to_string();

        Some(LeafTermEntry {
            LTE_DocFreq: u32::from_le_bytes(data[0..4].try_into().ok()?),
            LTE_IndexBlockID: u32::from_le_bytes(data[4..8].try_into().ok()?),
            LTE_IndexOffset: u16::from_le_bytes(data[8..10].try_into().ok()?),
            LTE_IndexLength: u16::from_le_bytes(data[10..12].try_into().ok()?),
            LTE_ContinuationBlockCount: u16::from_le_bytes(data[12..14].try_into().ok()?),
            LTE_Flags: data[14],
            LTE_Term: lteTerm,
        })
    }

    pub fn entries(&self) -> Vec<LeafTermEntry> {
        (0..self.entry_count()).filter_map(|index| self.entry(index)).collect()
    }

    pub fn to_bytes(&self) -> [u8; PAGE_SIZE] {
        let mut out = [0u8; PAGE_SIZE];
        for (index, value) in self.LTB_Directory.iter().enumerate() {
            let offset = index * 2;
            out[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
        }
        out[LEAF_TERM_DATA_OFFSET..].copy_from_slice(&self.LTB_Data);
        out
    }

    pub fn from_bytes(data: &[u8]) -> Option<Self> {
        if data.len() < PAGE_SIZE { return None; }
        let mut block = LeafTermBlock::default();
        for index in 0..LEAF_TERM_DIRECTORY_COUNT {
            let offset = index * 2;
            block.LTB_Directory[index] = u16::from_le_bytes(data[offset..offset + 2].try_into().ok()?);
        }
        block.LTB_Data.copy_from_slice(&data[LEAF_TERM_DATA_OFFSET..PAGE_SIZE]);
        Some(block)
    }
}

pub struct BloomFilter;
impl BloomFilter {
    pub fn can_term_exist(&self, _term: &str) -> bool { true }
}

#[allow(non_snake_case)]
pub fn DocDataEncodeScore(value: f32) -> u16 {
    if value <= 0.0 { return 0; }
    if value >= 1.0 { return u16::MAX; }
    (value * 65535.0 + 0.5) as u16
}

#[allow(non_snake_case)]
pub fn DocDataDecodeScore(value: u16) -> f32 {
    value as f32 / 65535.0
}

#[derive(Clone, Copy)]
struct IndexSlotEntry {
    block_id: u32,
    ref_count: u32,
}

impl Default for IndexSlotEntry {
    fn default() -> Self { Self { block_id: u32::MAX, ref_count: 0 } }
}

pub struct PinnedBlock<T: Copy> {
    page: T,
}

impl<T: Copy> Deref for PinnedBlock<T> {
    type Target = T;
    fn deref(&self) -> &Self::Target { &self.page }
}

struct BlockCachePool<T: Default + Copy> {
    pages: Option<Arc<PinnedMemory<T>>>,
    path: Option<String>,
    base_offset: u64,
    total_block_count: u32,
    slot_count: u32,
    evict_slot: u32,
    logic_table: Option<PinnedMemory<u32>>,
    slot_table: Option<PinnedMemory<IndexSlotEntry>>,
    #[cfg(target_os = "linux")]
    io_uring: Option<LinuxIoUring>,
}

enum BlockWorkerRequest<T: Default + Copy + Send + Sync + 'static> {
    Get {
        block_seq: u32,
        reply: mpsc::Sender<Option<(u32, PinnedBlock<T>)>>,
    },
    Stop,
}

struct BlockCacheWorker<T: Default + Copy + Send + Sync + 'static> {
    sender: mpsc::Sender<BlockWorkerRequest<T>>,
    handle: Option<JoinHandle<()>>,
}

impl<T: Default + Copy + Send + Sync + 'static> BlockCacheWorker<T> {
    fn start(pool: Arc<Mutex<BlockCachePool<T>>>) -> Self {
        let (sender, receiver) = mpsc::channel::<BlockWorkerRequest<T>>();
        let handle = thread::spawn(move || {
            while let Ok(request) = receiver.recv() {
                match request {
                    BlockWorkerRequest::Get { block_seq, reply } => {
                        let result = pool.lock().ok().and_then(|mut pool| pool.read_miss(block_seq));
                        let _ = reply.send(result);
                    }
                    BlockWorkerRequest::Stop => break,
                }
            }
        });
        Self { sender, handle: Some(handle) }
    }

    fn get(&self, block_seq: u32) -> Option<(u32, PinnedBlock<T>)> {
        let (reply, receiver) = mpsc::channel();
        self.sender.send(BlockWorkerRequest::Get { block_seq, reply }).ok()?;
        receiver.recv().ok().flatten()
    }

    fn stop(&mut self) {
        let _ = self.sender.send(BlockWorkerRequest::Stop);
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

impl<T: Default + Copy + Send + Sync + 'static> Drop for BlockCacheWorker<T> {
    fn drop(&mut self) {
        self.stop();
    }
}

#[allow(non_snake_case)]
impl<T: Default + Copy> BlockCachePool<T> {
    fn new() -> Self {
        Self {
            pages: None,
            path: None,
            base_offset: 0,
            total_block_count: 0,
            slot_count: 0,
            evict_slot: 0,
            logic_table: None,
            slot_table: None,
            #[cfg(target_os = "linux")]
            io_uring: None,
        }
    }

    fn reset_tables(&mut self, total_block_count: u32, slot_count: u32) {
        self.total_block_count = total_block_count;
        self.slot_count = slot_count.min(total_block_count);
        self.evict_slot = self.slot_count;
        self.logic_table = Some(PinnedMemory::from_slice(&vec![u32::MAX; self.total_block_count as usize]));
        self.slot_table = Some(PinnedMemory::from_slice(&vec![IndexSlotEntry::default(); self.slot_count as usize]));
    }

    fn set_pages(&mut self, pages: Vec<T>) {
        self.path = None;
        self.base_offset = 0;
        #[cfg(target_os = "linux")]
        {
            self.io_uring = None;
        }
        self.reset_tables(pages.len() as u32, pages.len() as u32);
        self.pages = Some(Arc::new(PinnedMemory::from_slice(&pages)));

        for block in 0..self.slot_count {
            self.logic_table.as_mut().unwrap()[block as usize] = block;
            self.slot_table.as_mut().unwrap()[block as usize].block_id = block;
        }
    }

    fn InitFileBacked(&mut self,
                        path: String,
                        base_offset: u64,
                        total_block_count: u32,
                        slot_count: u32) -> std::io::Result<()>
    {
        self.path = Some(path.clone());
        self.base_offset = base_offset;
        self.reset_tables(total_block_count, slot_count);
        #[cfg(target_os = "linux")]
        {
            self.io_uring = LinuxIoUring::init(64);
        }
        let mut pages = PinnedMemory::<T>::new_zeroed(self.slot_count as usize);

        let mut file = File::open(path)?;
        for block in 0..self.slot_count {
            file.seek(SeekFrom::Start(self.base_offset + block as u64 * PAGE_SIZE as u64))?;
            let page = &mut pages.as_mut_slice()[block as usize];
            let bytes = unsafe {
                std::slice::from_raw_parts_mut(page as *mut T as *mut u8, PAGE_SIZE)
            };
            file.read_exact(bytes)?;
            self.logic_table.as_mut().unwrap()[block as usize] = block;
            self.slot_table.as_mut().unwrap()[block as usize].block_id = block;
        }

        self.pages = Some(Arc::new(pages));
        Ok(())
    }

    fn get(&mut self, block_seq: u32) -> Option<(u32, PinnedBlock<T>)> {
        let logic_table = self.logic_table.as_ref()?;
        let slot = *logic_table.as_slice().get(block_seq as usize)?;
        if slot == u32::MAX { return None; }
        let page = self.pages.as_ref()?.as_slice()[slot as usize];
        Some((slot, PinnedBlock { page }))
    }

    fn read_miss(&mut self, block_seq: u32) -> Option<(u32, PinnedBlock<T>)> {
        if block_seq >= self.total_block_count || self.slot_count == 0 { return None; }
        let path = self.path.as_ref()?.clone();
        let mut found = u32::MAX;
        for _scanned in 0..self.slot_count {
            let candidate = self.evict_slot % self.slot_count;
            self.evict_slot = self.evict_slot.wrapping_add(1);
            if self.slot_table.as_ref()?.as_slice()[candidate as usize].ref_count == 0 {
                found = candidate;
                break;
            }
        }
        if found == u32::MAX { return None; }

        let old_block = self.slot_table.as_ref()?.as_slice()[found as usize].block_id;
        if old_block != u32::MAX {
            self.logic_table.as_mut()?.as_mut_slice()[old_block as usize] = u32::MAX;
        }

        let mut raw_page = [0u8; PAGE_SIZE];
        self.read_block_bytes(&path, block_seq, &mut raw_page)?;
        let pages = Arc::get_mut(self.pages.as_mut()?)?;
        let page = &mut pages.as_mut_slice()[found as usize];
        unsafe {
            std::ptr::copy_nonoverlapping(raw_page.as_ptr(), page as *mut T as *mut u8, PAGE_SIZE);
        }

        self.slot_table.as_mut()?.as_mut_slice()[found as usize].block_id = block_seq;
        self.slot_table.as_mut()?.as_mut_slice()[found as usize].ref_count = 0;
        self.logic_table.as_mut()?.as_mut_slice()[block_seq as usize] = found;
        let page = self.pages.as_ref()?.as_slice()[found as usize];
        Some((found, PinnedBlock { page }))
    }

    fn read_block_bytes(&mut self, path: &str, block_seq: u32, bytes: &mut [u8]) -> Option<()> {
        let offset = self.base_offset + block_seq as u64 * PAGE_SIZE as u64;
        #[cfg(target_os = "linux")]
        {
            if let Some(ring) = self.io_uring.as_mut() {
                if let Ok(file) = File::open(path) {
                    if ring.read(file.as_raw_fd(), bytes, offset) {
                        IO_URING_READS.fetch_add(1, Ordering::Relaxed);
                        return Some(());
                    }
                }
            }
            PREAD_FALLBACK_READS.fetch_add(1, Ordering::Relaxed);
        }

        let mut file = File::open(path).ok()?;
        file.seek(SeekFrom::Start(offset)).ok()?;
        file.read_exact(bytes).ok()?;
        Some(())
    }

}

pub struct IndexLocation {
    pub index_block_id: u32,
    pub index_offset: usize,
    pub index_length: usize,
    pub doc_freq: u32,
    pub continuation_block_count: u32,
}

pub struct IndexBlockTable {
    index_pool: Arc<Mutex<BlockCachePool<IndexBlock>>>,
    leaf_term_pool: Arc<Mutex<BlockCachePool<LeafTermBlock>>>,
    index_worker: Mutex<Option<BlockCacheWorker<IndexBlock>>>,
    leaf_term_worker: Mutex<Option<BlockCacheWorker<LeafTermBlock>>>,
    head_term_entries: Vec<HeadTermEntry>,
    term_mphf_header: TermMphfHeader,
    term_mphf_displacements: Vec<i32>,
    term_mphf_entry_pages: Vec<IndexBlock>,
    bloom: BloomFilter,
}

#[allow(non_snake_case)]
impl IndexBlockTable {
    pub fn new(_capacity: usize) -> Self {
        Self {
            index_pool: Arc::new(Mutex::new(BlockCachePool::new())),
            leaf_term_pool: Arc::new(Mutex::new(BlockCachePool::new())),
            index_worker: Mutex::new(None),
            leaf_term_worker: Mutex::new(None),
            head_term_entries: Vec::new(),
            term_mphf_header: TermMphfHeader::default(),
            term_mphf_displacements: Vec::new(),
            term_mphf_entry_pages: Vec::new(),
            bloom: BloomFilter,
        }
    }

    pub fn SetIndexBlocks(&mut self, blocks: Vec<IndexBlock>) {
        self.StopWorkers();
        self.index_pool.lock().unwrap().set_pages(blocks);
    }

    pub fn SetLeafTermBlocks(&mut self, blocks: Vec<LeafTermBlock>) {
        self.StopWorkers();
        self.leaf_term_pool.lock().unwrap().set_pages(blocks);
    }

    pub fn InitFileBacked(&mut self,
                            path: &str,
                            index_base_offset: u64,
                            index_block_count: u32,
                            index_slot_count: u32,
                            leaf_base_offset: u64,
                            leaf_block_count: u32,
                            leaf_slot_count: u32) -> std::io::Result<()> {
        self.StopWorkers();
        self.index_pool.lock().unwrap().InitFileBacked(
            path.to_string(), index_base_offset, index_block_count, index_slot_count)?;
        self.leaf_term_pool.lock().unwrap().InitFileBacked(
            path.to_string(), leaf_base_offset, leaf_block_count, leaf_slot_count)?;
        self.StartWorkers();
        Ok(())
    }

    pub fn InsertBlock(&mut self, seq: u32, block: IndexBlock) {
        let mut pages: Vec<IndexBlock> = self.index_pool
            .lock().unwrap()
            .pages
            .as_ref()
            .map(|pages| pages.as_slice().to_vec())
            .unwrap_or_default();
        if pages.len() <= seq as usize { pages.resize_with(seq as usize + 1, IndexBlock::default); }
        pages[seq as usize] = block;
        self.SetIndexBlocks(pages);
    }

    pub fn SetHeadLeafTermTable(&mut self, head: Vec<HeadTermEntry>, blocks: Vec<LeafTermBlock>) {
        self.head_term_entries = head;
        self.SetLeafTermBlocks(blocks);
    }

    pub fn SetHeadEntries(&mut self, head: Vec<HeadTermEntry>) {
        self.head_term_entries = head;
    }

    pub fn SetHeadTermEntries(&mut self, head: Vec<HeadTermEntry>) {
        self.SetHeadEntries(head);
    }

    pub fn HandOverBlockTable(&mut self, source: &mut IndexBlockTable) {
        if std::ptr::eq(self, source) { return; }
        self.StopWorkers();
        source.StopWorkers();
        self.index_pool = std::mem::replace(&mut source.index_pool, Arc::new(Mutex::new(BlockCachePool::new())));
        self.leaf_term_pool = std::mem::replace(&mut source.leaf_term_pool, Arc::new(Mutex::new(BlockCachePool::new())));
        self.head_term_entries = std::mem::take(&mut source.head_term_entries);
        self.term_mphf_header = source.term_mphf_header;
        self.term_mphf_displacements = std::mem::take(&mut source.term_mphf_displacements);
        self.term_mphf_entry_pages = std::mem::take(&mut source.term_mphf_entry_pages);
        source.term_mphf_header = TermMphfHeader::default();
        self.bloom = BloomFilter;
    }

    pub fn SetTermMphf(&mut self, header: TermMphfHeader, displacements: Vec<i32>, entry_pages: Vec<IndexBlock>) {
        if header.TMH_TermCount == 0 || header.TMH_BucketCount == 0 || header.TMH_SlotCount == 0 || displacements.is_empty() || entry_pages.is_empty() {
            self.term_mphf_header = TermMphfHeader::default();
            self.term_mphf_displacements.clear();
            self.term_mphf_entry_pages.clear();
            return;
        }
        let required_bytes = header.TMH_SlotCount as usize * TERM_MPHF_ENTRY_SIZE;
        let available_bytes = entry_pages.len() * PAGE_SIZE;
        if header.TMH_Magic != TERM_MPHF_MAGIC
            || header.TMH_SlotCount as u64 != header.TMH_TermCount
            || displacements.len() != header.TMH_BucketCount as usize
            || required_bytes > available_bytes
        {
            self.term_mphf_header = TermMphfHeader::default();
            self.term_mphf_displacements.clear();
            self.term_mphf_entry_pages.clear();
            return;
        }
        self.term_mphf_header = header;
        self.term_mphf_displacements = displacements;
        self.term_mphf_entry_pages = entry_pages;
    }

    pub fn TermMphfHeader(&self) -> &TermMphfHeader { &self.term_mphf_header }
    pub fn TermMphfDisplacements(&self) -> &[i32] { &self.term_mphf_displacements }
    pub fn TermMphfEntryPages(&self) -> &[IndexBlock] { &self.term_mphf_entry_pages }

    fn StartWorkers(&self) {
        let mut index_worker = self.index_worker.lock().unwrap();
        if index_worker.is_none() {
            *index_worker = Some(BlockCacheWorker::start(Arc::clone(&self.index_pool)));
        }
        drop(index_worker);

        let mut leaf_worker = self.leaf_term_worker.lock().unwrap();
        if leaf_worker.is_none() {
            *leaf_worker = Some(BlockCacheWorker::start(Arc::clone(&self.leaf_term_pool)));
        }
    }

    fn StopWorkers(&self) {
        if let Ok(mut worker) = self.index_worker.lock() {
            if let Some(mut worker) = worker.take() {
                worker.stop();
            }
        }
        if let Ok(mut worker) = self.leaf_term_worker.lock() {
            if let Some(mut worker) = worker.take() {
                worker.stop();
            }
        }
    }

    fn GetIndexBlock(&self, seq: u32) -> Option<(u32, PinnedBlock<IndexBlock>)> {
        if let Some(hit) = self.index_pool.lock().ok()?.get(seq) {
            return Some(hit);
        }
        if let Some(worker) = self.index_worker.lock().ok()?.as_ref() {
            return worker.get(seq);
        }
        self.index_pool.lock().ok()?.read_miss(seq)
    }

    fn GetLeafTermBlock(&self, seq: u32) -> Option<(u32, PinnedBlock<LeafTermBlock>)> {
        if let Some(hit) = self.leaf_term_pool.lock().ok()?.get(seq) {
            return Some(hit);
        }
        if let Some(worker) = self.leaf_term_worker.lock().ok()?.as_ref() {
            return worker.get(seq);
        }
        self.leaf_term_pool.lock().ok()?.read_miss(seq)
    }

    pub fn HeadTermEntries(&self) -> &[HeadTermEntry] {
        &self.head_term_entries
    }

    pub fn IndexBlocks(&self) -> Vec<IndexBlock> {
        self.index_pool
            .lock().unwrap()
            .pages
            .as_ref()
            .map(|pages| pages.as_slice().to_vec())
            .unwrap_or_default()
    }

    pub fn LeafTermBlocks(&self) -> Vec<LeafTermBlock> {
        self.leaf_term_pool
            .lock().unwrap()
            .pages
            .as_ref()
            .map(|pages| pages.as_slice().to_vec())
            .unwrap_or_default()
    }

    pub fn FindTermData(&self, term: &str) -> Option<(IndexLocation, PinnedBlock<IndexBlock>)> {
        if !self.bloom.can_term_exist(term) { return None; }
        if self.HasTermMphf() {
            if let Some(result) = self.FindTermDataMphf(term) {
                return Some(result);
            }
        }
        self.FindTermDataHeadLeaf(term)
    }

    fn FindTermDataHeadLeaf(&self, term: &str) -> Option<(IndexLocation, PinnedBlock<IndexBlock>)> {
        if self.head_term_entries.is_empty() { return None; }

        let pos = self.head_term_entries.partition_point(|entry| entry.first_term() <= term);
        if pos == 0 { return None; }
        let leaf_block_id = self.head_term_entries[pos - 1].HTE_LeafTermBlockID;
        let (_leaf_slot, leaf_block) = self.GetLeafTermBlock(leaf_block_id)?;

        let entry_count = leaf_block.entry_count();
        let mut left = 0usize;
        let mut right = entry_count;
        while left < right {
            let mid = left + (right - left) / 2;
            let entry = leaf_block.entry(mid)?;
            if entry.LTE_Term.as_str() < term { left = mid + 1; }
            else { right = mid; }
        }

        if left == entry_count { return None; }
        let entry = leaf_block.entry(left)?;
        if entry.LTE_Term != term { return None; }

        let (_index_slot, index_block) = self.GetIndexBlock(entry.LTE_IndexBlockID)?;
        Some((IndexLocation {
            index_block_id: entry.LTE_IndexBlockID,
            index_offset: entry.LTE_IndexOffset as usize,
            index_length: entry.LTE_IndexLength as usize,
            doc_freq: entry.LTE_DocFreq,
            continuation_block_count: entry.LTE_ContinuationBlockCount as u32,
        }, index_block))
    }

    fn HasTermMphf(&self) -> bool {
        self.term_mphf_header.TMH_Magic == TERM_MPHF_MAGIC
            && self.term_mphf_header.TMH_TermCount > 0
            && self.term_mphf_header.TMH_BucketCount > 0
            && self.term_mphf_header.TMH_SlotCount > 0
            && self.term_mphf_header.TMH_SlotCount as u64 == self.term_mphf_header.TMH_TermCount
            && self.term_mphf_displacements.len() == self.term_mphf_header.TMH_BucketCount as usize
            && self.term_mphf_entry_pages.len() * PAGE_SIZE >= self.term_mphf_header.TMH_SlotCount as usize * TERM_MPHF_ENTRY_SIZE
    }

    fn FindTermDataMphf(&self, term: &str) -> Option<(IndexLocation, PinnedBlock<IndexBlock>)> {
        let bytes = term.as_bytes();
        let bucket = (TermMphfHash(bytes, self.term_mphf_header.TMH_BucketSeed) % self.term_mphf_header.TMH_BucketCount as u64) as usize;
        let displacement = *self.term_mphf_displacements.get(bucket)?;
        let slot = if displacement < 0 {
            (-(displacement as i64) - 1) as u64
        } else {
            TermMphfHash(bytes, TermMphfSlotSeed(self.term_mphf_header.TMH_SlotSeed, displacement as u32)) % self.term_mphf_header.TMH_SlotCount as u64
        };
        if slot >= self.term_mphf_header.TMH_SlotCount as u64 { return None; }
        let byte_offset = slot as usize * TERM_MPHF_ENTRY_SIZE;
        let page = byte_offset / PAGE_SIZE;
        let offset = byte_offset % PAGE_SIZE;
        let block = self.term_mphf_entry_pages.get(page)?;
        if offset + TERM_MPHF_ENTRY_SIZE > PAGE_SIZE { return None; }
        let data = &block.IB_Data[offset..offset + TERM_MPHF_ENTRY_SIZE];
        let mut fingerprint = TermMphfHash(bytes, self.term_mphf_header.TMH_FingerprintSeed);
        if fingerprint == 0 { fingerprint = 1; }
        if u64::from_le_bytes(data[24..32].try_into().ok()?) != fingerprint { return None; }

        let index_block_id = u32::from_le_bytes(data[4..8].try_into().ok()?);
        let index_block = self.GetIndexBlock(index_block_id)?.1;
        Some((IndexLocation {
            doc_freq: u32::from_le_bytes(data[0..4].try_into().ok()?),
            index_block_id,
            index_offset: u32::from_le_bytes(data[8..12].try_into().ok()?) as usize,
            index_length: u32::from_le_bytes(data[12..16].try_into().ok()?) as usize,
            continuation_block_count: u32::from_le_bytes(data[16..20].try_into().ok()?),
        }, index_block))
    }

    pub fn GetBlockBySeq(&self, seq: u32) -> Option<PinnedBlock<IndexBlock>> {
        self.GetIndexBlock(seq).map(|(_, block)| block)
    }

    pub fn GetLeafBlockBySeq(&self, seq: u32) -> Option<PinnedBlock<LeafTermBlock>> {
        self.GetLeafTermBlock(seq).map(|(_, block)| block)
    }

    pub fn LeafTermBlockCount(&self) -> u32 {
        self.leaf_term_pool.lock().unwrap().total_block_count
    }

    pub fn PostingBytes(&self, entry: &LeafTermEntry) -> Option<Vec<u8>> {
        let first = self.GetBlockBySeq(entry.LTE_IndexBlockID)?;
        let begin = entry.LTE_IndexOffset as usize;
        let end = begin.checked_add(entry.LTE_IndexLength as usize)?;
        if end > PAGE_SIZE { return None; }

        let mut bytes = Vec::new();
        bytes.extend_from_slice(&first.IB_Data[begin..end]);

        for i in 0..entry.LTE_ContinuationBlockCount as u32 {
            let block = self.GetBlockBySeq(entry.LTE_IndexBlockID + 1 + i)?;
            let header = IndexBlockContinuationHeader::from_bytes(&block.IB_Data)?;
            let data_begin = INDEX_BLOCK_CONTINUATION_HEADER_SIZE;
            let data_end = data_begin.checked_add(header.IBCH_DataLength as usize)?;
            if data_end > PAGE_SIZE { return None; }
            bytes.extend_from_slice(&block.IB_Data[data_begin..data_end]);
        }

        Some(bytes)
    }

    pub fn AllLeafTermEntries(&self) -> Vec<LeafTermEntry> {
        self.LeafTermBlocks().iter().flat_map(|block| block.entries()).collect()
    }
}
