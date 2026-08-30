//! Direct translation of the C++ file-access surface; names stay aligned for API parity.
#![allow(non_snake_case, non_upper_case_globals)]

use std::sync::{
    atomic::{AtomicU64, Ordering},
    Mutex,
};

#[cfg(not(target_arch = "wasm32"))]
use std::fs::{File, OpenOptions};
#[cfg(not(target_arch = "wasm32"))]
use std::io::{Seek, SeekFrom, Write};
#[cfg(all(unix, not(target_arch = "wasm32")))]
use std::os::unix::fs::FileExt;
#[cfg(all(windows, not(target_arch = "wasm32")))]
use std::os::windows::fs::FileExt;

#[cfg(all(target_arch = "wasm32", feature = "wasm"))]
#[wasm_bindgen::prelude::wasm_bindgen(inline_js = r#"
export function moonshot_browser_read(offset, destination) {
    const file = globalThis.__moonshotIndexFile;
    if (!file || typeof FileReaderSync === 'undefined') return false;
    const bytes = new Uint8Array(new FileReaderSync().readAsArrayBuffer(
        file.slice(Number(offset), Number(offset) + destination.length)));
    if (bytes.length !== destination.length) return false;
    destination.set(bytes);
    return true;
}
"#)]
extern "C" {
    fn moonshot_browser_read(offset: f64, destination: &mut [u8]) -> bool;
}

#[cfg(target_arch = "wasm32")]
pub(crate) fn browser_read_range(offset: u64, destination: &mut [u8]) -> bool {
    #[cfg(feature = "wasm")]
    {
        moonshot_browser_read(offset as f64, destination)
    }
    #[cfg(not(feature = "wasm"))]
    {
        let _ = (offset, destination);
        false
    }
}

pub struct FileAccess {
    #[cfg(not(target_arch = "wasm32"))]
    m_FileHandle: Option<Mutex<File>>,
    // Intentional Rust safety difference: serialize OS-cursor operations and
    // logical-position reads instead of reproducing the C++ data race.
    m_SequentialAccess: Mutex<()>,
    m_Position: AtomicU64,
    #[cfg(not(target_arch = "wasm32"))]
    m_FileName: String,
}

#[derive(Debug, Clone, Copy, Default)]
#[allow(non_snake_case)]
pub struct IoStats {
    pub IoUringReads: u64,
    pub PreadFallbackReads: u64,
    pub IoUringSetupOk: u64,
    pub IoUringSetupFailed: u64,
}

static PREAD_FALLBACK_READS: AtomicU64 = AtomicU64::new(0);

impl FileAccess {
    pub fn new(fileName: &str) -> Self {
        #[cfg(target_arch = "wasm32")]
        let _ = fileName;

        Self {
            #[cfg(not(target_arch = "wasm32"))]
            m_FileHandle: None,
            m_SequentialAccess: Mutex::new(()),
            m_Position: AtomicU64::new(0),
            #[cfg(not(target_arch = "wasm32"))]
            m_FileName: fileName.to_string(),
        }
    }

    pub fn Init(&mut self) -> bool {
        #[cfg(target_arch = "wasm32")]
        {
            true
        }
        #[cfg(not(target_arch = "wasm32"))]
        {
            match File::open(&self.m_FileName) {
                Ok(file) => {
                    self.m_FileHandle = Some(Mutex::new(file));
                    true
                }
                Err(_) => false,
            }
        }
    }

    pub fn GetIoStats() -> IoStats {
        IoStats {
            PreadFallbackReads: PREAD_FALLBACK_READS.load(Ordering::Relaxed),
            ..IoStats::default()
        }
    }

    pub fn InitWrite(&mut self, truncate: bool) -> bool {
        #[cfg(target_arch = "wasm32")]
        {
            let _ = truncate;
            false
        }
        #[cfg(not(target_arch = "wasm32"))]
        {
            match OpenOptions::new()
                .read(true)
                .write(true)
                .create(true)
                .truncate(truncate)
                .open(&self.m_FileName)
            {
                Ok(file) => {
                    self.m_FileHandle = Some(Mutex::new(file));
                    true
                }
                Err(_) => false,
            }
        }
    }

