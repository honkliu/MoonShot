use std::fs::File;
use std::io::{BufReader, Read, Seek, Write};

use crate::block_table::{
    HeadTermEntry,
    IndexBlock,
    IndexBlockTable,
    IndexBlockContinuationHeader,
    TermMphfHash,
    TermMphfHeader,
    TermMphfSlotSeed,
    LeafTermBlock,
    LeafTermEntry,
    DocDataDecodeScore,
    PathPrefixSidecarEntry,
    PathPrefixSidecarHeader,
    DOC_PATH_MAX,
    DOC_REC_SIZE,
    DOC_VECTOR_DIM,
    DOC_VECTOR_OFFSET,
    DOC_PATH_OFFSET,
    PATH_PREFIX_SIDECAR_BYTES,
    PATH_PREFIX_SIDECAR_MAGIC,
    PATH_PREFIX_SIDECAR_VERSION,
    HEAD_TERM_KEY_MAX,
    INDEX_BLOCK_CONTINUATION_HEADER_SIZE,
    INDEX_FILE_HEADER_SIZE,
    INDEX_FORMAT_VERSION,
    TERM_MPHF_ENTRY_SIZE,
    TERM_MPHF_HEADER_SIZE,
    TERM_MPHF_MAGIC,
    LEAF_TERM_DATA_OFFSET,
    LEAF_TERM_DIRECTORY_COUNT,
    PAGE_SIZE,
};
use crate::error::{Result, RustBladeError};
use crate::posting_store::PostingStore;

const MAGIC: &[u8; 8] = b"MOONSHOT";

#[derive(Debug, Clone, Copy, Default)]
#[allow(non_snake_case)]
pub struct IndexFileHeader {
    pub IFH_AvgDocLength: f32,
    pub IFH_NumDocuments: u64,
    pub IFH_NumTerms: u64,
    pub IFH_HeadTermEntryOffset: u64,
    pub IFH_HeadTermEntryCount: u64,
    pub IFH_LeafTermBlockOffset: u64,
    pub IFH_LeafTermBlockCount: u64,
    pub IFH_DocDataOffset: u64,
    pub IFH_IndexBlockOffset: u64,
    pub IFH_IndexBlockCount: u64,
    pub IFH_TermMphfHeaderOffset: u64,
    pub IFH_TermMphfHeaderCount: u64,
    pub IFH_TermMphfDisplacementOffset: u64,
    pub IFH_TermMphfDisplacementCount: u64,
    pub IFH_TermMphfEntryOffset: u64,
    pub IFH_TermMphfEntryPageCount: u64,
}

impl IndexFileHeader {
    pub fn parse(data: &[u8]) -> Result<Self> {
        if data.len() < INDEX_FILE_HEADER_SIZE { return Err(RustBladeError::InvalidFormat); }
        if &data[0..8] != MAGIC { return Err(RustBladeError::InvalidFormat); }
        if u32_at(data, 8) != INDEX_FORMAT_VERSION { return Err(RustBladeError::InvalidFormat); }
        Ok(Self {
            IFH_AvgDocLength: f32_at(data, 12),
            IFH_NumDocuments: u64_at(data, 16),
            IFH_NumTerms: u64_at(data, 24),
            IFH_HeadTermEntryOffset: u64_at(data, 32),
            IFH_HeadTermEntryCount: u64_at(data, 40),
            IFH_LeafTermBlockOffset: u64_at(data, 48),
            IFH_LeafTermBlockCount: u64_at(data, 56),
            IFH_DocDataOffset: u64_at(data, 64),
            IFH_IndexBlockOffset: u64_at(data, 72),
            IFH_IndexBlockCount: u64_at(data, 80),
            IFH_TermMphfHeaderOffset: u64_at(data, 88),
            IFH_TermMphfHeaderCount: u64_at(data, 96),
            IFH_TermMphfDisplacementOffset: u64_at(data, 104),
            IFH_TermMphfDisplacementCount: u64_at(data, 112),
            IFH_TermMphfEntryOffset: u64_at(data, 120),
            IFH_TermMphfEntryPageCount: u64_at(data, 128),
        })
    }

    pub fn to_bytes(&self) -> [u8; INDEX_FILE_HEADER_SIZE] {
        let mut out = [0u8; INDEX_FILE_HEADER_SIZE];
        out[0..8].copy_from_slice(MAGIC);
        write_u32(&mut out, 8, INDEX_FORMAT_VERSION);
        write_f32(&mut out, 12, self.IFH_AvgDocLength);
        write_u64(&mut out, 16, self.IFH_NumDocuments);
        write_u64(&mut out, 24, self.IFH_NumTerms);
        write_u64(&mut out, 32, self.IFH_HeadTermEntryOffset);
        write_u64(&mut out, 40, self.IFH_HeadTermEntryCount);
        write_u64(&mut out, 48, self.IFH_LeafTermBlockOffset);
        write_u64(&mut out, 56, self.IFH_LeafTermBlockCount);
        write_u64(&mut out, 64, self.IFH_DocDataOffset);
        write_u64(&mut out, 72, self.IFH_IndexBlockOffset);
        write_u64(&mut out, 80, self.IFH_IndexBlockCount);
        write_u64(&mut out, 88, self.IFH_TermMphfHeaderOffset);
        write_u64(&mut out, 96, self.IFH_TermMphfHeaderCount);
        write_u64(&mut out, 104, self.IFH_TermMphfDisplacementOffset);
        write_u64(&mut out, 112, self.IFH_TermMphfDisplacementCount);
        write_u64(&mut out, 120, self.IFH_TermMphfEntryOffset);
        write_u64(&mut out, 128, self.IFH_TermMphfEntryPageCount);
        out
    }
}

