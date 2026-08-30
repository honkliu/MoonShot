//! Direct translation of the C++ hash functions; names stay aligned for API parity.
#![allow(non_snake_case, non_upper_case_globals)]

pub fn MurmurHash3_x86_32(key: &[u8], seed: u32) -> u32 {
    let c1 = 0xcc9e2d51u32;
    let c2 = 0x1b873593u32;
    let mut h1 = seed;
    let blocks = key.len() / 4;
    for index in 0..blocks {
        let mut k1 = u32::from_le_bytes(key[index * 4..index * 4 + 4].try_into().unwrap());
        k1 = k1.wrapping_mul(c1).rotate_left(15).wrapping_mul(c2);
        h1 ^= k1;
        h1 = h1.rotate_left(13).wrapping_mul(5).wrapping_add(0xe6546b64);
    }
    let tail = &key[blocks * 4..];
    let mut k1 = 0u32;
    if tail.len() >= 3 {
        k1 ^= (tail[2] as u32) << 16;
    }
    if tail.len() >= 2 {
        k1 ^= (tail[1] as u32) << 8;
    }
    if !tail.is_empty() {
        k1 ^= tail[0] as u32;
        h1 ^= k1.wrapping_mul(c1).rotate_left(15).wrapping_mul(c2);
    }
    h1 ^= key.len() as u32;
    h1 ^= h1 >> 16;
    h1 = h1.wrapping_mul(0x85ebca6b);
    h1 ^= h1 >> 13;
    h1 = h1.wrapping_mul(0xc2b2ae35);
    h1 ^ (h1 >> 16)
}

fn fmix32(mut value: u32) -> u32 {
    value ^= value >> 16;
    value = value.wrapping_mul(0x85ebca6b);
    value ^= value >> 13;
    value = value.wrapping_mul(0xc2b2ae35);
    value ^ (value >> 16)
}

