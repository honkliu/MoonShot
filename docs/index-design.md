# MoonShot Index Design — Memory & Disk Organization

> **Status**: Design document — drives IndexAccess, BlockTable, and
> FileBlockManager implementation.  
> **See also**: `query-syntax.md` for the query layer that consumes this index.

---

## 1. Document Model

### 1.1 DocumentID

Every document receives a single `u64` DocumentID assigned at **index-build
time**.  IDs are allocated in **ascending order of document importance** so that
lower DocID always means higher quality.

```
DocID 0  →  most important document in the corpus
DocID 1  →  second most important
  …
DocID N-1 →  least important
```

Importance is computed offline (PageRank, domain authority, or a learned model)
before the index is built.  This ordering is the single most impactful
structural choice: DAAT (document-at-a-time) traversal naturally surfaces the
best results first, enabling early exit.

### 1.2 Fields and MetaStreams

A document is split into named **MetaStreams** at ingestion time:

```
Document
├── Anchor  (A)  — inbound link anchor text
├── URL     (U)  — tokens from the document URL / file path
├── Title   (T)  — document title
├── Body    (B)  — main content
└── Meta    (M)  — meta-description, keywords (optional)
```

Each term × stream combination produces a separate posting-list key:

```
Term "bad" in Title  →  key "badT"
Term "bad" in Body   →  key "badB"
Term "bad" in Anchor →  key "badA"
```

This makes stream-specific ranking (AUT vs AUTB phases) a simple matter of
opening a different set of ISRs.

---

## 2. Posting List Format

### 2.1 Logical Layout

For a given key (e.g. `"badB"`), the posting list is a sorted array of
(DocID, TermFrequency) pairs:

```
badB:  (1, 24),  (3, 123),  (9, 32),  (17, 7),  …
        ↑   ↑
      DocID  TF
```

DocIDs are sorted ascending.  Because DocIDs encode importance, traversal
from the front automatically yields the highest-quality documents.

### 2.2 Wire Encoding — VarByte DocID + Fixed TF

Storing raw 8-byte DocIDs is wasteful.  We store:

- **DocID**: the absolute document ID.
- **TF**: raw unsigned integer.

DocIDs are encoded with **Variable-Byte (VBC)** encoding, and TF is stored as a scale16 8-bit integer:

```
DocID is stored in 1–9 bytes.
Low 7 bits of each byte carry payload.
High bit = 1 means "more bytes follow"; 0 means "last byte".
TF is stored in exactly 1 byte as `min(255, round(16 * log2(1 + tf)))`.
At query time, this encoded byte is used directly as the BM25 term-frequency signal.
```

Example for the entry `(DocID=1, TF=24)`:

```
DocID      = 1:    encoded as  0x01          (1 byte)
TF         = 24:   encoded as  0x4A          (1 byte, scale16)
```

Example for the next entry `(DocID=3, TF=123)`:

```
DocID       = 3:     encoded as  0x03
TF          = 123:   encoded as  0x6F
```

Full posting `badB: (1,24),(3,123),(9,32)` encodes to:

```
01 4A 03 6F 09 51
│  │  │  │  │  └─ TF=32 scale16
│  │  │  │  └──── DocID=9
│  │  │  └─────── TF=123 scale16
│  │  └────────── DocID=3
│  └───────────── TF=24 scale16
└──────────────── DocID=1
```

Average compression depends on DocID size: TF is always 1 byte, so common small-docID pairs are 2–4 bytes vs 12 bytes raw.

### 2.3 Position Data (future)

For phrase queries, we need the within-document position of each term
occurrence.  Position lists are stored **after** the fixed TF in a VBC stream:

```
(DocID=1, TF=3, positions=[4, 17, 42])

Encoding:  DocID  TF  delta(pos[0])  delta(pos[1])  delta(pos[2])
```

Position data roughly doubles posting-list size.  It is stored in a separate
section of the block (same block, different offset) so non-phrase queries skip
it with zero overhead.

---

## 3. DocData — Fixed-Size Per-Document Record

Each document has a fixed-size **DocData record** stored contiguously on disk.
Given DocID `d`, its record starts at byte offset `d × DOCDATA_SIZE`.

### 3.1 Layout (proposed 64 bytes)

```
Offset  Size  Field
──────  ────  ──────────────────────────────────────────
 0       8    DocID              (redundant, sanity check)
 8       4    ImportanceScore    (f32 — PageRank or equivalent)
12       4    DocumentLength     (total token count across all streams)
16       8    URLHash            (64-bit Fowler–Noll–Vo hash of the URL)
24       4    Language           (ISO 639 code packed as u32)
28       4    DocumentType       (MIME type code)
32       4    LastModified       (Unix timestamp, seconds)
36       4    BodyLength         (token count in Body stream only)
40       4    TitleLength        (token count in Title stream only)
44       4    Flags              (bit field: is_adult, is_spam, …)
48      16    Reserved           (future use — zero-filled)
──────  ────
Total  64 bytes
```