#[allow(non_snake_case)]
pub struct BuildBlocksResult {
    pub BBR_IndexBlocks: Vec<IndexBlock>,
    pub BBR_HeadTermEntries: Vec<HeadTermEntry>,
    pub BBR_LeafTermBlocks: Vec<LeafTermBlock>,
    pub BBR_TermMphfHeader: TermMphfHeader,
    pub BBR_TermMphfDisplacements: Vec<i32>,
    pub BBR_TermMphfEntryPages: Vec<IndexBlock>,
    pub BBR_TotalTerms: u64,
}

pub struct IndexSerializer;

#[derive(Clone)]
#[allow(non_snake_case)]
struct TermMphfBuildTerm {
    Term: String,
    DocFreq: u32,
    IndexBlockID: u32,
    IndexOffset: u32,
    IndexLength: u32,
    ContinuationBlockCount: u32,
    Flags: u32,
}

fn next_power_of_two(mut value: u32) -> u32 {
    if value <= 1 { return 1; }
    value -= 1;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    value + 1
}

#[allow(non_snake_case)]
fn MphfSlotFor(term: &str, slot_count: u32, slot_seed: u64, displacement: u32) -> u32 {
    (TermMphfHash(term.as_bytes(), TermMphfSlotSeed(slot_seed, displacement)) % slot_count as u64) as u32
}

#[allow(non_snake_case)]
fn TryBuildTermMphf(terms: &[TermMphfBuildTerm], bucket_seed: u64, slot_seed: u64, fingerprint_seed: u64)
    -> Option<(TermMphfHeader, Vec<i32>, Vec<IndexBlock>)>
{
    let term_count = terms.len() as u32;
    if term_count == 0 { return None; }
    let bucket_count = next_power_of_two(std::cmp::max(1, term_count / 2));
    let slot_count = term_count;
    let max_displacement = std::cmp::min(1u32 << 20, std::cmp::max(65536u32, std::cmp::max(1u32, slot_count / 8)));

    let mut ids: Vec<usize> = (0..terms.len()).collect();
    ids.sort_by(|a, b| terms[*a].Term.cmp(&terms[*b].Term));
    for index in 1..ids.len() {
        if terms[ids[index - 1]].Term == terms[ids[index]].Term { return None; }
    }

    let mut buckets = vec![Vec::<usize>::new(); bucket_count as usize];
    for (index, term) in terms.iter().enumerate() {
        let bucket = (TermMphfHash(term.Term.as_bytes(), bucket_seed) % bucket_count as u64) as usize;
        buckets[bucket].push(index);
    }

    let mut order: Vec<usize> = (0..bucket_count as usize).collect();
    order.sort_by(|a, b| buckets[*b].len().cmp(&buckets[*a].len()));

    let mut displacements = vec![-1i32; bucket_count as usize];
    let mut used = vec![false; slot_count as usize];
    let mut slots = vec![u32::MAX; term_count as usize];

    for bucket in order {
        let bucket_terms = &buckets[bucket];
        if bucket_terms.is_empty() {
            displacements[bucket] = 0;
            continue;
        }
        if bucket_terms.len() == 1 {
            continue;
        }

        let mut placed = false;
        let mut candidate_slots = vec![0u32; bucket_terms.len()];
        for displacement in 0..max_displacement {
            let mut ok = true;
            for (i, term_index) in bucket_terms.iter().enumerate() {
                let slot = MphfSlotFor(&terms[*term_index].Term, slot_count, slot_seed, displacement);
                candidate_slots[i] = slot;
                if used[slot as usize] || candidate_slots[..i].iter().any(|candidate| *candidate == slot) {
                    ok = false;
                    break;
                }
            }
            if !ok { continue; }

            displacements[bucket] = displacement as i32;
            for (i, term_index) in bucket_terms.iter().enumerate() {
                used[candidate_slots[i] as usize] = true;
                slots[*term_index] = candidate_slots[i];
            }
            placed = true;
            break;
        }

        if !placed { return None; }
    }

    let mut free_slots = Vec::with_capacity(slot_count as usize);
    for slot in 0..slot_count {
        if !used[slot as usize] {
            free_slots.push(slot);
        }
    }
    let mut next_free_slot = 0usize;
    for (bucket, bucket_terms) in buckets.iter().enumerate() {
        if bucket_terms.len() != 1 { continue; }
        if next_free_slot >= free_slots.len() { return None; }
        let slot = free_slots[next_free_slot];
        next_free_slot += 1;
        let direct_displacement = -(slot as i64) - 1;
        if direct_displacement < i32::MIN as i64 { return None; }
        displacements[bucket] = direct_displacement as i32;
        used[slot as usize] = true;
        slots[bucket_terms[0]] = slot;
    }

    let page_count = (slot_count as usize + (PAGE_SIZE / TERM_MPHF_ENTRY_SIZE) - 1) / (PAGE_SIZE / TERM_MPHF_ENTRY_SIZE);
    let mut entry_pages = vec![IndexBlock::default(); page_count];
    for (index, source) in terms.iter().enumerate() {
        let slot = slots[index];
        if slot == u32::MAX { return None; }
        let byte_offset = slot as usize * TERM_MPHF_ENTRY_SIZE;
        let page = byte_offset / PAGE_SIZE;
        let offset = byte_offset % PAGE_SIZE;
        let data = &mut entry_pages[page].IB_Data[offset..offset + TERM_MPHF_ENTRY_SIZE];
        data[0..4].copy_from_slice(&source.DocFreq.to_le_bytes());
        data[4..8].copy_from_slice(&source.IndexBlockID.to_le_bytes());
        data[8..12].copy_from_slice(&source.IndexOffset.to_le_bytes());
        data[12..16].copy_from_slice(&source.IndexLength.to_le_bytes());
        data[16..20].copy_from_slice(&source.ContinuationBlockCount.to_le_bytes());
        data[20..24].copy_from_slice(&source.Flags.to_le_bytes());
        let mut fingerprint = TermMphfHash(source.Term.as_bytes(), fingerprint_seed);
        if fingerprint == 0 { fingerprint = 1; }
        data[24..32].copy_from_slice(&fingerprint.to_le_bytes());
    }

    Some((TermMphfHeader {
        TMH_Magic: TERM_MPHF_MAGIC,
        TMH_TermCount: term_count as u64,
        TMH_BucketCount: bucket_count,
        TMH_SlotCount: slot_count,
        TMH_BucketSeed: bucket_seed,
        TMH_SlotSeed: slot_seed,
        TMH_FingerprintSeed: fingerprint_seed,
    }, displacements, entry_pages))
}

