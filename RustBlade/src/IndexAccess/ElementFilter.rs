pub struct ElementFilter {
    m_size: i32,
    m_FilterSpace: Vec<u8>,
}

#[allow(non_snake_case)]
impl ElementFilter {
    pub fn new(size: i32) -> Self {
        Self {
            m_size: size,
            m_FilterSpace: if size > 0 { vec![0; size as usize] } else { Vec::new() },
        }
    }

    pub fn with_memory(size: i32, memory: Vec<u8>) -> Self {
        assert!(size <= 0 || memory.len() >= size as usize);
        Self { m_size: size, m_FilterSpace: memory }
    }

    pub fn AddElement(&mut self, element: &str) {
        if self.m_FilterSpace.is_empty() || self.m_size <= 0 { return; }
        let n1 = hash(element);
        let n2 = n1 ^ (n1 >> 17);
        let size = self.m_size as u64;
        self.m_FilterSpace[(n1 % size) as usize] = 1;
        self.m_FilterSpace[(n2 % size) as usize] = 1;
    }
    pub fn Contains(&self, element: &str) -> bool {
        if self.m_FilterSpace.is_empty() || self.m_size <= 0 { return true; }
        let n1 = hash(element);
        let n2 = n1 ^ (n1 >> 17);
        let size = self.m_size as u64;
        self.m_FilterSpace[(n1 % size) as usize] != 0 && self.m_FilterSpace[(n2 % size) as usize] != 0
    }
}

impl Default for ElementFilter {
    fn default() -> Self { Self::new(24) }
}

fn hash(value: &str) -> u64 {
    // Compatibility target: MSVC x64 std::hash<std::string_view>, which uses
    // 64-bit FNV-1a over the bytes in the string_view. The C++ API receives a
    // null-terminated char pointer, so bytes after the first NUL are excluded.
    // Other standard-library implementations are not promised to match this.
    let mut result = 14695981039346656037u64;
    let bytes = value.as_bytes();
    let length = bytes.iter().position(|byte| *byte == 0).unwrap_or(bytes.len());
    for byte in &bytes[..length] {
        result ^= *byte as u64;
        result = result.wrapping_mul(1099511628211u64);
    }
    result
}