`ImportanceScore` is the single pre-computed document quality signal.
It is added directly to the BM25 score at rank time (see query-syntax.md §5).

DocData is accessed as a flat array: `seek(DocID × 64)` + `read(64 bytes)`.
For hot documents (low DocID) this data is kept in the OS page cache.

---

## 4. On-Disk Index File Format

The index is stored in a **single binary file** with five contiguous sections.

```
┌──────────────────────────────────┐  offset 0
│  Section 0: File Header          │  fixed 88 bytes
├──────────────────────────────────┤
│  Section 1: HeadTermEntry        │  fixed 32-byte records
├──────────────────────────────────┤
│  Section 2: LeafTermBlock        │  fixed 4 KB blocks
├──────────────────────────────────┤
│  Section 3: DocData              │  fixed 1024-byte records
├──────────────────────────────────┤
│  Section 4: IndexBlock           │  fixed 4 KB posting blocks
└──────────────────────────────────┘
```

### 4.1 File Header (Section 0 — 88 bytes)

```
Offset  Size  Field
──────  ────  ──────────────────────────────────
 0       8    Magic number  ("MOONSHOT")
 8       4    Version
12       4    AvgDocLength
16       8    NumDocuments
24       8    NumTerms
32       8    HeadTermEntryOffset
40       8    HeadTermEntryCount
48       8    LeafTermBlockOffset
56       8    LeafTermBlockCount
64       8    DocDataOffset
72       8    IndexBlockOffset
80       8    IndexBlockCount
```

### 4.2 HeadTermEntry and LeafTermBlock

The level-1 term directory is a sorted fixed array of 32-byte `HeadTermEntry`
records.  Each record stores the full first stream key for one `LeafTermBlock`
and that block's ID.  The maximum stream-key length in a head entry is 26 bytes;
longer stream keys are not written to the index because lookup is a pure binary
search over the sorted head array.

```
HeadTermEntry:
    u32  leaf_page_id
    u16  first_key_len
    char first_key[26]
```

Each `LeafTermBlock` is a 4096-byte block containing sorted variable-length
`LeafTermEntry` records for exact term lookup inside the selected block.

### 4.3 Posting Blocks (Section 4)

Posting data is stored in **4 096-byte (4 KB) fixed-size blocks**.  4 KB
matches the OS virtual-memory page size, so each disk read aligns perfectly
with one page fault.

```
Block structure:
┌──────────────────────┐  offset 0
│  BlockHeader (32 B)  │
├──────────────────────┤
│  PostingData (≤ 4064 B) │   one or more VBC-encoded posting lists
└──────────────────────┘
```

**BlockHeader layout (32 bytes):**

```
 0   4   BlockID           (u32)
 4   4   NumPostings       (u32 — how many posting lists start in this block)
 8   4   DataByteLen       (u32 — bytes used in PostingData)
12   4   Checksum          (CRC32 of PostingData)
16  16   Reserved
```

A posting list that is **longer than one block** spans consecutive blocks.
The TermDictionary entry points to the *first* block; the reader follows
`block_id + 1, block_id + 2, …` until `posting_byte_len` bytes have been
consumed.

---

## 5. In-Memory Structures

### 5.1 TermDictionary (HashMap)

```
HashMap<String, DictEntry>

DictEntry {
    block_id:         u32,
    intra_block_off:  u16,
    posting_byte_len: u32,
    doc_freq:         u32,
}
```

Loaded once at index-open time.  For 10 M distinct term-stream keys at ~60
bytes per entry, this is ~600 MB.  For local-scale indexes (< 1 M keys) it is
~60 MB.

### 5.2 BlockTable — The In-Memory Block Cache

The BlockTable is the **primary interface between the query path and disk I/O**.
It caches recently accessed 4 KB blocks using a **Clock replacement policy**
(a lightweight, lock-friendly approximation of LRU; see REF `ClockPolicy`).

```
BlockTable {
    frames:   [BlockFrame; CAPACITY],   // fixed-size frame array
    index:    HashMap<u32, usize>,      // block_id → frame_index
    hand:     usize,                    // clock hand
    file:     FileBlockManager,         // disk I/O layer
}

BlockFrame {
    block_id: u32,
    data:     [u8; 4096],
    state:    FrameState,   // Available | Touched | Pinned
    ref_count: u32,
}
```