#[allow(non_snake_case)]
fn BuildTermMphf(terms: &[TermMphfBuildTerm]) -> (TermMphfHeader, Vec<i32>, Vec<IndexBlock>) {
    if terms.is_empty() { return (TermMphfHeader::default(), Vec::new(), Vec::new()); }
    const BUCKET_SEED_BASE: u64 = 0x9ae16a3b2f90404f;
    const SLOT_SEED_BASE: u64 = 0xc3a5c85c97cb3127;
    const FINGERPRINT_SEED_BASE: u64 = 0xb492b66fbe98f273;
    const SEED_STEP: u64 = 0x9e3779b97f4a7c15;

    for attempt in 0..32u64 {
        let bucket_seed = BUCKET_SEED_BASE.wrapping_add(SEED_STEP.wrapping_mul(attempt));
        let slot_seed = SLOT_SEED_BASE ^ SEED_STEP.wrapping_mul(attempt + 1);
        let fingerprint_seed = FINGERPRINT_SEED_BASE.wrapping_add(SEED_STEP.wrapping_mul(attempt + 3));
        if let Some(result) = TryBuildTermMphf(terms, bucket_seed, slot_seed, fingerprint_seed) {
            return result;
        }
    }

    (TermMphfHeader::default(), Vec::new(), Vec::new())
}

impl IndexSerializer {
    #[allow(non_snake_case)]
    pub fn Save(header: &IndexFileHeader, blockTable: &IndexBlockTable, docData: &[u8], pathPrefixSidecar: &[u8], path: &str) -> Result<()> {
        let mut file = File::create(path)?;
        file.write_all(&header.to_bytes())?;
        if pathPrefixSidecar.len() != PATH_PREFIX_SIDECAR_BYTES { return Err(RustBladeError::InvalidFormat); }
        file.write_all(pathPrefixSidecar)?;

        if header.IFH_HeadTermEntryCount > 0 {
            let mut bytes = Vec::with_capacity(header.IFH_HeadTermEntryCount as usize * 32);
            for entry in blockTable.HeadTermEntries() {
                bytes.extend_from_slice(&entry.to_bytes());
            }
            file.write_all(&bytes)?;
        }

        if header.IFH_LeafTermBlockCount > 0 {
            let blocks = blockTable.LeafTermBlocks();
            let mut bytes = Vec::with_capacity(blocks.len() * PAGE_SIZE);
            for block in &blocks {
                bytes.extend_from_slice(&block.to_bytes());
            }
            file.write_all(&bytes)?;
        }

        if header.IFH_NumDocuments > 0 {
            file.write_all(docData)?;
        }

        if header.IFH_IndexBlockCount > 0 {
            let blocks = blockTable.IndexBlocks();
            let mut bytes = Vec::with_capacity(blocks.len() * PAGE_SIZE);
            for block in &blocks {
                bytes.extend_from_slice(&block.IB_Data);
            }
            file.write_all(&bytes)?;
        }

        if header.IFH_TermMphfHeaderCount > 0 {
            file.write_all(&blockTable.TermMphfHeader().to_bytes())?;
        }

        if header.IFH_TermMphfDisplacementCount > 0 {
            for displacement in blockTable.TermMphfDisplacements() {
                file.write_all(&displacement.to_le_bytes())?;
            }
        }

        if header.IFH_TermMphfEntryPageCount > 0 {
            for block in blockTable.TermMphfEntryPages() {
                file.write_all(&block.IB_Data)?;
            }
        }

        file.flush()?;
        Ok(())
    }