    pub fn GetData(&self, buffer: &mut [u8], numBytes: i32) -> i32 {
        if numBytes < 0 {
            return -1;
        }
        let Ok(_sequential) = self.m_SequentialAccess.lock() else {
            return -1;
        };
        let requested = (numBytes as usize).min(buffer.len());
        let position = self.m_Position.load(Ordering::Relaxed);
        let count = self.ReadAt(position, &mut buffer[..requested]);
        if count > 0 {
            self.m_Position
                .store(position + count as u64, Ordering::Relaxed);
        }
        count
    }

    pub fn PutData(&self, buffer: &[u8]) -> bool {
        if buffer.is_empty() {
            return true;
        }
        let Ok(_sequential) = self.m_SequentialAccess.lock() else {
            return false;
        };

        #[cfg(target_arch = "wasm32")]
        {
            false
        }

        #[cfg(not(target_arch = "wasm32"))]
        {
            let Some(file) = self.m_FileHandle.as_ref() else {
                return false;
            };
            let Ok(mut file) = file.lock() else {
                return false;
            };
            let mut offset = 0usize;
            while offset < buffer.len() {
                let end = offset.saturating_add(i32::MAX as usize).min(buffer.len());
                match file.write(&buffer[offset..end]) {
                    Ok(count) if count == end - offset => offset = end,
                    _ => return false,
                }
            }

            self.m_Position
                .fetch_add(buffer.len() as u64, Ordering::Relaxed);
            true
        }
    }

    pub fn ReadBlock(
        &self,
        block_seq: u32,
        buffer: &mut [u8],
        block_size: usize,
        base_byte_offset: u64,
    ) -> bool {
        if block_size > buffer.len() {
            return false;
        }
        let position = base_byte_offset + block_seq as u64 * block_size as u64;
        #[cfg(all(unix, not(target_arch = "wasm32")))]
        if self.m_FileHandle.is_some() {
            PREAD_FALLBACK_READS.fetch_add(1, Ordering::Relaxed);
        }
        self.ReadAt(position, &mut buffer[..block_size]) == block_size as i32
    }

    pub fn WriteBlock(&self, block_seq: u32, buffer: &[u8], block_size: usize) -> bool {
        if block_size > buffer.len() {
            return false;
        }
        let Ok(_sequential) = self.m_SequentialAccess.lock() else {
            return false;
        };
        let position = block_seq as u64 * block_size as u64;
        #[cfg(target_arch = "wasm32")]
        {
            let _ = position;
            false
        }
        #[cfg(not(target_arch = "wasm32"))]
        {
            let Some(file) = self.m_FileHandle.as_ref() else {
                return false;
            };
            let Ok(mut file) = file.lock() else {
                return false;
            };
            file.seek(SeekFrom::Start(position)).is_ok()
                && matches!(file.write(&buffer[..block_size]), Ok(count) if count == block_size)
        }
    }

    pub fn SetPosition(&self, position: u64) -> bool {
        let Ok(_sequential) = self.m_SequentialAccess.lock() else {
            return false;
        };
        self.m_Position.store(position, Ordering::Relaxed);
        #[cfg(target_arch = "wasm32")]
        {
            true
        }
        #[cfg(not(target_arch = "wasm32"))]
        {
            let Some(file) = self.m_FileHandle.as_ref() else {
                return false;
            };
            let Ok(mut file) = file.lock() else {
                return false;
            };
            file.seek(SeekFrom::Start(position)).is_ok()
        }
    }

    fn ReadAt(&self, position: u64, buffer: &mut [u8]) -> i32 {
        #[cfg(target_arch = "wasm32")]
        {
            if browser_read_range(position, buffer) {
                buffer.len() as i32
            } else {
                -1
            }
        }
        #[cfg(all(unix, not(target_arch = "wasm32")))]
        {
            let Some(file) = self.m_FileHandle.as_ref() else {
                return -1;
            };
            let Ok(file) = file.lock() else {
                return -1;
            };
            file.read_at(buffer, position)
                .map(|count| count as i32)
                .unwrap_or(-1)
        }
        #[cfg(all(windows, not(target_arch = "wasm32")))]
        {
            let Some(file) = self.m_FileHandle.as_ref() else {
                return -1;
            };
            let Ok(file) = file.lock() else {
                return -1;
            };
            file.seek_read(buffer, position)
                .map(|count| count as i32)
                .unwrap_or(-1)
        }
    }
}
