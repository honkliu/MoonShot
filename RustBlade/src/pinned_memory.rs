use std::marker::PhantomData;
use std::ops::{Index, IndexMut};
use std::ptr::NonNull;

pub struct PinnedMemory<T: Copy> {
    ptr: NonNull<T>,
    len: usize,
    #[cfg(not(target_arch = "wasm32"))]
    bytes: usize,
    #[cfg(target_arch = "wasm32")]
    #[allow(dead_code)]
    buffer: Box<[T]>,
    _marker: PhantomData<T>,
}

unsafe impl<T: Copy + Send> Send for PinnedMemory<T> {}
unsafe impl<T: Copy + Sync> Sync for PinnedMemory<T> {}

impl<T: Copy + Default> PinnedMemory<T> {
    pub fn new_zeroed(len: usize) -> Self {
        #[cfg(target_arch = "wasm32")]
        {
            let mut buffer = vec![T::default(); len].into_boxed_slice();
            let ptr = NonNull::new(buffer.as_mut_ptr()).expect("Box returned null");
            return Self { ptr, len, buffer, _marker: PhantomData };
        }

        #[cfg(not(target_arch = "wasm32"))]
        unsafe {
            let bytes = len * std::mem::size_of::<T>();
            let raw = os_alloc(bytes);
            let ptr = NonNull::new(raw as *mut T).expect("pinned allocation failed");
            std::ptr::write_bytes(ptr.as_ptr(), 0, len);
            Self { ptr, len, bytes, _marker: PhantomData }
        }
    }

    pub fn from_slice(values: &[T]) -> Self {
        let mut memory = Self::new_zeroed(values.len());
        memory.as_mut_slice().copy_from_slice(values);
        memory
    }
}

impl<T: Copy> PinnedMemory<T> {
    pub fn len(&self) -> usize { self.len }
    pub fn is_empty(&self) -> bool { self.len == 0 }

    pub fn as_slice(&self) -> &[T] {
        unsafe { std::slice::from_raw_parts(self.ptr.as_ptr(), self.len) }
    }

    pub fn as_mut_slice(&mut self) -> &mut [T] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr.as_ptr(), self.len) }
    }
}

impl<T: Copy> Index<usize> for PinnedMemory<T> {
    type Output = T;
    fn index(&self, index: usize) -> &Self::Output { &self.as_slice()[index] }
}

impl<T: Copy> IndexMut<usize> for PinnedMemory<T> {
    fn index_mut(&mut self, index: usize) -> &mut Self::Output { &mut self.as_mut_slice()[index] }
}

impl<T: Copy> Drop for PinnedMemory<T> {
    fn drop(&mut self) {
        #[cfg(not(target_arch = "wasm32"))]
        unsafe { os_free(self.ptr.as_ptr() as *mut u8, self.bytes); }
    }
}

#[cfg(windows)]
unsafe fn os_alloc(bytes: usize) -> *mut u8 {
    use windows_sys::Win32::System::Memory::{VirtualAlloc, VirtualLock, MEM_COMMIT, MEM_RESERVE, PAGE_READWRITE};
    let ptr = VirtualAlloc(std::ptr::null_mut(), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) as *mut u8;
    if !ptr.is_null() {
        let _ = VirtualLock(ptr as *const _, bytes);
    }
    ptr
}

#[cfg(windows)]
unsafe fn os_free(ptr: *mut u8, bytes: usize) {
    use windows_sys::Win32::System::Memory::{VirtualFree, VirtualUnlock, MEM_RELEASE};
    let _ = VirtualUnlock(ptr as *const _, bytes);
    let _ = VirtualFree(ptr as *mut _, 0, MEM_RELEASE);
}

#[cfg(unix)]
unsafe fn os_alloc(bytes: usize) -> *mut u8 {
    let ptr = libc::mmap(
        std::ptr::null_mut(),
        bytes,
        libc::PROT_READ | libc::PROT_WRITE,
        libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
        -1,
        0,
    );
    if ptr == libc::MAP_FAILED {
        return std::ptr::null_mut();
    }
    let _ = libc::mlock(ptr, bytes);
    ptr as *mut u8
}

#[cfg(unix)]
unsafe fn os_free(ptr: *mut u8, bytes: usize) {
    let _ = libc::munmap(ptr as *mut _, bytes);
}