    #[allow(non_snake_case)]
    pub fn LoadFileTables(store: &mut PostingStore, path: &str)
        -> Result<(IndexFileHeader, Vec<HeadTermEntry>, Vec<u8>)>
    {
        let file = File::open(path)?;
        let mut reader = BufReader::new(file);
        let mut header_bytes = [0u8; INDEX_FILE_HEADER_SIZE];
        reader.read_exact(&mut header_bytes)?;
        let header = IndexFileHeader::parse(&header_bytes)?;

        let mut head = Vec::with_capacity(header.IFH_HeadTermEntryCount as usize);
        reader.seek(std::io::SeekFrom::Start(header.IFH_HeadTermEntryOffset))?;
        for _ in 0..header.IFH_HeadTermEntryCount {
            let mut bytes = [0u8; 32];
            reader.read_exact(&mut bytes)?;
            head.push(HeadTermEntry::from_bytes(&bytes).ok_or(RustBladeError::InvalidFormat)?);
        }

        let docdata = Self::LoadDocData(store, &mut reader, &header)?;
        Ok((header, head, docdata))
    }

    #[allow(non_snake_case)]
    pub fn LoadPathPrefixSidecar(path: &str) -> Result<(Vec<u8>, Vec<String>)> {
        let mut file = File::open(path)?;
        file.seek(std::io::SeekFrom::Start(INDEX_FILE_HEADER_SIZE as u64))?;
        let mut sidecar = vec![0u8; PATH_PREFIX_SIDECAR_BYTES];
        file.read_exact(&mut sidecar)?;
        let prefixes = Self::DecodePathPrefixSidecar(&sidecar)?;
        Ok((sidecar, prefixes))
    }

    #[allow(non_snake_case)]
    pub fn LoadTermMphf(path: &str, header: &IndexFileHeader) -> Result<(TermMphfHeader, Vec<i32>, Vec<IndexBlock>)> {
        if header.IFH_TermMphfHeaderCount == 0 {
            return Ok((TermMphfHeader::default(), Vec::new(), Vec::new()));
        }
        if header.IFH_TermMphfHeaderCount != 1 || header.IFH_TermMphfDisplacementCount == 0 || header.IFH_TermMphfEntryPageCount == 0 {
            return Err(RustBladeError::InvalidFormat);
        }

        let mut file = File::open(path)?;
        file.seek(std::io::SeekFrom::Start(header.IFH_TermMphfHeaderOffset))?;
        let mut header_bytes = [0u8; TERM_MPHF_HEADER_SIZE];
        file.read_exact(&mut header_bytes)?;
        let mphf_header = TermMphfHeader::from_bytes(&header_bytes).ok_or(RustBladeError::InvalidFormat)?;
        if mphf_header.TMH_Magic != TERM_MPHF_MAGIC { return Err(RustBladeError::InvalidFormat); }

        file.seek(std::io::SeekFrom::Start(header.IFH_TermMphfDisplacementOffset))?;
        let mut displacements = Vec::with_capacity(header.IFH_TermMphfDisplacementCount as usize);
        for _ in 0..header.IFH_TermMphfDisplacementCount {
            let mut bytes = [0u8; 4];
            file.read_exact(&mut bytes)?;
            displacements.push(i32::from_le_bytes(bytes));
        }

        file.seek(std::io::SeekFrom::Start(header.IFH_TermMphfEntryOffset))?;
        let mut pages = Vec::with_capacity(header.IFH_TermMphfEntryPageCount as usize);
        for _ in 0..header.IFH_TermMphfEntryPageCount {
            let mut block = IndexBlock::default();
            file.read_exact(&mut block.IB_Data)?;
            pages.push(block);
        }

        Ok((mphf_header, displacements, pages))
    }

    #[allow(non_snake_case)]
    pub fn BuildTermMphfFromLeafBlocks(leaf_blocks: &[LeafTermBlock]) -> (TermMphfHeader, Vec<i32>, Vec<IndexBlock>) {
        let mut terms = Vec::new();
        for block in leaf_blocks {
            for entry in block.entries() {
                terms.push(TermMphfBuildTerm {
                    Term: entry.LTE_Term,
                    DocFreq: entry.LTE_DocFreq,
                    IndexBlockID: entry.LTE_IndexBlockID,
                    IndexOffset: entry.LTE_IndexOffset as u32,
                    IndexLength: entry.LTE_IndexLength as u32,
                    ContinuationBlockCount: entry.LTE_ContinuationBlockCount as u32,
                    Flags: entry.LTE_Flags as u32,
                });
            }
        }
        BuildTermMphf(&terms)
    }

    pub fn load_file_tables(store: &mut PostingStore, path: &str)
        -> Result<(IndexFileHeader, Vec<HeadTermEntry>, Vec<u8>)>
    {
        Self::LoadFileTables(store, path)
    }

