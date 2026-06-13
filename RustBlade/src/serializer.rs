/*
 * IndexSerializer v3 — matches C++ IndexSerializer.cpp.
 *
 * Layout:
 *   [Header 96B]      magic, version, section offsets (added pageskip)
 *   [SubIndex]        u32 count + entries (kl, key, block_seq, block_entry_start, page_skip_offset)
 *   [PageSkipList]    flat u64 arrays, one per multi-block term
 *   [DocData]         N × 16B
 *   [Padding]         zeros to PAGE_SIZE
 *   [Blocks]          raw IndexBlock structs
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
const FORMAT_VERSION: u32      = 4;

pub struct BlocksResult {
    pub blocks:   Vec<IndexBlock>,
    pub subindex: Vec<SubIndexEntry>,
    pub pageskip: Vec<u64>,
}

pub struct IndexSerializer;

impl IndexSerializer {
    pub fn save(store: &mut PostingStore, path: &str) -> Result<()> {
        let bytes = Self::encode(store)?;
        let f = File::create(path)?;
        let mut w = BufWriter::new(f);
        w.write_all(&bytes)?;
        w.flush()?;
        Ok(())
    }

    pub fn encode(store: &mut PostingStore) -> Result<Vec<u8>> {
        let br = Self::build_blocks(store);
        let subindex_buf = Self::encode_subindex(&br.subindex);
        let pageskip_buf = Self::encode_pageskip(&br.pageskip);
        let docdata_buf  = Self::encode_docdata(store);

        let hdr_size:      usize = 96;  // v3: 80+16 for pageskip fields
        let subindex_off:  usize = hdr_size;
        let subindex_size: usize = subindex_buf.len();
        let pageskip_off:  usize = subindex_off + subindex_size;
        let pageskip_size: usize = pageskip_buf.len();
        let docdata_off:   usize = pageskip_off + pageskip_size;
        let docdata_size:  usize = docdata_buf.len();
        let raw_off:       usize = docdata_off + docdata_size;
        let blocks_off:    usize = ((raw_off + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

        let total = blocks_off + br.blocks.len() * PAGE_SIZE;
        let mut out = vec![0u8; total];

        out[0..8].copy_from_slice(MAGIC);
        write_u32(&mut out,  8, FORMAT_VERSION);
        write_u32(&mut out, 12, 0);
        write_u64(&mut out, 16, store.total_docs() as u64);
        write_u64(&mut out, 24, store.total_terms() as u64);
        write_u64(&mut out, 32, subindex_off  as u64);
        write_u64(&mut out, 40, subindex_size as u64);
        write_u64(&mut out, 48, pageskip_off  as u64);
        write_u64(&mut out, 56, pageskip_size as u64);
        write_u64(&mut out, 64, docdata_off   as u64);
        write_u64(&mut out, 72, docdata_size  as u64);
        write_u64(&mut out, 80, blocks_off    as u64);
        write_u64(&mut out, 88, br.blocks.len() as u64);

        out[subindex_off..subindex_off + subindex_size].copy_from_slice(&subindex_buf);
        out[pageskip_off..pageskip_off + pageskip_size].copy_from_slice(&pageskip_buf);
        out[docdata_off..docdata_off + docdata_size].copy_from_slice(&docdata_buf);

        for (i, blk) in br.blocks.iter().enumerate() {
            let off = blocks_off + i * PAGE_SIZE;
            let dst = &mut out[off..off + PAGE_SIZE];
            dst[0..8].copy_from_slice(&blk.ib_header.to_le_bytes());
            for (j, v) in blk.ib_skip.iter().enumerate() {
                let base = 8 + j * 4;
                dst[base..base+4].copy_from_slice(&v.to_le_bytes());
            }
            dst[IB_DATA_OFFSET..IB_DATA_OFFSET + DATA_SIZE].copy_from_slice(&blk.ib_data);
        }
        Ok(out)
    }

    pub fn load(store: &mut PostingStore, path: &str)
        -> Result<(Vec<SubIndexEntry>, Vec<IndexBlock>, Vec<u64>)>
    {
        let f = File::open(path)?;
        let mut r = BufReader::new(f);
        let mut bytes = Vec::new();
        r.read_to_end(&mut bytes)?;
        Self::decode(store, &bytes)
    }

    pub fn decode(store: &mut PostingStore, data: &[u8])
        -> Result<(Vec<SubIndexEntry>, Vec<IndexBlock>, Vec<u64>)>
    {
        if data.len() < 96 { return Err(RustBladeError::InvalidFormat); }
        if &data[0..8] != MAGIC { return Err(RustBladeError::InvalidFormat); }
        if u32_at(data, 8) != FORMAT_VERSION { return Err(RustBladeError::InvalidFormat); }

        let subindex_off  = u64_at(data, 32) as usize;
        let subindex_size = u64_at(data, 40) as usize;
        let pageskip_off  = u64_at(data, 48) as usize;
        let pageskip_size = u64_at(data, 56) as usize;
        let docdata_off   = u64_at(data, 64) as usize;
        let docdata_size  = u64_at(data, 72) as usize;
        let blocks_off    = u64_at(data, 80) as usize;
        let num_blocks    = u64_at(data, 88) as usize;

        /* SubIndex */
        let mut subindex = Vec::new();
        if subindex_off + subindex_size <= data.len() && subindex_size >= 4 {
            let n   = u32_at(data, subindex_off) as usize;
            let mut p = subindex_off + 4;
            for _ in 0..n {
                if p + 2 > data.len() { break; }
                let kl = u16_at(data, p) as usize; p += 2;
                if p + kl + 12 > data.len() { break; }
                let term = std::str::from_utf8(&data[p..p+kl])
                    .map_err(|_| RustBladeError::InvalidFormat)?.to_string();
                p += kl;
                let bseq = u32_at(data, p); p += 4;
                let bes  = u32_at(data, p); p += 4;
                let pso  = u32_at(data, p); p += 4;
                subindex.push(SubIndexEntry { term, block_seq: bseq, block_entry_start: bes, page_skip_offset: pso });
            }
        }

        /* PageSkipList */
        let mut pageskip = Vec::new();
        if pageskip_off + pageskip_size <= data.len() && pageskip_size >= 8 {
            let n = pageskip_size / 8;
            pageskip.reserve(n);
            for i in 0..n {
                pageskip.push(u64_at(data, pageskip_off + i * 8));
            }
        }

        /* DocData — 256B per record */
        const DOC_REC: usize = 256;
        const PMAX: usize = 232;
        if docdata_off + docdata_size <= data.len() {
            let n = docdata_size / DOC_REC;
            for i in 0..n {
                let base   = docdata_off + i * DOC_REC;
                let doc_id = u64_at(data, base);
                let importance = f32::from_le_bytes(data[base+8..base+12].try_into().unwrap());
                let doc_len = u32_at(data, base+12);
                let path_len = u16::from_le_bytes(data[base+16..base+18].try_into().unwrap()) as usize;
                store.add_doc_tokens(doc_id, doc_len);
                store.set_doc_importance(doc_id, importance);
                if path_len > 0 {
                    let plen = path_len.min(PMAX - 1);
                    if let Ok(p) = std::str::from_utf8(&data[base+24..base+24+plen]) {
                        store.set_doc_path(doc_id, p.to_string());
                    }
                }
            }
        }

        /* Blocks */
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

        Ok((subindex, blocks, pageskip))
    }

    pub fn is_valid_index(path: &str) -> bool {
        let Ok(mut f) = File::open(path) else { return false; };
        let mut magic = [0u8; 8];
        f.read_exact(&mut magic).is_ok() && &magic == MAGIC
    }

    pub fn is_valid_bytes(data: &[u8]) -> bool {
        data.len() >= 8 && &data[0..8] == MAGIC
    }

    /// Called by IndexContext::build() — packs blocks in memory without writing to disk.
    pub fn build_blocks_pub(store: &PostingStore) -> (Vec<IndexBlock>, Vec<SubIndexEntry>, Vec<u64>) {
        let br = Self::build_blocks(store);
        (br.blocks, br.subindex, br.pageskip)
    }

    // ── Internal ─────────────────────────────────────────────────────────────

    fn build_blocks(store: &PostingStore) -> BlocksResult {
        let mut terms: Vec<(&String, &crate::posting_store::PostingList)> =
            store.all_postings().iter().collect();
        terms.sort_by_key(|(k, _)| k.as_str());

        let cap = DATA_SIZE - 1;
        let mut blocks:   Vec<IndexBlock>    = Vec::new();
        let mut subindex: Vec<SubIndexEntry> = Vec::new();
        let mut pageskip: Vec<u64>           = Vec::new();

        let mut cur = IndexBlock::default();
        let mut wptr:            usize = 0;
        let mut seq:             u32   = 0;
        let mut fresh:           bool  = true;
        let mut blk_entry_start: u32   = 0;

        let flush = |blocks: &mut Vec<IndexBlock>,
                     cur: &mut IndexBlock,
                     wptr: &mut usize,
                     seq: &mut u32,
                     fresh: &mut bool,
                     blk_entry_start: &mut u32,
                     has_more: bool|
        {
            cur.ib_header = *seq as u64;
            if has_more { cur.ib_header |= IB_HEADER_HAS_MORE; }
            if !has_more && *wptr + 2 <= cap + 1 {
                cur.ib_data[*wptr]     = 0;
                cur.ib_data[*wptr + 1] = 0;
            }
            blocks.push(cur.clone());
            *seq  += 1;
            *cur   = IndexBlock::default();
            *wptr  = 0;
            *fresh = true;
            *blk_entry_start = 0;
        };

        for (key, pl) in &terms {
            let bytes    = pl.get_bytes_ref();
            if bytes.is_empty() { continue; }

            let kl       = key.len() as u16;
            let freq     = pl.doc_freq();
            let hdr_size = 2 + key.len() + 4 + 4;

            if cap.saturating_sub(wptr) < hdr_size + 1 {
                flush(&mut blocks, &mut cur, &mut wptr, &mut seq, &mut fresh, &mut blk_entry_start, false);
            }

            if fresh {
                subindex.push(SubIndexEntry {
                    term: key.to_string(),
                    block_seq: seq,
                    block_entry_start: blk_entry_start,
                    page_skip_offset: 0,
                });
                fresh = false;
            }

            let data_space = cap.saturating_sub(wptr).saturating_sub(hdr_size);
            let data_here  = bytes.len().min(data_space);
            let has_more   = data_here < bytes.len();

            let kl_le = kl.to_le_bytes();
            cur.ib_data[wptr..wptr+2].copy_from_slice(&kl_le);              wptr += 2;
            cur.ib_data[wptr..wptr+key.len()].copy_from_slice(key.as_bytes()); wptr += key.len();
            cur.ib_data[wptr..wptr+4].copy_from_slice(&freq.to_le_bytes());  wptr += 4;
            let dl_le = (data_here as u32).to_le_bytes();
            cur.ib_data[wptr..wptr+4].copy_from_slice(&dl_le);              wptr += 4;
            cur.ib_data[wptr..wptr+data_here].copy_from_slice(&bytes[..data_here]);
            wptr += data_here;

            if has_more {
                flush(&mut blocks, &mut cur, &mut wptr, &mut seq, &mut fresh, &mut blk_entry_start, true);

                /* record PageSkipList for this multi-block term */
                let skip_offset = pageskip.len() as u32;
                pageskip.push(0u64);  /* entry 0: base doc for first block = 0 */
                subindex.last_mut().unwrap().page_skip_offset = skip_offset;

                let mut src = data_here;
                while src < bytes.len() {
                    let cont_cap  = DATA_SIZE - 4;  /* 4 = CONT_MARKER(2) + cont_len(2) */
                    let cont_here = (bytes.len() - src).min(cont_cap);
                    let more_cont = cont_here < bytes.len() - src;

                    /* Decode last docid of previous chunk for pageskip base */
                    let prev_chunk = &bytes[..src];
                    let base_doc = decode_last_docid(prev_chunk);
                    pageskip.push(base_doc);

                    /* Write CONT_MARKER + cont_len + bytes */
                    let cm = CONT_MARKER.to_le_bytes();
                    let cl = (cont_here as u16).to_le_bytes();
                    cur.ib_data[wptr..wptr+2].copy_from_slice(&cm);              wptr += 2;
                    cur.ib_data[wptr..wptr+2].copy_from_slice(&cl);              wptr += 2;
                    cur.ib_data[wptr..wptr+cont_here].copy_from_slice(&bytes[src..src+cont_here]);
                    wptr += cont_here;
                    src  += cont_here;

                    if more_cont {
                        flush(&mut blocks, &mut cur, &mut wptr, &mut seq, &mut fresh, &mut blk_entry_start, true);
                    } else {
                        /* last continuation: remaining space for new entries */
                        blk_entry_start = (2 + 2 + cont_here) as u32;
                        fresh = true;
                    }
                }
                /* close pageskip array for this term */
                pageskip.push(u64::MAX);
            }
        }

        if !fresh || wptr > 0 {
            flush(&mut blocks, &mut cur, &mut wptr, &mut seq, &mut fresh, &mut blk_entry_start, false);
        }

        BlocksResult { blocks, subindex, pageskip }
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
            out.extend_from_slice(&e.block_entry_start.to_le_bytes());
            out.extend_from_slice(&e.page_skip_offset.to_le_bytes());
        }
        out
    }

    fn encode_pageskip(data: &[u64]) -> Vec<u8> {
        let mut out = vec![0u8; data.len() * 8];
        for (i, v) in data.iter().enumerate() {
            out[i*8..(i+1)*8].copy_from_slice(&v.to_le_bytes());
        }
        out
    }

    fn encode_docdata(store: &PostingStore) -> Vec<u8> {
        const REC: usize = 256;
        const PMAX: usize = 232;
        let mut out = Vec::new();
        for (doc_id, stats) in store.all_doc_stats() {
            let mut rec = [0u8; REC];
            rec[0..8].copy_from_slice(&doc_id.to_le_bytes());
            rec[8..12].copy_from_slice(&stats.importance.to_le_bytes());
            rec[12..16].copy_from_slice(&stats.doc_len.to_le_bytes());
            let plen = stats.path.len().min(PMAX - 1);
            let plen16 = plen as u16;
            rec[16..18].copy_from_slice(&plen16.to_le_bytes());
            // rec[18..24] = padding (zeroed)
            rec[24..24+plen].copy_from_slice(&stats.path.as_bytes()[..plen]);
            out.extend_from_slice(&rec);
        }
        out
    }
}

