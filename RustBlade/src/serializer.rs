/*
 * IndexSerializer — v2 block-based format.
 *
 * File layout (same as C++ IndexSerializer.cpp):
 *   [FileHdr: 80 B]  magic + version + section offsets
 *   [SubIndex]       u32 count + entries (u16 kl, key, u32 block_seq)
 *   [DocData]        N × 16 B  (doc_id u64, importance f32, doc_len u32)
 *   [Padding]        zeros to PAGE_SIZE boundary
 *   [Blocks]         M × PAGE_SIZE — raw IndexBlock structs
 *
 * Block IB_Data format (multi-term, sorted alphabetically):
 *   Entry: u16 key_len | key | u32 doc_freq | u32 data_len | VarByte posting data
 *   Sentinel: u16 = 0  (no more entries)
 *   Continuation marker: u16 = 0xFFFF at IB_Data[0..2]
 */

use std::io::{Write, Read, BufWriter, BufReader};
use std::fs::File;
use crate::posting_store::PostingStore;
use crate::block_table::{
    IndexBlock, SubIndexEntry,
    IB_DATA_OFFSET, IB_SKIP_SLOTS, DATA_SIZE, PAGE_SIZE, IB_HEADER_HAS_MORE, CONT_MARKER,
};
use crate::error::{RustBladeError, Result};

const MAGIC:          &[u8; 8] = b"MOONSHOT";
const FORMAT_VERSION: u32      = 2;

pub struct IndexSerializer;

impl IndexSerializer {
    // ── Save ─────────────────────────────────────────────────────────────────

    pub fn save(store: &mut PostingStore, path: &str) -> Result<()> {
        let bytes = Self::encode(store)?;
        let f = File::create(path)?;
        let mut w = BufWriter::new(f);
        w.write_all(&bytes)?;
        w.flush()?;
        Ok(())
    }