**Cache lookup path:**

```
BlockTable::get(block_id)
  1. index.get(block_id)  → hit: mark Touched, return &data
  2. miss: find victim frame (Clock sweep)
  3. file.read_block(block_id) → load 4 KB into victim frame
  4. index.insert(block_id, victim_frame)
  5. return &data
```

**Sizing guidance:**

| Corpus size   | Suggested CAPACITY | Memory used |
|---------------|-------------------|-------------|
| < 1 M docs    | 512 frames        | 2 MB        |
| 1 M – 10 M    | 4 096 frames      | 16 MB       |
| 10 M – 100 M  | 32 768 frames     | 128 MB      |

### 5.3 ISR Pool — Per-Query Posting Cache

Within a single query execution, many nodes of the EvalTree may reference the
**same** posting list (e.g., "rust" appears in both AUT and AUTB phases).
The ISR pool avoids re-reading the same block for the same term:

```
IsrPool {
    cache: HashMap<String, Arc<Vec<PostingEntry>>>,
}
```

Lifecycle: created at the start of a query, dropped (or `clear()`d) at the end.
The `Arc` allows multiple ISRs to share decoded posting data with zero copy.

### 5.4 DocData Buffer

For hot documents (small DocID), the DocData array is accessed via OS page
cache.  No explicit in-process buffer is required.  The pattern `seek +
read(64)` for a given DocID is a single, well-predicted sequential access.

---

## 6. Read Path — End to End

```
Query string
    │
    ▼  QueryParser
EvalTree (AndNode, OrNode, TermNodes, …)
    │
    ▼  For each TermNode  e.g. "badT"
TermDictionary.get("badT")
    → DictEntry { block_id=42, offset=1280, posting_len=312, doc_freq=1823 }
    │
    ▼  BlockTable.get(42)
Block [42] loaded into frame (or hit in cache)
    │
    ▼  InvertedIsr
Decode VBC stream from byte 1280 + 32 (header)
   → PostingEntry stream: (1,24), (3,123), (9,32), …
    │
    ▼  DAAT traversal (AndIsr / OrIsr)
Matching DocIDs
    │
    ▼  DocData.read(DocID × 64)
ImportanceScore + DocumentLength
    │
    ▼  AdvancedIndexReader::GetScore(DocDataEntry*)
BM25(tf, DocDataEntry.DDE_DocLength, doc_freq) + StaticRank
Final score per doc
    │
    ▼  Top-K heap
Ranked results
```

---

## 7. Write Path — Index Construction

```
Raw documents (text + metadata)
    │
    ▼  Tokenizer (per MetaStream)
(term, stream, DocID, TF) tuples
    │
    ▼  Sort by (term+stream, DocID)
Sorted postings
    │
    ▼  PostingListBuilder
VBC-encoded byte streams per key
    │
    ▼  BlockAllocator
Assign posting lists to 4 KB blocks
(large lists span multiple consecutive blocks)
    │
    ▼  TermDictionary writer
(term_key, block_id, offset, len, df) entries, sorted by key
    │
    ▼  Importance Scorer
Sort DocIDs by importance, write DocData array
    │
    ▼  File writer (in section order)
Header → DocData → TermDictionary → PostingBlocks
    │
    ▼  Index file on disk
```

---

## 8. Key Design Properties

| Property               | Mechanism                                              |
|------------------------|--------------------------------------------------------|
| Importance-first ordering | DocIDs assigned in PageRank order → early exit   |
| Space efficiency       | VBC delta encoding, 2–4 bytes per posting entry        |
| Cache locality         | 4 KB blocks align with OS pages; Clock cache           |
| Query-level reuse      | ISR pool shares Arc<Vec<PostingEntry>> within one query |
| Stream separation      | Separate posting keys per stream → flexible phasing   |
| Fixed-cost doc lookup  | DocData = direct array index, no B-tree needed         |
| Single-file layout     | All sections in one file; offset in header             |

---

## 9. Future Work

| Item                     | Notes                                                      |
|--------------------------|------------------------------------------------------------|
| Skip lists               | Store skip pointers every K entries to accelerate `seek()` |
| Positional postings      | Store within-doc positions for phrase queries              |
| Tiered index (hot/cold)  | SSD for recent docs, HDD for tail; clock cache spans both  |
| Runtime update           | Append-only delta index + periodic merge (like Lucene)     |
| Compression              | PForDelta or Roaring Bitmaps for dense posting lists       |
| SIMD decode              | Vectorised VBC decode for throughput on hot terms          |
| Vector co-hosting        | HNSW graph stored in same file, separate section           |
