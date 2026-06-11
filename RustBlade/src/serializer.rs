use std::io::{Write, Read, BufWriter, BufReader};
use std::fs::File;
use crate::posting_store::PostingStore;
use crate::error::{RustBladeError, Result};

const MAGIC:          &[u8; 8] = b"MOONSHOT";
const FORMAT_VERSION: u32      = 1;

/*
* IndexSerializer — binary index format.
*
* Layout:
*   8B  magic "MOONSHOT"
*   4B  format version
*   8B  num_docs
*   8B  total_terms
*   [DocDataRecord × num_docs]
*     8B doc_id, 4B doc_len, 4B importance
*   8B  num_terms
*   [TermEntry × num_terms]
*     2B key_len, key_len B key, 8B data_len, data_len B VarByte data
*/
pub struct IndexSerializer;

impl IndexSerializer {
    pub fn save(store: &mut PostingStore, path: &str) -> Result<()> {
        let file = File::create(path)?;
        let mut w = BufWriter::new(file);

        w.write_all(MAGIC)?;
        write_u32(&mut w, FORMAT_VERSION)?;
        write_u64(&mut w, store.total_docs())?;
        write_u64(&mut w, store.total_terms())?;

        for (doc_id, stats) in store.all_doc_stats() {
            write_u64(&mut w, *doc_id)?;
            write_u32(&mut w, stats.doc_len)?;
            write_f32(&mut w, stats.importance)?;
        }

        let postings = store.all_postings_mut();
        write_u64(&mut w, postings.len() as u64)?;

        for (key, pl) in postings.iter_mut() {
            let key_bytes = key.as_bytes();
            write_u16(&mut w, key_bytes.len() as u16)?;
            w.write_all(key_bytes)?;
            let bytes = pl.get_bytes_ref();
            write_u64(&mut w, bytes.len() as u64)?;
            w.write_all(&bytes)?;
        }

        w.flush()?;
        Ok(())
    }

    pub fn load(store: &mut PostingStore, path: &str) -> Result<()> {
        let file = File::open(path)?;
        let mut r = BufReader::new(file);

        let mut magic = [0u8; 8];
        r.read_exact(&mut magic)?;
        if &magic != MAGIC { return Err(RustBladeError::InvalidFormat); }

        let _version    = read_u32(&mut r)?;
        let num_docs    = read_u64(&mut r)?;
        let _total_terms = read_u64(&mut r)?;

        for _ in 0..num_docs {
            let doc_id     = read_u64(&mut r)?;
            let doc_len    = read_u32(&mut r)?;
            let importance = read_f32(&mut r)?;
            store.add_doc_tokens(doc_id, doc_len);
            store.set_doc_importance(doc_id, importance);
        }

        let num_terms = read_u64(&mut r)?;
        for _ in 0..num_terms {
            let key_len  = read_u16(&mut r)? as usize;
            let mut key_bytes = vec![0u8; key_len];
            r.read_exact(&mut key_bytes)?;
            let key = String::from_utf8(key_bytes)
                .map_err(|_| RustBladeError::InvalidFormat)?;

            let data_len = read_u64(&mut r)? as usize;
            let mut data = vec![0u8; data_len];
            r.read_exact(&mut data)?;

            decode_and_insert(store, &key, &data);
        }

        Ok(())
    }

    pub fn is_valid_index(path: &str) -> bool {
        let Ok(mut f) = File::open(path) else { return false; };
        let mut magic = [0u8; 8];
        f.read_exact(&mut magic).is_ok() && &magic == MAGIC
    }
}

fn decode_and_insert(store: &mut PostingStore, key: &str, data: &[u8]) {
    let mut pos      = 0usize;
    let mut last_doc = 0u64;
    while pos < data.len() {
        let (delta, n) = vb_read(data, pos); pos += n;
        if pos >= data.len() { break; }
        let (tf, n2)   = vb_read(data, pos); pos += n2;
        last_doc += delta;
        store.add_posting(key, last_doc, tf as u32);
    }
}

fn vb_read(data: &[u8], start: usize) -> (u64, usize) {
    let mut val   = 0u64;
    let mut shift = 0u8;
    let mut pos   = start;
    loop {
        if pos >= data.len() { break; }
        let b  = data[pos]; pos += 1;
        val   |= ((b & 0x7F) as u64) << shift;
        if (b & 0x80) == 0 { break; }
        shift += 7;
    }
    (val, pos - start)
}

fn write_u16(w: &mut impl Write, v: u16) -> std::io::Result<()> { w.write_all(&v.to_le_bytes()) }
fn write_u32(w: &mut impl Write, v: u32) -> std::io::Result<()> { w.write_all(&v.to_le_bytes()) }
fn write_u64(w: &mut impl Write, v: u64) -> std::io::Result<()> { w.write_all(&v.to_le_bytes()) }
fn write_f32(w: &mut impl Write, v: f32) -> std::io::Result<()> { w.write_all(&v.to_le_bytes()) }

fn read_u16(r: &mut impl Read) -> Result<u16> { let mut b = [0u8;2]; r.read_exact(&mut b)?; Ok(u16::from_le_bytes(b)) }
fn read_u32(r: &mut impl Read) -> Result<u32> { let mut b = [0u8;4]; r.read_exact(&mut b)?; Ok(u32::from_le_bytes(b)) }
fn read_u64(r: &mut impl Read) -> Result<u64> { let mut b = [0u8;8]; r.read_exact(&mut b)?; Ok(u64::from_le_bytes(b)) }
fn read_f32(r: &mut impl Read) -> Result<f32> { let mut b = [0u8;4]; r.read_exact(&mut b)?; Ok(f32::from_le_bytes(b)) }