    pub fn decode(store: &mut PostingStore, data: &[u8])
        -> Result<(Vec<HeadTermEntry>, Vec<LeafTermBlock>, Vec<IndexBlock>, Vec<u8>, Vec<u8>, Vec<String>, TermMphfHeader, Vec<i32>, Vec<IndexBlock>)>
    {
        let header = IndexFileHeader::parse(data)?;

        if INDEX_FILE_HEADER_SIZE + PATH_PREFIX_SIDECAR_BYTES > data.len() { return Err(RustBladeError::InvalidFormat); }
        let sidecar = data[INDEX_FILE_HEADER_SIZE..INDEX_FILE_HEADER_SIZE + PATH_PREFIX_SIDECAR_BYTES].to_vec();
        let prefixes = Self::DecodePathPrefixSidecar(&sidecar)?;

        let head_offset = header.IFH_HeadTermEntryOffset as usize;
        let head_count = header.IFH_HeadTermEntryCount as usize;
        let leaf_offset = header.IFH_LeafTermBlockOffset as usize;
        let leaf_count = header.IFH_LeafTermBlockCount as usize;
        let docdata_offset = header.IFH_DocDataOffset as usize;
        let index_offset = header.IFH_IndexBlockOffset as usize;
        let index_count = header.IFH_IndexBlockCount as usize;
        let mphf_header_offset = header.IFH_TermMphfHeaderOffset as usize;
        let mphf_displacement_offset = header.IFH_TermMphfDisplacementOffset as usize;
        let mphf_displacement_count = header.IFH_TermMphfDisplacementCount as usize;
        let mphf_entry_offset = header.IFH_TermMphfEntryOffset as usize;
        let mphf_entry_page_count = header.IFH_TermMphfEntryPageCount as usize;
        let num_docs = header.IFH_NumDocuments as usize;

        let mut head = Vec::with_capacity(head_count);
        for index in 0..head_count {
            let offset = head_offset + index * 32;
            if offset + 32 > data.len() { return Err(RustBladeError::InvalidFormat); }
            head.push(HeadTermEntry::from_bytes(&data[offset..offset + 32]).ok_or(RustBladeError::InvalidFormat)?);
        }

        let mut leaf_blocks = Vec::with_capacity(leaf_count);
        for index in 0..leaf_count {
            let offset = leaf_offset + index * PAGE_SIZE;
            if offset + PAGE_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            leaf_blocks.push(LeafTermBlock::from_bytes(&data[offset..offset + PAGE_SIZE]).ok_or(RustBladeError::InvalidFormat)?);
        }

        let docdata_size = num_docs * DOC_REC_SIZE;
        if docdata_offset + docdata_size > data.len() { return Err(RustBladeError::InvalidFormat); }
        let docdata = data[docdata_offset..docdata_offset + docdata_size].to_vec();

        let first_doc_id = Self::DocDataFirstDocId(&docdata, &header);
        for index in 0..num_docs {
            let offset = docdata_offset + index * DOC_REC_SIZE;
            Self::DecodeDocDataRecord(store, first_doc_id + index as u64, &data[offset..offset + DOC_REC_SIZE])?;
        }

        let mut index_blocks = Vec::with_capacity(index_count);
        for index in 0..index_count {
            let offset = index_offset + index * PAGE_SIZE;
            if offset + PAGE_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            let mut block = IndexBlock::default();
            block.IB_Data.copy_from_slice(&data[offset..offset + PAGE_SIZE]);
            index_blocks.push(block);
        }

        let (mphf_header, mphf_displacements, mphf_entry_pages) = if header.IFH_TermMphfHeaderCount == 0 {
            (TermMphfHeader::default(), Vec::new(), Vec::new())
        } else {
            if header.IFH_TermMphfHeaderCount != 1 { return Err(RustBladeError::InvalidFormat); }
            if mphf_header_offset + TERM_MPHF_HEADER_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            let mphf_header = TermMphfHeader::from_bytes(&data[mphf_header_offset..mphf_header_offset + TERM_MPHF_HEADER_SIZE]).ok_or(RustBladeError::InvalidFormat)?;
            if mphf_header.TMH_Magic != TERM_MPHF_MAGIC { return Err(RustBladeError::InvalidFormat); }
            if mphf_displacement_offset + mphf_displacement_count * 4 > data.len() { return Err(RustBladeError::InvalidFormat); }
            let mut mphf_displacements = Vec::with_capacity(mphf_displacement_count);
            for index in 0..mphf_displacement_count {
                let offset = mphf_displacement_offset + index * 4;
                mphf_displacements.push(i32::from_le_bytes(data[offset..offset + 4].try_into().unwrap()));
            }
            if mphf_entry_offset + mphf_entry_page_count * PAGE_SIZE > data.len() { return Err(RustBladeError::InvalidFormat); }
            let mut mphf_entry_pages = Vec::with_capacity(mphf_entry_page_count);
            for index in 0..mphf_entry_page_count {
                let offset = mphf_entry_offset + index * PAGE_SIZE;
                let mut block = IndexBlock::default();
                block.IB_Data.copy_from_slice(&data[offset..offset + PAGE_SIZE]);
                mphf_entry_pages.push(block);
            }
            (mphf_header, mphf_displacements, mphf_entry_pages)
        };

        Ok((head, leaf_blocks, index_blocks, docdata, sidecar, prefixes, mphf_header, mphf_displacements, mphf_entry_pages))
    }

    #[allow(non_snake_case)]
    fn LoadDocData<R: Read + std::io::Seek>(store: &mut PostingStore, reader: &mut R, header: &IndexFileHeader) -> Result<Vec<u8>> {
        reader.seek(std::io::SeekFrom::Start(header.IFH_DocDataOffset))?;
        let mut docdata = vec![0u8; header.IFH_NumDocuments as usize * DOC_REC_SIZE];
        reader.read_exact(&mut docdata)?;
        let first_doc_id = Self::DocDataFirstDocId(&docdata, header);
        for index in 0..header.IFH_NumDocuments as usize {
            let offset = index * DOC_REC_SIZE;
            Self::DecodeDocDataRecord(store, first_doc_id + index as u64, &docdata[offset..offset + DOC_REC_SIZE])?;
        }
        Ok(docdata)
    }

