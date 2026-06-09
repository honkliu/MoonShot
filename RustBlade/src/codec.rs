/// Variable-byte (VarByte) integer codec.
///
/// Each byte carries 7 bits of payload in the low bits; the MSB is a
/// continuation flag.  Delta-compressed doc-ID sequences compress to
/// roughly 1–3 bytes per entry on typical corpora.
///
/// Matches the encoding used in MoonShot's `UnifiedDecoder` / `UnifiedEncoder`.

/// Encode `value` into `buf` using VarByte encoding.
pub fn encode(mut value: u64, buf: &mut Vec<u8>) {
    loop {
        if value < 0x80 {
            buf.push(value as u8);
            return;
        }
        buf.push((value as u8 & 0x7F) | 0x80);
        value >>= 7;
    }
}

/// Decode one VarByte integer starting at `buf[*pos]`, advancing `*pos`.
///
/// # Panics
/// Panics if the buffer is truncated (no terminating byte).
pub fn decode(buf: &[u8], pos: &mut usize) -> u64 {
    let mut result = 0u64;
    let mut shift = 0u32;
    loop {
        let byte = buf[*pos];
        *pos += 1;
        result |= ((byte & 0x7F) as u64) << shift;
        if byte & 0x80 == 0 {
            return result;
        }
        shift += 7;
    }
}

/// Encode a sorted slice of `(doc_id, term_freq)` pairs using delta coding
/// for doc-IDs, returning the compressed byte vector.
pub fn encode_postings(entries: &[(u64, u32)]) -> Vec<u8> {
    let mut buf = Vec::with_capacity(entries.len() * 3);
    let mut prev = 0u64;
    for &(doc_id, tf) in entries {
        encode(doc_id - prev, &mut buf);
        encode(tf as u64, &mut buf);
        prev = doc_id;
    }
    buf
}

/// Decode a byte slice produced by `encode_postings` into a `Vec<(doc_id, tf)>`.
pub fn decode_postings(buf: &[u8]) -> Vec<(u64, u32)> {
    let mut entries = Vec::new();
    let mut pos = 0;
    let mut prev = 0u64;
    while pos < buf.len() {
        let delta = decode(buf, &mut pos);
        let tf = decode(buf, &mut pos) as u32;
        prev += delta;
        entries.push((prev, tf));
    }
    entries
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_varint() {
        for &v in &[0u64, 1, 127, 128, 16383, 16384, u32::MAX as u64, u64::MAX / 2] {
            let mut buf = Vec::new();
            encode(v, &mut buf);
            let mut pos = 0;
            assert_eq!(decode(&buf, &mut pos), v);
        }
    }

    #[test]
    fn round_trip_postings() {
        let entries = vec![(1u64, 3u32), (5, 1), (1000, 7), (1001, 2)];
        let encoded = encode_postings(&entries);
        let decoded = decode_postings(&encoded);
        assert_eq!(decoded, entries);
    }
}