    /// Encode the store to bytes (also used by WASM path).
    pub fn encode(store: &mut PostingStore) -> Result<Vec<u8>> {
        let (blocks, subindex) = Self::build_blocks(store);
        let subindex_buf       = Self::encode_subindex(&subindex);
        let docdata_buf        = Self::encode_docdata(store);

        let hdr_size:      usize = 80;
        let subindex_off:  usize = hdr_size;
        let subindex_size: usize = subindex_buf.len();
        let docdata_off:   usize = subindex_off + subindex_size;
        let docdata_size:  usize = docdata_buf.len();
        let raw_blocks_off: usize = docdata_off + docdata_size;
        let blocks_off:    usize = ((raw_blocks_off + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

        let total = blocks_off + blocks.len() * PAGE_SIZE;
        let mut out = vec![0u8; total];

        // Header
        out[0..8].copy_from_slice(MAGIC);
        write_u32(&mut out, 8,  FORMAT_VERSION);
        write_u32(&mut out, 12, 0);                          // reserved
        write_u64(&mut out, 16, store.total_docs() as u64);
        write_u64(&mut out, 24, store.total_terms() as u64);
        write_u64(&mut out, 32, subindex_off as u64);
        write_u64(&mut out, 40, subindex_size as u64);
        write_u64(&mut out, 48, docdata_off as u64);
        write_u64(&mut out, 56, docdata_size as u64);
        write_u64(&mut out, 64, blocks_off as u64);
        write_u64(&mut out, 72, blocks.len() as u64);

        out[subindex_off..subindex_off + subindex_size].copy_from_slice(&subindex_buf);
        out[docdata_off..docdata_off + docdata_size].copy_from_slice(&docdata_buf);

        // Write IndexBlocks as raw bytes
        for (i, blk) in blocks.iter().enumerate() {
            let off = blocks_off + i * PAGE_SIZE;
            let dst = &mut out[off..off + PAGE_SIZE];
            dst[0..8].copy_from_slice(&blk.ib_header.to_le_bytes());
            for (j, v) in blk.ib_skip.iter().enumerate() {
                let base = 8 + j * 4;
                dst[base..base + 4].copy_from_slice(&v.to_le_bytes());
            }
            dst[IB_DATA_OFFSET..IB_DATA_OFFSET + DATA_SIZE]
                .copy_from_slice(&blk.ib_data);
        }

        Ok(out)
    }

    // ── Load ─────────────────────────────────────────────────────────────────

    pub fn load(store: &mut PostingStore, path: &str)
        -> Result<(Vec<SubIndexEntry>, Vec<IndexBlock>)>
    {
        let f = File::open(path)?;
        let mut r = BufReader::new(f);
        let mut bytes = Vec::new();
        r.read_to_end(&mut bytes)?;
        Self::decode(store, &bytes)
    }

    /// Decode from a byte slice — fills DocData into store, returns subindex + loaded blocks.
    /// The caller (IndexContext) inserts the blocks into BlockTable and sets SubIndex.
    pub fn decode(
        store: &mut PostingStore,
        data:  &[u8],
    ) -> Result<(Vec<SubIndexEntry>, Vec<IndexBlock>)> {
        if data.len() < 80 { return Err(RustBladeError::InvalidFormat); }
        if &data[0..8] != MAGIC { return Err(RustBladeError::InvalidFormat); }
        let version = u32_at(data, 8);
        if version != FORMAT_VERSION { return Err(RustBladeError::InvalidFormat); }

        let subindex_off  = u64_at(data, 32) as usize;
        let subindex_size = u64_at(data, 40) as usize;
        let docdata_off   = u64_at(data, 48) as usize;
        let docdata_size  = u64_at(data, 56) as usize;
        let blocks_off    = u64_at(data, 64) as usize;
        let num_blocks    = u64_at(data, 72) as usize;

        // SubIndex
        let mut subindex = Vec::new();
        if subindex_off + subindex_size <= data.len() && subindex_size >= 4 {
            let n   = u32_at(data, subindex_off) as usize;
            let mut p = subindex_off + 4;
            for _ in 0..n {
                if p + 2 > data.len() { break; }
                let kl = u16_at(data, p) as usize; p += 2;
                if p + kl + 4 > data.len() { break; }
                let term = std::str::from_utf8(&data[p..p + kl])
                    .map_err(|_| RustBladeError::InvalidFormat)?.to_string();
                p += kl;
                let bseq = u32_at(data, p); p += 4;
                subindex.push(SubIndexEntry { term, block_seq: bseq });
            }
        }

        // DocData
        const DOC_REC: usize = 16;
        if docdata_off + docdata_size <= data.len() {
            let n = docdata_size / DOC_REC;
            for i in 0..n {
                let base       = docdata_off + i * DOC_REC;
                let doc_id     = u64_at(data, base);
                let importance = f32::from_le_bytes([data[base+8],data[base+9],data[base+10],data[base+11]]);
                let doc_len    = u32_at(data, base + 12);
                store.add_doc_tokens(doc_id, doc_len);
                store.set_doc_importance(doc_id, importance);
            }
        }

        // Load all IndexBlocks from the file bytes
        let mut blocks = Vec::with_capacity(num_blocks);
        for i in 0..num_blocks {
            let off = blocks_off + i * PAGE_SIZE;
            if off + PAGE_SIZE > data.len() { break; }
            let mut blk = IndexBlock::default();
            blk.ib_header = u64_at(data, off);
            for j in 0..IB_SKIP_SLOTS {
                blk.ib_skip[j] = u32_at(data, off + 8 + j * 4);
            }
            blk.ib_data.copy_from_slice(&data[off + IB_DATA_OFFSET..off + PAGE_SIZE]);
            blocks.push(blk);
        }

        Ok((subindex, blocks))
    }

    pub fn is_valid_index(path: &str) -> bool {
        let Ok(mut f) = File::open(path) else { return false; };
        let mut magic = [0u8; 8];
        f.read_exact(&mut magic).is_ok() && &magic == MAGIC
    }

    pub fn is_valid_bytes(data: &[u8]) -> bool {
        data.len() >= 8 && &data[0..8] == MAGIC
    }

    // ── Internal helpers ─────────────────────────────────────────────────────

    /// Public alias used by IndexContext::build().
    pub fn build_blocks_pub(store: &PostingStore)
        -> (Vec<IndexBlock>, Vec<SubIndexEntry>)
    {
        Self::build_blocks(store)
    }

    /// Sort all terms alphabetically, pack them into IndexBlocks.
    /// Returns (blocks, subindex_entries).
    fn build_blocks(store: &PostingStore) -> (Vec<IndexBlock>, Vec<SubIndexEntry>) {
        let mut terms: Vec<(&String, &crate::posting_store::PostingList)> =
            store.all_postings().iter().collect();
        terms.sort_by_key(|(k, _)| k.as_str());

        let cap = DATA_SIZE - 1;

        let mut blocks:   Vec<IndexBlock>   = Vec::new();
        let mut subindex: Vec<SubIndexEntry> = Vec::new();

        let mut cur_blk  = IndexBlock::default();
        let mut wptr     = 0usize;
        let mut fresh    = true;
        let mut seq      = 0u32;

        let flush = |blocks: &mut Vec<IndexBlock>,
                     cur_blk: &mut IndexBlock,
                     wptr: &mut usize,
                     fresh: &mut bool,
                     seq: &mut u32,
                     has_more: bool|
        {
            cur_blk.ib_header = *seq as u64;
            if has_more { cur_blk.ib_header |= IB_HEADER_HAS_MORE; }
            if !has_more && *wptr + 2 <= cap + 1 {
                cur_blk.ib_data[*wptr]     = 0;
                cur_blk.ib_data[*wptr + 1] = 0;
            }
            blocks.push(cur_blk.clone());
            *seq   += 1;
            *cur_blk = IndexBlock::default();
            *wptr  = 0;
            *fresh = true;
        };

        for (key, pl) in &terms {
            let bytes     = pl.get_bytes_ref();
            if bytes.is_empty() { continue; }

            let kl        = key.len() as u16;
            let hdr_size  = 2 + key.len() + 4 + 4;

            if cap.saturating_sub(wptr) < hdr_size + 1 {
                flush(&mut blocks, &mut cur_blk, &mut wptr, &mut fresh, &mut seq, false);
            }

            if fresh {
                subindex.push(SubIndexEntry { term: key.to_string(), block_seq: seq });
                fresh = false;
            }

            let data_space  = cap.saturating_sub(wptr).saturating_sub(hdr_size);
            let data_here   = bytes.len().min(data_space);
            let has_more    = data_here < bytes.len();

            // Write entry header
            let kl_le = kl.to_le_bytes();
            cur_blk.ib_data[wptr..wptr+2].copy_from_slice(&kl_le);     wptr += 2;
            cur_blk.ib_data[wptr..wptr+key.len()].copy_from_slice(key.as_bytes()); wptr += key.len();
            let freq_le = pl.doc_freq().to_le_bytes();
            cur_blk.ib_data[wptr..wptr+4].copy_from_slice(&freq_le);   wptr += 4;
            let dl_le   = (data_here as u32).to_le_bytes();
            cur_blk.ib_data[wptr..wptr+4].copy_from_slice(&dl_le);     wptr += 4;
            cur_blk.ib_data[wptr..wptr+data_here].copy_from_slice(&bytes[..data_here]);
            wptr += data_here;

            if has_more {
                flush(&mut blocks, &mut cur_blk, &mut wptr, &mut fresh, &mut seq, true);
                let mut src = data_here;
                while src < bytes.len() {
                    // continuation block
                    let cm = CONT_MARKER.to_le_bytes();
                    cur_blk.ib_data[0..2].copy_from_slice(&cm); wptr = 2;
                    let cont_here = (bytes.len() - src).min(cap - 2);
                    let more_cont = cont_here < bytes.len() - src;
                    cur_blk.ib_data[wptr..wptr+cont_here].copy_from_slice(&bytes[src..src+cont_here]);
                    wptr += cont_here;
                    src  += cont_here;
                    flush(&mut blocks, &mut cur_blk, &mut wptr, &mut fresh, &mut seq, more_cont);
                }
            }
        }

        if !fresh || wptr > 0 {
            flush(&mut blocks, &mut cur_blk, &mut wptr, &mut fresh, &mut seq, false);
        }

        (blocks, subindex)
    }

    fn encode_subindex(entries: &[SubIndexEntry]) -> Vec<u8> {
        let mut out = Vec::new();
        let n = entries.len() as u32;
        out.extend_from_slice(&n.to_le_bytes());
        for e in entries {
            let kl = e.term.len() as u16;
            out.extend_from_slice(&kl.to_le_bytes());
            out.extend_from_slice(e.term.as_bytes());
            out.extend_from_slice(&e.block_seq.to_le_bytes());
        }
        out
    }

    fn encode_docdata(store: &PostingStore) -> Vec<u8> {
        let mut out = Vec::new();
        for (doc_id, stats) in store.all_doc_stats() {
            out.extend_from_slice(&doc_id.to_le_bytes());
            out.extend_from_slice(&stats.importance.to_le_bytes());
            out.extend_from_slice(&stats.doc_len.to_le_bytes());
            out.extend_from_slice(&0u32.to_le_bytes()); // padding
        }
        out
    }
}

// ── byte helpers ──────────────────────────────────────────────────────────────
fn u16_at(d: &[u8], o: usize) -> u16 { u16::from_le_bytes([d[o], d[o+1]]) }
fn u32_at(d: &[u8], o: usize) -> u32 { u32::from_le_bytes([d[o],d[o+1],d[o+2],d[o+3]]) }
fn u64_at(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes([d[o],d[o+1],d[o+2],d[o+3],d[o+4],d[o+5],d[o+6],d[o+7]])
}
fn write_u32(buf: &mut [u8], off: usize, v: u32) { buf[off..off+4].copy_from_slice(&v.to_le_bytes()); }
fn write_u64(buf: &mut [u8], off: usize, v: u64) { buf[off..off+8].copy_from_slice(&v.to_le_bytes()); }