    #[allow(non_snake_case)]
    pub fn DocDataFirstDocId(docdata: &[u8], header: &IndexFileHeader) -> u64 {
        if header.IFH_NumDocuments > 0 && docdata.len() >= DOC_REC_SIZE {
            u32_at(docdata, 0) as u64
        } else {
            0
        }
    }

    #[allow(non_snake_case)]
    pub fn DecodePathPrefixSidecar(sidecar: &[u8]) -> Result<Vec<String>> {
        if sidecar.len() != PATH_PREFIX_SIDECAR_BYTES { return Err(RustBladeError::InvalidFormat); }
        let header = PathPrefixSidecarHeader {
            PPSH_Magic: sidecar[0..8].try_into().map_err(|_| RustBladeError::InvalidFormat)?,
            PPSH_Version: u16_at(sidecar, 8),
            PPSH_PrefixCount: u16_at(sidecar, 10),
            PPSH_EntryOffset: u32_at(sidecar, 12),
            PPSH_StringOffset: u32_at(sidecar, 16),
            PPSH_StringBytes: u32_at(sidecar, 20),
            PPSH_Reserved: sidecar[24..32].try_into().map_err(|_| RustBladeError::InvalidFormat)?,
        };
        if &header.PPSH_Magic != PATH_PREFIX_SIDECAR_MAGIC
            || header.PPSH_Version != PATH_PREFIX_SIDECAR_VERSION
            || (header.PPSH_EntryOffset as usize) < 32
            || (header.PPSH_StringOffset as usize) > PATH_PREFIX_SIDECAR_BYTES
            || (header.PPSH_StringOffset as usize) + (header.PPSH_StringBytes as usize) > PATH_PREFIX_SIDECAR_BYTES
            || (header.PPSH_EntryOffset as usize) + (header.PPSH_PrefixCount as usize) * 8 > PATH_PREFIX_SIDECAR_BYTES
        {
            return Err(RustBladeError::InvalidFormat);
        }

        let mut prefixes = Vec::with_capacity(header.PPSH_PrefixCount as usize);
        for index in 0..header.PPSH_PrefixCount as usize {
            let offset = header.PPSH_EntryOffset as usize + index * 8;
            let entry = PathPrefixSidecarEntry {
                PPSE_Offset: u32_at(sidecar, offset),
                PPSE_Length: u16_at(sidecar, offset + 4),
                PPSE_Flags: u16_at(sidecar, offset + 6),
            };
            let begin = header.PPSH_StringOffset as usize + entry.PPSE_Offset as usize;
            let end = begin + entry.PPSE_Length as usize;
            if end > header.PPSH_StringOffset as usize + header.PPSH_StringBytes as usize { return Err(RustBladeError::InvalidFormat); }
            prefixes.push(std::str::from_utf8(&sidecar[begin..end]).map_err(|_| RustBladeError::InvalidFormat)?.to_string());
        }
        Ok(prefixes)
    }

    #[allow(non_snake_case)]
    pub fn EncodePathPrefixSidecar(prefixes: &[String]) -> Vec<u8> {
        let mut sidecar = vec![0u8; PATH_PREFIX_SIDECAR_BYTES];
        sidecar[0..8].copy_from_slice(PATH_PREFIX_SIDECAR_MAGIC);
        sidecar[8..10].copy_from_slice(&PATH_PREFIX_SIDECAR_VERSION.to_le_bytes());
        let count = prefixes.len().min(u16::MAX as usize) as u16;
        sidecar[10..12].copy_from_slice(&count.to_le_bytes());
        let entry_offset = 32u32;
        let string_offset = entry_offset + count as u32 * 8;
        sidecar[12..16].copy_from_slice(&entry_offset.to_le_bytes());
        sidecar[16..20].copy_from_slice(&string_offset.to_le_bytes());

        let mut cursor = 0u32;
        for (index, prefix) in prefixes.iter().take(count as usize).enumerate() {
            let entry_offset = entry_offset as usize + index * 8;
            sidecar[entry_offset..entry_offset + 4].copy_from_slice(&cursor.to_le_bytes());
            sidecar[entry_offset + 4..entry_offset + 6].copy_from_slice(&(prefix.len() as u16).to_le_bytes());
            let string_begin = string_offset as usize + cursor as usize;
            sidecar[string_begin..string_begin + prefix.len()].copy_from_slice(prefix.as_bytes());
            cursor += prefix.len() as u32;
        }
        sidecar[20..24].copy_from_slice(&cursor.to_le_bytes());
        sidecar
    }