fn fmix64(mut value: u64) -> u64 {
    value ^= value >> 33;
    value = value.wrapping_mul(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value = value.wrapping_mul(0xc4ceb9fe1a85ec53);
    value ^ (value >> 33)
}

pub fn MurmurHash3_x86_128(key: &[u8], seed: u32) -> [u32; 4] {
    const C1: u32 = 0x239b961b;
    const C2: u32 = 0xab0e9789;
    const C3: u32 = 0x38b34ae5;
    const C4: u32 = 0xa1e38b93;
    let (mut h1, mut h2, mut h3, mut h4) = (seed, seed, seed, seed);

    for block in key.chunks_exact(16) {
        let mut k1 = u32::from_le_bytes(block[0..4].try_into().unwrap());
        let mut k2 = u32::from_le_bytes(block[4..8].try_into().unwrap());
        let mut k3 = u32::from_le_bytes(block[8..12].try_into().unwrap());
        let mut k4 = u32::from_le_bytes(block[12..16].try_into().unwrap());
        k1 = k1.wrapping_mul(C1).rotate_left(15).wrapping_mul(C2);
        h1 ^= k1;
        h1 = h1
            .rotate_left(19)
            .wrapping_add(h2)
            .wrapping_mul(5)
            .wrapping_add(0x561ccd1b);
        k2 = k2.wrapping_mul(C2).rotate_left(16).wrapping_mul(C3);
        h2 ^= k2;
        h2 = h2
            .rotate_left(17)
            .wrapping_add(h3)
            .wrapping_mul(5)
            .wrapping_add(0x0bcaa747);
        k3 = k3.wrapping_mul(C3).rotate_left(17).wrapping_mul(C4);
        h3 ^= k3;
        h3 = h3
            .rotate_left(15)
            .wrapping_add(h4)
            .wrapping_mul(5)
            .wrapping_add(0x96cd1c35);
        k4 = k4.wrapping_mul(C4).rotate_left(18).wrapping_mul(C1);
        h4 ^= k4;
        h4 = h4
            .rotate_left(13)
            .wrapping_add(h1)
            .wrapping_mul(5)
            .wrapping_add(0x32ac3b17);
    }

    let tail = &key[key.len() / 16 * 16..];
    let (mut k1, mut k2, mut k3, mut k4) = (0u32, 0u32, 0u32, 0u32);
    if tail.len() >= 15 {
        k4 ^= (tail[14] as u32) << 16;
    }
    if tail.len() >= 14 {
        k4 ^= (tail[13] as u32) << 8;
    }
    if tail.len() >= 13 {
        k4 ^= tail[12] as u32;
        h4 ^= k4.wrapping_mul(C4).rotate_left(18).wrapping_mul(C1);
    }
    if tail.len() >= 12 {
        k3 ^= (tail[11] as u32) << 24;
    }
    if tail.len() >= 11 {
        k3 ^= (tail[10] as u32) << 16;
    }
    if tail.len() >= 10 {
        k3 ^= (tail[9] as u32) << 8;
    }
    if tail.len() >= 9 {
        k3 ^= tail[8] as u32;
        h3 ^= k3.wrapping_mul(C3).rotate_left(17).wrapping_mul(C4);
    }
    if tail.len() >= 8 {
        k2 ^= (tail[7] as u32) << 24;
    }
    if tail.len() >= 7 {
        k2 ^= (tail[6] as u32) << 16;
    }
    if tail.len() >= 6 {
        k2 ^= (tail[5] as u32) << 8;
    }
    if tail.len() >= 5 {
        k2 ^= tail[4] as u32;
        h2 ^= k2.wrapping_mul(C2).rotate_left(16).wrapping_mul(C3);
    }
    if tail.len() >= 4 {
        k1 ^= (tail[3] as u32) << 24;
    }
    if tail.len() >= 3 {
        k1 ^= (tail[2] as u32) << 16;
    }
    if tail.len() >= 2 {
        k1 ^= (tail[1] as u32) << 8;
    }
    if !tail.is_empty() {
        k1 ^= tail[0] as u32;
        h1 ^= k1.wrapping_mul(C1).rotate_left(15).wrapping_mul(C2);
    }

    let len = key.len() as u32;
    h1 ^= len;
    h2 ^= len;
    h3 ^= len;
    h4 ^= len;
    h1 = h1.wrapping_add(h2).wrapping_add(h3).wrapping_add(h4);
    h2 = h2.wrapping_add(h1);
    h3 = h3.wrapping_add(h1);
    h4 = h4.wrapping_add(h1);
    h1 = fmix32(h1);
    h2 = fmix32(h2);
    h3 = fmix32(h3);
    h4 = fmix32(h4);
    h1 = h1.wrapping_add(h2).wrapping_add(h3).wrapping_add(h4);
    h2 = h2.wrapping_add(h1);
    h3 = h3.wrapping_add(h1);
    h4 = h4.wrapping_add(h1);
    [h1, h2, h3, h4]
}

pub fn MurmurHash3_x64_128(key: &[u8], seed: u32) -> [u64; 2] {
    const C1: u64 = 0x87c37b91114253d5;
    const C2: u64 = 0x4cf5ad432745937f;
    let (mut h1, mut h2) = (seed as u64, seed as u64);
    for block in key.chunks_exact(16) {
        let mut k1 = u64::from_le_bytes(block[0..8].try_into().unwrap());
        let mut k2 = u64::from_le_bytes(block[8..16].try_into().unwrap());
        k1 = k1.wrapping_mul(C1).rotate_left(31).wrapping_mul(C2);
        h1 ^= k1;
        h1 = h1
            .rotate_left(27)
            .wrapping_add(h2)
            .wrapping_mul(5)
            .wrapping_add(0x52dce729);
        k2 = k2.wrapping_mul(C2).rotate_left(33).wrapping_mul(C1);
        h2 ^= k2;
        h2 = h2
            .rotate_left(31)
            .wrapping_add(h1)
            .wrapping_mul(5)
            .wrapping_add(0x38495ab5);
    }
    let tail = &key[key.len() / 16 * 16..];
    let (mut k1, mut k2) = (0u64, 0u64);
    for (index, byte) in tail.iter().copied().enumerate().skip(8) {
        k2 ^= (byte as u64) << ((index - 8) * 8);
    }
    if tail.len() >= 9 {
        h2 ^= k2.wrapping_mul(C2).rotate_left(33).wrapping_mul(C1);
    }
    for (index, byte) in tail.iter().copied().take(8).enumerate() {
        k1 ^= (byte as u64) << (index * 8);
    }
    if !tail.is_empty() {
        h1 ^= k1.wrapping_mul(C1).rotate_left(31).wrapping_mul(C2);
    }
    let len = key.len() as u64;
    h1 ^= len;
    h2 ^= len;
    h1 = h1.wrapping_add(h2);
    h2 = h2.wrapping_add(h1);
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 = h1.wrapping_add(h2);
    h2 = h2.wrapping_add(h1);
    [h1, h2]
}

pub fn Hash1(key: &[u8]) -> i32 {
    MurmurHash3_x86_32(key, 1) as i32
}
pub fn Hash2(key: &[u8]) -> i32 {
    MurmurHash3_x86_32(key, 2) as i32
}
pub fn HashMurmur3(key: &[u8], seed: i32) -> i32 {
    MurmurHash3_x86_32(key, seed as u32) as i32
}
pub fn HashCrypto(_key: &[u8]) -> i32 {
    0
}
