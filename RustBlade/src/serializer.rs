/*
 * IndexSerializer v7 — paged two-level Head/Leaf term table format.
 *
 * Head/Leaf term table on disk:
 *   [dir_count:4]
 *     per head entry: [key_len:2][HTE_FirstTerm:key_len][HTE_LeafTermBlockID:4]
 *   [leaf_page_count:4]
 *     per 4096-byte page: [entry_count:4]
 *       per entry: [key_len:2][LTE_Term:key_len][LTE_DocFreq:4]
 *                  [LTE_IndexBlockID:4][LTE_IndexOffset:4][LTE_IndexLength:4]
 *                  [LTE_PageSkipOffset:4][LTE_ContinuationBlockCount:4][LTE_Flags:4]
 */

use std::io::{Write, Read, BufWriter, BufReader};
use std::fs::File;
use crate::posting_store::PostingStore;
use crate::block_table::{
    IndexBlock, LeafTermEntry, HeadTermEntry, LeafTermBlock,
    IB_DATA_OFFSET, IB_SKIP_SLOTS, DATA_SIZE, PAGE_SIZE,
    IB_HEADER_HAS_MORE, CONT_MARKER,
};
use crate::error::{RustBladeError, Result};

const MAGIC:          &[u8; 8] = b"MOONSHOT";
const FORMAT_VERSION: u32      = 7;

pub struct BlocksResult {
    pub bbr_index_blocks:       Vec<IndexBlock>,
    pub bbr_head_term_entries:  Vec<HeadTermEntry>,
    pub bbr_leaf_term_blocks:   Vec<LeafTermBlock>,
    pub bbr_page_skip_list:     Vec<u64>,
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
        let head_leaf_term_table_buf = Self::encode_head_leaf_term_table(&br.bbr_head_term_entries, &br.bbr_leaf_term_blocks);
        let pageskip_buf = Self::encode_pageskip(&br.bbr_page_skip_list);
        let docdata_buf  = Self::encode_docdata(store);

        let total_terms: usize = br.bbr_leaf_term_blocks.iter().map(|b| b.ltb_entries.len()).sum();