    #[allow(non_snake_case)]
    fn DecodeDocDataRecord(store: &mut PostingStore, index: u64, record: &[u8]) -> Result<()> {
        let doc_id = u32_at(record, 0) as u64;
        if doc_id != index { return Ok(()); }
        let importance = DocDataDecodeScore(u16_at(record, 4));
        let path_len = u16_at(record, 18) as usize;
        let title_len = u32_at(record, 26);
        let body_len = u32_at(record, 30);
        let url_len = u32_at(record, 34);
        let anchor_len = u32_at(record, 38);
        let meta_len = u32_at(record, 42);
        let vector_dim = u16_at(record, 54) as usize;
        let vector_format = u16_at(record, 56);
        store.SetDocLengths(doc_id, title_len, body_len, url_len, anchor_len, meta_len);
        store.SetDocImportance(doc_id, importance);
        if vector_dim == DOC_VECTOR_DIM && vector_format != 0 {
            store.SetDocVectorBytes(doc_id, &record[DOC_VECTOR_OFFSET..DOC_VECTOR_OFFSET + DOC_VECTOR_DIM]);
        }
        if path_len > 0 && path_len <= DOC_PATH_MAX {
            if let Ok(path) = std::str::from_utf8(&record[DOC_PATH_OFFSET..DOC_PATH_OFFSET + path_len]) {
                store.SetDocPath(doc_id, path.to_string());
            }
        }
        Ok(())
    }

    #[allow(non_snake_case)]
    pub fn IsValidIndex(path: &str) -> bool {
        let Ok(mut file) = File::open(path) else { return false; };
        let mut header = [0u8; 8];
        file.read_exact(&mut header).is_ok()
            && &header == MAGIC
    }

            pub fn is_valid_index(path: &str) -> bool { Self::IsValidIndex(path) }

    #[allow(non_snake_case)]
    pub fn BuildBlocks(store: &PostingStore) -> BuildBlocksResult {
        let mut terms: Vec<(&String, &crate::posting_store::PostingList)> = store.AllPostings().iter().collect();
        terms.sort_by_key(|(term, _)| term.as_str());

        let mut IndexBlocks = Vec::new();
        let mut cur = IndexBlock::default();
        let mut wptr = 0usize;
        let mut seq = 0u32;

        let mut leaf_blocks = Vec::new();
        let mut leaf_block = LeafTermBlock::default();
        let mut leaf_write_offset = 0usize;
        let mut leaf_entry_count = 0usize;
        let mut first_leaf_term = String::new();

        let flush_index_block = |IndexBlocks: &mut Vec<IndexBlock>, cur: &mut IndexBlock, wptr: &mut usize, seq: &mut u32| {
            IndexBlocks.push(cur.clone());
            *seq += 1;
            *cur = IndexBlock::default();
            *wptr = 0;
        };

        let flush_leaf_block = |leaf_blocks: &mut Vec<LeafTermBlock>,
                                head: &mut Vec<HeadTermEntry>,
                                leaf_block: &mut LeafTermBlock,
                                leaf_write_offset: &mut usize,
                                leaf_entry_count: &mut usize,
                                first_leaf_term: &mut String| {
            if *leaf_entry_count == 0 { return; }
            leaf_block.LTB_Directory[LEAF_TERM_DIRECTORY_COUNT - 1] = *leaf_entry_count as u16;
            head.push(HeadTermEntry::new(first_leaf_term, leaf_blocks.len() as u32));
            leaf_blocks.push(leaf_block.clone());
            *leaf_block = LeafTermBlock::default();
            *leaf_write_offset = 0;
            *leaf_entry_count = 0;
            first_leaf_term.clear();
        };

        let mut head_entries = Vec::new();
        let mut total_terms = 0u64;
        for (term, posting_list) in terms {
            if term.len() > HEAD_TERM_KEY_MAX { continue; }
            let bytes = posting_list.get_bytes();
            if bytes.is_empty() { continue; }

            if wptr >= PAGE_SIZE {
                flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);
            }

            let mut src = 0usize;
            let mut remaining = bytes.len();
            let mut data_offset = wptr;
            let mut data_here = PostingPrefixBytes(&bytes[src..], PAGE_SIZE - wptr);
            if data_here == 0 {
                flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);
                data_offset = wptr;
                data_here = PostingPrefixBytes(&bytes[src..], PAGE_SIZE);
                if data_here == 0 { continue; }
            }

            let index_block_id = seq;
            cur.IB_Data[wptr..wptr + data_here].copy_from_slice(&bytes[src..src + data_here]);
            wptr += data_here;
            src += data_here;
            remaining -= data_here;
            let mut continuation_block_count = 0u32;

            if remaining > 0 {
                flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);

