use std::sync::Arc;

use crate::file_access::FileAccess;

pub struct FileBlockManager {
    m_FileAccess: Option<Arc<FileAccess>>,
    m_BlockSize: usize,
    m_BaseOffset: u64,
    m_Memory: Option<Arc<[u8]>>,
}

#[allow(non_snake_case)]
impl FileBlockManager {
    pub fn new(block_size: usize, base_offset: u64) -> Self {
        Self { m_FileAccess: None, m_BlockSize: block_size, m_BaseOffset: base_offset, m_Memory: None }
    }
    pub fn open(&mut self, filename: &str) -> bool {
        self.m_Memory = None;
        self.m_FileAccess = None;
        let mut file = FileAccess::new(filename);
        if !file.Init() { return false; }
        self.m_FileAccess = Some(Arc::new(file));
        true
    }
    pub fn openWrite(&mut self, filename: &str) -> bool {
        self.m_Memory = None;
        self.m_FileAccess = None;
        let mut file = FileAccess::new(filename);
        if !file.InitWrite(true) { return false; }
        self.m_FileAccess = Some(Arc::new(file));
        true
    }
    pub fn openMemory(&mut self, memory: Arc<[u8]>) -> bool {
        if memory.len() < self.m_BlockSize { return false; }
        self.m_FileAccess = None;
        self.m_Memory = Some(memory);
        true
    }
    pub fn close(&mut self) { self.m_FileAccess = None; self.m_Memory = None; }
    pub fn read(&self, block_seq: u32, buffer: &mut [u8]) -> bool {
        if buffer.len() < self.m_BlockSize { return false; }
        if let Some(memory) = self.m_Memory.as_ref() {
            let offset = block_seq as usize * self.m_BlockSize;
            let Some(source) = memory.get(offset..offset + self.m_BlockSize) else { return false; };
            buffer[..self.m_BlockSize].copy_from_slice(source);
            return true;
        }
        self.m_FileAccess.as_ref().map(|file| file.ReadBlock(block_seq, buffer, self.m_BlockSize, self.m_BaseOffset)).unwrap_or(false)
    }
    pub fn write(&self, block_seq: u32, buffer: &[u8]) -> bool {
        if buffer.len() < self.m_BlockSize { return false; }
        self.m_FileAccess.as_ref()
            .map(|file| file.WriteBlock(block_seq, buffer, self.m_BlockSize))
            .unwrap_or(false)
    }
    pub fn getBlockSize(&self) -> usize { self.m_BlockSize }
    pub fn setBlockSize(&mut self, block_size: usize) { self.m_BlockSize = block_size; }
    pub fn getBaseOffset(&self) -> u64 { self.m_BaseOffset }
    pub fn setBaseOffset(&mut self, base_offset: u64) { self.m_BaseOffset = base_offset; }
}

impl Default for FileBlockManager { fn default() -> Self { Self::new(4096, 0) } }