        let hdr_size:     usize = 96;
        let si_off:       usize = hdr_size;
        let si_size:      usize = head_leaf_term_table_buf.len();
        let ps_off:       usize = si_off + si_size;
        let ps_size:      usize = pageskip_buf.len();
        let dd_off:       usize = ps_off + ps_size;
        let dd_size:      usize = docdata_buf.len();
        let raw_off:      usize = dd_off + dd_size;
        let blocks_off:   usize = ((raw_off + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
        let total        = blocks_off + br.bbr_index_blocks.len() * PAGE_SIZE;

        let mut out = vec![0u8; total];
        out[0..8].copy_from_slice(MAGIC);
        write_u32(&mut out,  8, FORMAT_VERSION);
        write_u32(&mut out, 12, 0);
        write_u64(&mut out, 16, store.total_docs()  as u64);
        write_u64(&mut out, 24, total_terms         as u64);
        write_u64(&mut out, 32, si_off              as u64);
        write_u64(&mut out, 40, si_size             as u64);
        write_u64(&mut out, 48, ps_off              as u64);
        write_u64(&mut out, 56, ps_size             as u64);
        write_u64(&mut out, 64, dd_off              as u64);
        write_u64(&mut out, 72, dd_size             as u64);
        write_u64(&mut out, 80, blocks_off          as u64);
        write_u64(&mut out, 88, br.bbr_index_blocks.len() as u64);

        out[si_off..si_off + si_size].copy_from_slice(&head_leaf_term_table_buf);
        out[ps_off..ps_off + ps_size].copy_from_slice(&pageskip_buf);
        out[dd_off..dd_off + dd_size].copy_from_slice(&docdata_buf);

        for (i, blk) in br.bbr_index_blocks.iter().enumerate() {
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
        -> Result<(Vec<HeadTermEntry>, Vec<LeafTermBlock>, Vec<IndexBlock>, Vec<u64>)>
    {
        let f = File::open(path)?;
        let mut r = BufReader::new(f);
        let mut bytes = Vec::new();
        r.read_to_end(&mut bytes)?;
        Self::decode(store, &bytes)
    }

    pub fn decode(store: &mut PostingStore, data: &[u8])
        -> Result<(Vec<HeadTermEntry>, Vec<LeafTermBlock>, Vec<IndexBlock>, Vec<u64>)>
    {
        if data.len() < 96 { return Err(RustBladeError::InvalidFormat); }
        if &data[0..8] != MAGIC { return Err(RustBladeError::InvalidFormat); }
        if u32_at(data, 8) != FORMAT_VERSION { return Err(RustBladeError::InvalidFormat); }

        let si_off    = u64_at(data, 32) as usize;
        let si_size   = u64_at(data, 40) as usize;
        let ps_off    = u64_at(data, 48) as usize;
        let ps_size   = u64_at(data, 56) as usize;
        let dd_off    = u64_at(data, 64) as usize;
        let dd_size   = u64_at(data, 72) as usize;
        let blk_off   = u64_at(data, 80) as usize;
        let num_blks  = u64_at(data, 88) as usize;

        /* Head/Leaf term table — two-level */
        let mut head: Vec<HeadTermEntry> = Vec::new();
        let mut leaf_blocks: Vec<LeafTermBlock> = Vec::new();

        if si_off + si_size <= data.len() && si_size >= 4 {
            let mut p = si_off;

            let head_count = u32_at(data, p) as usize; p += 4;
            head.reserve(head_count);
            for _ in 0..head_count {
                if p + 2 > data.len() { break; }
                let kl = u16_at(data, p) as usize; p += 2;
                if p + kl + 4 > data.len() { break; }
                let first = std::str::from_utf8(&data[p..p+kl])
                    .map_err(|_| RustBladeError::InvalidFormat)?.to_string();
                p += kl;
                let bidx = u32_at(data, p); p += 4;
                head.push(HeadTermEntry { hte_first_term: first, hte_leaf_term_block_id: bidx });
            }

            if p + 4 <= data.len() {
                let blk_count = u32_at(data, p) as usize; p += 4;
                leaf_blocks.resize_with(blk_count, LeafTermBlock::default);
                for b in 0..blk_count {
                    let page_end = (p + PAGE_SIZE).min(si_off + si_size).min(data.len());
                    if p + 4 > page_end { break; }
                    let mut q = p;
                    let entry_count = u32_at(data, q) as usize; q += 4;
                    leaf_blocks[b].ltb_entries.reserve(entry_count);
                    for _ in 0..entry_count {
                        if q + 2 > page_end { break; }
                        let kl = u16_at(data, q) as usize; q += 2;
                        if q + kl + 28 > page_end { break; }
                        let term = std::str::from_utf8(&data[q..q+kl])
                            .map_err(|_| RustBladeError::InvalidFormat)?.to_string();
                        q += kl;
                        let freq = u32_at(data, q); q += 4;
                        let pbid = u32_at(data, q); q += 4;
                        let poff = u32_at(data, q); q += 4;
                        let plen = u32_at(data, q); q += 4;
                        let skip = u32_at(data, q); q += 4;
                        let cont = u32_at(data, q); q += 4;
                        let flags = u32_at(data, q); q += 4;
                        leaf_blocks[b].ltb_entries.push(LeafTermEntry {
                            lte_term: term,
                            lte_doc_freq: freq,
                            lte_index_block_id: pbid,
                            lte_index_offset: poff,
                            lte_index_length: plen,
                            lte_page_skip_offset: skip,
                            lte_continuation_block_count: cont,
                            lte_flags: flags,
                        });
                    }
                    p += PAGE_SIZE;
                }
            }
        }

        /* PageSkipList */
        let mut pageskip = Vec::new();
        if ps_off + ps_size <= data.len() && ps_size >= 8 {
            let n = ps_size / 8;
            pageskip.reserve(n);
            for i in 0..n { pageskip.push(u64_at(data, ps_off + i * 8)); }
        }

        /* DocData */
        const DOC_REC: usize = 1024;
        const PMAX: usize = 1000;
        if dd_off + dd_size <= data.len() {
            let n = dd_size / DOC_REC;
            for i in 0..n {
                let base = dd_off + i * DOC_REC;
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

        /* Posting blocks */
        let mut index_blocks = Vec::with_capacity(num_blks);
        for i in 0..num_blks {
            let off = blk_off + i * PAGE_SIZE;
            if off + PAGE_SIZE > data.len() { break; }
            let mut blk = IndexBlock::default();
            blk.ib_header = u64_at(data, off);
            for j in 0..IB_SKIP_SLOTS { blk.ib_skip[j] = u32_at(data, off + 8 + j * 4); }
            blk.ib_data.copy_from_slice(&data[off + IB_DATA_OFFSET..off + PAGE_SIZE]);
            index_blocks.push(blk);
        }

        Ok((head, leaf_blocks, index_blocks, pageskip))
    }

    pub fn is_valid_index(path: &str) -> bool {
        let Ok(mut f) = File::open(path) else { return false; };
        let mut magic = [0u8; 8];
        f.read_exact(&mut magic).is_ok() && &magic == MAGIC
    }

    pub fn is_valid_bytes(data: &[u8]) -> bool {
        data.len() >= 8 && &data[0..8] == MAGIC
    }

    pub fn build_blocks_pub(store: &PostingStore)
        -> (Vec<IndexBlock>, Vec<HeadTermEntry>, Vec<LeafTermBlock>, Vec<u64>)
    {
        let br = Self::build_blocks(store);
        (br.bbr_index_blocks, br.bbr_head_term_entries, br.bbr_leaf_term_blocks, br.bbr_page_skip_list)
    }

    // ── Internal ─────────────────────────────────────────────────────────────

    fn build_blocks(store: &PostingStore) -> BlocksResult {
        let mut terms: Vec<(&String, &crate::posting_store::PostingList)> =
            store.all_postings().iter().collect();
        terms.sort_by_key(|(k, _)| k.as_str());

        let cap = DATA_SIZE;
        let mut index_blocks: Vec<IndexBlock> = Vec::new();
        let mut flat: Vec<LeafTermEntry> = Vec::new();
        let mut pageskip: Vec<u64> = Vec::new();
        pageskip.push(u64::MAX); // offset 0 means no skip list

        let mut cur  = IndexBlock::default();
        let mut wptr = 0usize;
        let mut seq  = 0u32;

        let flush = |pb: &mut Vec<IndexBlock>, cur: &mut IndexBlock,
                         wptr: &mut usize, seq: &mut u32, has_more: bool| {
            cur.ib_header = *seq as u64;
            if has_more { cur.ib_header |= IB_HEADER_HAS_MORE; }
            pb.push(cur.clone());
            *seq += 1; *cur = IndexBlock::default(); *wptr = 0;
        };

        for (key, pl) in &terms {
            let bytes = pl.get_bytes_ref();
            if bytes.is_empty() { continue; }

            let doc_freq = pl.doc_freq();
            let total    = bytes.len();
            let mut src  = 0usize;

            if wptr >= cap {
                flush(&mut index_blocks, &mut cur, &mut wptr, &mut seq, false);
            }

            let data_offset = wptr;
            let data_here   = (cap - wptr).min(total);
            let has_more    = data_here < total;

            cur.ib_data[wptr..wptr + data_here].copy_from_slice(&bytes[..data_here]);
            wptr += data_here;
            src  += data_here;

            flat.push(LeafTermEntry {
                lte_term: key.to_string(),
                lte_doc_freq: doc_freq,
                lte_index_block_id: seq,
                lte_index_offset: data_offset as u32,
                lte_index_length: data_here as u32,
                lte_page_skip_offset: 0,
                lte_continuation_block_count: 0,
                lte_flags: 0,
            });

            if has_more {
                flush(&mut index_blocks, &mut cur, &mut wptr, &mut seq, true);
                let skip_offset = pageskip.len() as u32;
                pageskip.push(0u64);
                flat.last_mut().unwrap().lte_page_skip_offset = skip_offset;

                while src < total {
                    const CONT_HDR: usize = 4;
                    let cont_here = (total - src).min(cap - CONT_HDR);
                    let more_cont = cont_here < total - src;

                    let base_doc = decode_last_docid(&bytes[..src]);
                    pageskip.push(base_doc);

                    let cm = CONT_MARKER.to_le_bytes();
                    let cl = (cont_here as u16).to_le_bytes();
                    cur.ib_data[wptr..wptr+2].copy_from_slice(&cm); wptr += 2;
                    cur.ib_data[wptr..wptr+2].copy_from_slice(&cl); wptr += 2;
                    cur.ib_data[wptr..wptr+cont_here].copy_from_slice(&bytes[src..src+cont_here]);
                    wptr += cont_here; src += cont_here;

                    flat.last_mut().unwrap().lte_continuation_block_count += 1;

                    if more_cont { flush(&mut index_blocks, &mut cur, &mut wptr, &mut seq, true); }
                }
                pageskip.push(u64::MAX);
            }
        }
        if wptr > 0 {
            flush(&mut index_blocks, &mut cur, &mut wptr, &mut seq, false);
        }

        let mut head: Vec<HeadTermEntry> = Vec::new();
        let mut leaf_blocks: Vec<LeafTermBlock> = Vec::new();
        let mut page_entries: Vec<LeafTermEntry> = Vec::new();
        let mut page_bytes = 4usize;
        for entry in flat {
            let need = leaf_entry_size(&entry);
            if !page_entries.is_empty() && page_bytes + need > PAGE_SIZE {
                let block_id = leaf_blocks.len() as u32;
                head.push(HeadTermEntry { hte_first_term: page_entries[0].lte_term.clone(), hte_leaf_term_block_id: block_id });
                leaf_blocks.push(LeafTermBlock { ltb_entries: page_entries });
                page_entries = Vec::new();
                page_bytes = 4;
            }
            page_bytes += need;
            page_entries.push(entry);
        }
        if !page_entries.is_empty() {
            let block_id = leaf_blocks.len() as u32;
            head.push(HeadTermEntry { hte_first_term: page_entries[0].lte_term.clone(), hte_leaf_term_block_id: block_id });
            leaf_blocks.push(LeafTermBlock { ltb_entries: page_entries });
        }

        BlocksResult {
            bbr_index_blocks: index_blocks,
            bbr_head_term_entries: head,
            bbr_leaf_term_blocks: leaf_blocks,
            bbr_page_skip_list: pageskip,
        }
    }

    fn encode_head_leaf_term_table(head: &[HeadTermEntry], blocks: &[LeafTermBlock]) -> Vec<u8> {
        let mut out = Vec::new();
        let push_u16 = |out: &mut Vec<u8>, v: u16| { out.extend_from_slice(&v.to_le_bytes()); };
        let push_u32 = |out: &mut Vec<u8>, v: u32| { out.extend_from_slice(&v.to_le_bytes()); };

        push_u32(&mut out, head.len() as u32);
        for d in head {
            push_u16(&mut out, d.hte_first_term.len() as u16);
            out.extend_from_slice(d.hte_first_term.as_bytes());
            push_u32(&mut out, d.hte_leaf_term_block_id);
        }

        push_u32(&mut out, blocks.len() as u32);
        for blk in blocks {
            let page_start = out.len();
            push_u32(&mut out, blk.ltb_entries.len() as u32);
            for e in &blk.ltb_entries {
                push_u16(&mut out, e.lte_term.len() as u16);
                out.extend_from_slice(e.lte_term.as_bytes());
                push_u32(&mut out, e.lte_doc_freq);
                push_u32(&mut out, e.lte_index_block_id);
                push_u32(&mut out, e.lte_index_offset);
                push_u32(&mut out, e.lte_index_length);
                push_u32(&mut out, e.lte_page_skip_offset);
                push_u32(&mut out, e.lte_continuation_block_count);
                push_u32(&mut out, e.lte_flags);
            }
            out.resize(page_start + PAGE_SIZE, 0);
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
        const REC: usize = 1024; const PMAX: usize = 1000;
        let mut out = Vec::new();
        for (doc_id, stats) in store.all_doc_stats() {
            let mut rec = [0u8; REC];
            rec[0..8].copy_from_slice(&doc_id.to_le_bytes());
            rec[8..12].copy_from_slice(&stats.importance.to_le_bytes());
            rec[12..16].copy_from_slice(&stats.doc_len.to_le_bytes());
            let plen = stats.path.len().min(PMAX - 1);
            rec[16..18].copy_from_slice(&(plen as u16).to_le_bytes());
            rec[24..24+plen].copy_from_slice(&stats.path.as_bytes()[..plen]);
            out.extend_from_slice(&rec);
        }
        out
    }
}

fn decode_last_docid(data: &[u8]) -> u64 {
    let mut pos = 0usize; let mut prev = 0u64;
    while pos < data.len() {
        let (delta, n) = vb_read(data, pos); pos += n;
        if pos >= data.len() { break; }
        let (_tf, m) = vb_read(data, pos); pos += m;
        prev += delta;
    }
    prev
}

fn leaf_entry_size(entry: &LeafTermEntry) -> usize {
    2 + entry.lte_term.len() + 7 * std::mem::size_of::<u32>()
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