                while remaining > 0 {
                    let cont_cap = PAGE_SIZE - INDEX_BLOCK_CONTINUATION_HEADER_SIZE;
                    let cont_here = PostingPrefixBytes(&bytes[src..], cont_cap);
                    if cont_here == 0 { break; }
                    let more_cont = cont_here < remaining;
                    let cont_max_doc_id = MaxDocIDInPairs(&bytes[src..src + cont_here]);
                    IndexBlockContinuationHeader {
                        IBCH_MaxDocID: cont_max_doc_id,
                        IBCH_DataLength: cont_here as u32,
                    }.write_to(&mut cur.IB_Data[wptr..wptr + INDEX_BLOCK_CONTINUATION_HEADER_SIZE]);
                    wptr += INDEX_BLOCK_CONTINUATION_HEADER_SIZE;
                    cur.IB_Data[wptr..wptr + cont_here].copy_from_slice(&bytes[src..src + cont_here]);
                    wptr += cont_here;
                    src += cont_here;
                    remaining -= cont_here;
                    continuation_block_count += 1;
                    if more_cont {
                        flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq);
                    }
                }
            }

            let leaf_entry = LeafTermEntry {
                LTE_Term: term.clone(),
                LTE_DocFreq: posting_list.doc_freq(),
                LTE_IndexBlockID: index_block_id,
                LTE_IndexOffset: data_offset as u16,
                LTE_IndexLength: data_here as u16,
                LTE_ContinuationBlockCount: continuation_block_count as u16,
                LTE_Flags: 0,
            };
            let entry_bytes = leaf_entry.byte_len();
            if leaf_entry_count > 0
                && (leaf_entry_count >= LEAF_TERM_DIRECTORY_COUNT - 1
                    || leaf_write_offset + entry_bytes > PAGE_SIZE - LEAF_TERM_DATA_OFFSET)
            {
                flush_leaf_block(&mut leaf_blocks, &mut head_entries, &mut leaf_block, &mut leaf_write_offset, &mut leaf_entry_count, &mut first_leaf_term);
            }
            if leaf_entry_count == 0 { first_leaf_term = term.clone(); }
            WriteLeafEntry(&mut leaf_block, leaf_entry_count, leaf_write_offset, &leaf_entry);
            leaf_write_offset += entry_bytes;
            leaf_entry_count += 1;
            total_terms += 1;
        }

        if wptr > 0 { flush_index_block(&mut IndexBlocks, &mut cur, &mut wptr, &mut seq); }
        flush_leaf_block(&mut leaf_blocks, &mut head_entries, &mut leaf_block, &mut leaf_write_offset, &mut leaf_entry_count, &mut first_leaf_term);
        let (mphf_header, mphf_displacements, mphf_entry_pages) = (TermMphfHeader::default(), Vec::new(), Vec::new());

        BuildBlocksResult {
            BBR_IndexBlocks: IndexBlocks,
            BBR_HeadTermEntries: head_entries,
            BBR_LeafTermBlocks: leaf_blocks,
            BBR_TermMphfHeader: mphf_header,
            BBR_TermMphfDisplacements: mphf_displacements,
            BBR_TermMphfEntryPages: mphf_entry_pages,
            BBR_TotalTerms: total_terms,
        }
    }

}

#[allow(non_snake_case)]
fn WriteLeafEntry(block: &mut LeafTermBlock, entryIndex: usize, offset: usize, entry: &LeafTermEntry) {
    block.LTB_Directory[entryIndex] = (LEAF_TERM_DATA_OFFSET + offset) as u16;
    let data = &mut block.LTB_Data[offset..];
    data[0..4].copy_from_slice(&entry.LTE_DocFreq.to_le_bytes());
    data[4..8].copy_from_slice(&entry.LTE_IndexBlockID.to_le_bytes());
    data[8..10].copy_from_slice(&entry.LTE_IndexOffset.to_le_bytes());
    data[10..12].copy_from_slice(&entry.LTE_IndexLength.to_le_bytes());
    data[12..14].copy_from_slice(&entry.LTE_ContinuationBlockCount.to_le_bytes());
    data[14] = entry.LTE_Flags;
    data[15] = entry.LTE_Term.len() as u8;
    data[16..16 + entry.LTE_Term.len()].copy_from_slice(entry.LTE_Term.as_bytes());
}

#[allow(non_snake_case)]
fn ReadVbcPairEnd(data: &[u8], offset: &mut usize) -> bool {
    let readOne = |data: &[u8], offset: &mut usize| -> bool {
        while *offset < data.len() {
            let byte = data[*offset];
            *offset += 1;
            if byte & 0x80 == 0 { return true; }
        }
        false
    };
    if !readOne(data, offset) || *offset >= data.len() { return false; }
    *offset += 1;
    true
}

#[allow(non_snake_case)]
fn PostingPrefixBytes(data: &[u8], capacity: usize) -> usize {
    let mut cursor = 0usize;
    let mut last_pair_end = 0usize;
    let limit = data.len().min(capacity);
    while cursor < limit {
        if !ReadVbcPairEnd(data, &mut cursor) || cursor > limit {
            break;
        }
        last_pair_end = cursor;
    }
    last_pair_end
}

#[allow(non_snake_case)]
fn MaxDocIDInPairs(data: &[u8]) -> u64 {
    let mut cursor = 0usize;
    let mut max_doc_id = 0u64;
    while cursor < data.len() {
        let (doc_id, doc_bytes) = VbRead(data, cursor);
        cursor += doc_bytes;
        if cursor >= data.len() { break; }
        cursor += 1;
        max_doc_id = doc_id;
    }
    max_doc_id
}


#[allow(non_snake_case)]
fn VbRead(data: &[u8], start: usize) -> (u64, usize) {
    let mut value = 0u64;
    let mut shift = 0u8;
    let mut pos = start;
    loop {
        if pos >= data.len() { break; }
        let byte = data[pos];
        pos += 1;
        value |= ((byte & 0x7F) as u64) << shift;
        if byte & 0x80 == 0 { break; }
        shift += 7;
    }
    (value, pos - start)
}

fn u16_at(data: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(data[offset..offset + 2].try_into().unwrap())
}
fn u32_at(data: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}
fn u64_at(data: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(data[offset..offset + 8].try_into().unwrap())
}
fn f32_at(data: &[u8], offset: usize) -> f32 {
    f32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}
fn write_u32(data: &mut [u8], offset: usize, value: u32) {
    data[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn write_u64(data: &mut [u8], offset: usize, value: u64) {
    data[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
fn write_f32(data: &mut [u8], offset: usize, value: f32) {
    data[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