/// Decode a VarByte stream and return the last doc_id seen.
fn decode_last_docid(data: &[u8]) -> u64 {
    let mut pos = 0usize;
    let mut prev = 0u64;
    while pos < data.len() {
        let (delta, n) = vb_read(data, pos); pos += n;
        if pos >= data.len() { break; }
        let (_tf, m) = vb_read(data, pos); pos += m;
        prev += delta;
    }
    prev
}

fn vb_read(data: &[u8], start: usize) -> (u64, usize) {
    let mut val = 0u64; let mut shift = 0u8; let mut pos = start;
    loop {
        if pos >= data.len() { break; }
        let b = data[pos]; pos += 1;
        val |= ((b & 0x7F) as u64) << shift;
        if (b & 0x80) == 0 { break; }
        shift += 7;
    }
    (val, pos - start)
}

fn u16_at(d: &[u8], o: usize) -> u16 { u16::from_le_bytes([d[o], d[o+1]]) }
fn u32_at(d: &[u8], o: usize) -> u32 { u32::from_le_bytes([d[o],d[o+1],d[o+2],d[o+3]]) }
fn u64_at(d: &[u8], o: usize) -> u64 {
    u64::from_le_bytes([d[o],d[o+1],d[o+2],d[o+3],d[o+4],d[o+5],d[o+6],d[o+7]])
}
fn write_u32(buf: &mut [u8], off: usize, v: u32) { buf[off..off+4].copy_from_slice(&v.to_le_bytes()); }
fn write_u64(buf: &mut [u8], off: usize, v: u64) { buf[off..off+8].copy_from_slice(&v.to_le_bytes()); }
