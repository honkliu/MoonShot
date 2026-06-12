/*
 * moon_inspect — MoonShot index file analyser.
 *
 * Usage:
 *   moon_inspect <index.idx> [index.idx.meta] > report.html
 *   moon_inspect <index.idx> [index.idx.meta] -o report.html
 *
 * Generates a self-contained HTML report showing:
 *   • File header (magic, version, section offsets)
 *   • SubIndex  (sparse term → block_seq table)
 *   • DocData   (per-document metadata; merged with .meta paths)
 *   • Blocks    (decoded term entries + hex view per block)
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <format>
#include <sstream>
#include <fstream>
#include <iostream>

/* ── constants that must match IndexSerializer ─────────────────────────── */
static const uint8_t  MAGIC[8]       = {'M','O','O','N','S','H','O','T'};
static const uint32_t FORMAT_VERSION = 2;
static const int      PAGE_SIZE      = 4096;
static const int      IB_DATA_OFF    = 8 + 200;          /* IB_Header + IB_Skip */
static const int      IB_DATA_LEN    = PAGE_SIZE - IB_DATA_OFF;
static const uint64_t HAS_MORE_FLAG  = (1ULL << 63);
static const uint16_t CONT_MARKER    = 0xFFFFu;

/* ── on-disk structs (packed) ──────────────────────────────────────────── */
#pragma pack(push,1)
struct FileHdr {
    uint8_t  magic[8];
    uint32_t version, reserved;
    uint64_t num_documents, num_terms;
    uint64_t subindex_off, subindex_size;
    uint64_t docdata_off,  docdata_size;
    uint64_t blocks_off,   num_blocks;
};
struct DocRec { uint64_t doc_id; float importance; uint32_t doc_len; };
#pragma pack(pop)

/* ── VarByte decoder ───────────────────────────────────────────────────── */
static uint64_t vb_decode(const uint8_t* data, size_t size, size_t& pos)
{
    uint64_t r = 0; uint32_t shift = 0; uint8_t b;
    do {
        if (pos >= size) return 0;
        b = data[pos++];
        r |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        shift += 7;
    } while (b & 0x80u);
    return r;
}

/* ── parsed data model ─────────────────────────────────────────────────── */
struct SubEntry { std::string term; uint32_t block_seq; };

struct Posting  { uint64_t doc_id; uint32_t tf; };
struct TermEntry {
    std::string         key;
    uint32_t            freq, data_len;
    bool                is_last;
    std::vector<Posting> postings;
};
struct Block {
    uint32_t             seq;
    bool                 has_more;
    bool                 is_continuation;
    std::vector<TermEntry> terms;
    std::vector<uint8_t>   raw;   /* IB_Data bytes */
};

/* ── HTML helpers ──────────────────────────────────────────────────────── */
static std::string esc(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if      (c=='<') o += "&lt;";
        else if (c=='>') o += "&gt;";
        else if (c=='&') o += "&amp;";
        else if (c=='"') o += "&quot;";
        else             o += c;
    }
    return o;
}

static std::string hex8(uint64_t v)
{
    return std::format("{:016X}", v);
}
static std::string hex4(uint32_t v)
{
    return std::format("{:08X}", v);
}

/* hex dump of bytes */
static std::string hexdump(const uint8_t* d, size_t n, size_t cols=16)
{
    std::ostringstream o;
    for (size_t i=0; i<n; i+=cols) {
        o << std::format("{:04X}  ", i);
        for (size_t j=0; j<cols; ++j) {
            if (i+j < n) o << std::format("{:02X} ", d[i+j]);
            else o << "   ";
            if (j==7) o << ' ';
        }
        o << "  ";
        for (size_t j=0; j<cols && i+j<n; ++j)
            o << (char)(d[i+j]>=32&&d[i+j]<127?d[i+j]:'.');
        o << '\n';
    }
    return o.str();
}

/* ── parse the index file ──────────────────────────────────────────────── */
struct Index {
    FileHdr hdr{};
    std::vector<SubEntry>  subindex;
    std::vector<DocRec>    docdata;
    std::vector<Block>     blocks;
    std::string            error;
    std::vector<uint8_t>   file_bytes;
};

static Index parse(const char* path)
{
    Index idx;
    FILE* f = fopen(path,"rb");
    if (!f) { idx.error = "Cannot open file"; return idx; }
    fseek(f,0,SEEK_END); long fsz = ftell(f); fseek(f,0,SEEK_SET);
    idx.file_bytes.resize(fsz);
    fread(idx.file_bytes.data(),1,fsz,f);
    fclose(f);

    const uint8_t* D = idx.file_bytes.data();
    size_t         N = idx.file_bytes.size();

    if (N < sizeof(FileHdr)) { idx.error="File too small"; return idx; }
    memcpy(&idx.hdr, D, sizeof(FileHdr));
    if (memcmp(idx.hdr.magic, MAGIC, 8) != 0) { idx.error="Bad magic"; return idx; }
    if (idx.hdr.version != FORMAT_VERSION)     { idx.error="Version mismatch"; return idx; }

    /* SubIndex */
    {
        size_t p = idx.hdr.subindex_off;
        if (p+4 <= N) {
            uint32_t n; memcpy(&n,D+p,4); p+=4;
            for (uint32_t i=0; i<n && p+2<=N; ++i) {
                uint16_t kl; memcpy(&kl,D+p,2); p+=2;
                if (p+kl+4>N) break;
                SubEntry e;
                e.term.assign(reinterpret_cast<const char*>(D+p),kl); p+=kl;
                memcpy(&e.block_seq,D+p,4); p+=4;
                idx.subindex.push_back(std::move(e));
            }
        }
    }

    /* DocData */
    {
        size_t p = idx.hdr.docdata_off;
        size_t n = idx.hdr.docdata_size / sizeof(DocRec);
        for (size_t i=0; i<n && p+sizeof(DocRec)<=N; ++i, p+=sizeof(DocRec)) {
            DocRec r; memcpy(&r,D+p,sizeof(DocRec));
            idx.docdata.push_back(r);
        }
    }

    /* Blocks */
    for (uint64_t bs=0; bs<idx.hdr.num_blocks; ++bs) {
        uint64_t off = idx.hdr.blocks_off + bs*PAGE_SIZE;
        if (off+PAGE_SIZE > N) break;
        const uint8_t* blk = D+off;

        Block b;
        b.seq = static_cast<uint32_t>(bs);
        uint64_t ibhdr; memcpy(&ibhdr,blk,8);
        b.has_more = (ibhdr & HAS_MORE_FLAG) != 0;

        const uint8_t* data = blk + IB_DATA_OFF;
        b.raw.assign(data, data+IB_DATA_LEN);

        /* Check for continuation marker */
        uint16_t first2=0; if (IB_DATA_LEN>=2) memcpy(&first2,data,2);
        b.is_continuation = (first2 == CONT_MARKER);

        if (!b.is_continuation) {
            size_t p=0;
            while (p+2 <= (size_t)IB_DATA_LEN) {
                uint16_t kl; memcpy(&kl,data+p,2); p+=2;
                if (kl==0) break;
                if (kl==CONT_MARKER) break;
                if (p+kl+8 > (size_t)IB_DATA_LEN) break;

                TermEntry te;
                te.key.assign(reinterpret_cast<const char*>(data+p),kl); p+=kl;
                memcpy(&te.freq,   data+p,   4);
                memcpy(&te.data_len,data+p+4,4); p+=8;

                size_t dend = p + te.data_len;
                if (dend > (size_t)IB_DATA_LEN) dend = IB_DATA_LEN;

                /* Decode posting list */
                uint64_t prev=0;
                size_t pp=p;
                while (pp < dend) {
                    uint64_t delta = vb_decode(data,dend,pp);
                    if (pp >= dend) break;
                    uint64_t tf    = vb_decode(data,dend,pp);
                    prev += delta;
                    te.postings.push_back({prev, static_cast<uint32_t>(tf)});
                }

                /* Detect if last entry */
                uint16_t next_kl=0;
                if (p+te.data_len+2 <= (size_t)IB_DATA_LEN)
                    memcpy(&next_kl, data+p+te.data_len, 2);
                te.is_last = (next_kl==0 || p+te.data_len+2>(size_t)IB_DATA_LEN);

                p += te.data_len;
                b.terms.push_back(std::move(te));
            }
        }
        idx.blocks.push_back(std::move(b));
    }
    return idx;
}

/* ── load .meta (doc_id → filepath) ────────────────────────────────────── */
static std::unordered_map<uint64_t,std::string> load_meta(const char* path)
{
    std::unordered_map<uint64_t,std::string> m;
    FILE* f = fopen(path,"rb");
    if (!f) return m;
    uint64_t n=0; fread(&n,8,1,f);
    for (uint64_t i=0; i<n; ++i) {
        uint64_t id=0; uint32_t len=0;
        fread(&id,8,1,f); fread(&len,4,1,f);
        std::string p(len,'\0'); fread(&p[0],1,len,f);
        m[id]=p;
    }
    fclose(f);
    return m;
}

/* ── HTML generation ───────────────────────────────────────────────────── */
static std::string css()
{
    return R"css(
* { box-sizing:border-box; margin:0; padding:0; }
body { font-family:'Segoe UI',sans-serif; background:#1e1e1e; color:#d4d4d4; font-size:13px; }
h1 { padding:10px 16px; background:#252526; color:#cccccc; font-size:16px; border-bottom:1px solid #3e3e42; }
.toolbar { display:flex; gap:8px; padding:6px 16px; background:#2d2d30; border-bottom:1px solid #3e3e42; }
.tab { padding:4px 12px; border:1px solid #555; border-radius:3px; cursor:pointer; background:#3c3c3c; color:#ccc; }
.tab.active { background:#007acc; border-color:#007acc; color:#fff; }
.layout { display:flex; height:calc(100vh - 74px); overflow:hidden; }
.left { width:420px; min-width:200px; overflow-y:auto; border-right:1px solid #3e3e42; }
.right { flex:1; overflow-y:auto; padding:12px; }
.panel { display:none; }
.panel.active { display:block; }
.section-hdr { padding:6px 10px; background:#2d2d30; font-weight:600; color:#9cdcfe; font-size:12px;
                position:sticky; top:0; z-index:1; border-bottom:1px solid #3e3e42; }
table { width:100%; border-collapse:collapse; }
th { background:#252526; padding:4px 8px; text-align:left; color:#9cdcfe; font-weight:600;
     position:sticky; top:0; z-index:1; border-bottom:1px solid #3e3e42; }
td { padding:3px 8px; border-bottom:1px solid #2a2a2a; vertical-align:top; }
tr:hover td { background:#2a2d2e; }
tr.selected td { background:#094771; }
.mono { font-family:'Cascadia Code','Consolas',monospace; font-size:12px; }
.badge { display:inline-block; padding:1px 5px; border-radius:3px; font-size:11px; margin-left:4px; }
.badge-more { background:#ca5010; color:#fff; }
.badge-cont { background:#5c8a3a; color:#fff; }
.badge-last { background:#555; color:#bbb; }
.hexview { font-family:'Cascadia Code','Consolas',monospace; font-size:11px; white-space:pre;
           background:#1a1a1a; padding:8px; border-radius:4px; overflow-x:auto; color:#ce9178; }
.block-card { background:#252526; border:1px solid #3e3e42; border-radius:4px; margin-bottom:12px; }
.block-card-hdr { padding:6px 10px; background:#2d2d30; border-bottom:1px solid #3e3e42;
                   font-weight:600; display:flex; align-items:center; gap:6px; cursor:pointer; }
.block-card-body { padding:8px; }
.posting { color:#4ec9b0; font-size:11px; margin-left:12px; }
.path { color:#ce9178; font-size:11px; }
.num { color:#b5cea8; }
.keyword { color:#569cd6; }
.term-row { display:flex; align-items:baseline; gap:8px; padding:2px 0;
             border-bottom:1px solid #2a2a2a; }
.term-key { color:#9cdcfe; font-family:monospace; min-width:0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; max-width:220px; }
.info-grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(220px,1fr)); gap:8px; padding:8px; }
.info-card { background:#2d2d30; border:1px solid #3e3e42; border-radius:4px; padding:8px; }
.info-card dt { color:#9cdcfe; font-size:11px; margin-bottom:2px; }
.info-card dd { color:#d4d4d4; font-family:monospace; font-size:12px; }
.search-box { padding:4px 8px; background:#3c3c3c; border:1px solid #555; color:#d4d4d4;
               border-radius:3px; width:200px; outline:none; }
)css";
}

static void emit(std::ostream& o, const Index& idx,
                 const std::unordered_map<uint64_t,std::string>& meta,
                 const char* idx_path)
{
    auto& H = idx.hdr;

    o << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      << "<title>moon_inspect — " << esc(idx_path) << "</title>"
      << "<style>" << css() << "</style></head><body>\n";

    o << "<h1>&#128269; moon_inspect &nbsp;<span style='color:#888;font-weight:normal'>" << esc(idx_path) << "</span></h1>\n";

    o << "<div class='toolbar'>"
      << "<button class='tab active' onclick=\"switchTab('header')\">Header</button>"
      << "<button class='tab' onclick=\"switchTab('subindex')\">SubIndex <span class='num'>("
      << idx.subindex.size() << ")</span></button>"
      << "<button class='tab' onclick=\"switchTab('docdata')\">DocData <span class='num'>("
      << idx.docdata.size() << ")</span></button>"
      << "<button class='tab' onclick=\"switchTab('blocks')\">Blocks <span class='num'>("
      << idx.blocks.size() << ")</span></button>"
      << "<input class='search-box' id='srch' placeholder='Filter terms…' oninput='filterTerms(this.value)'>"
      << "</div>\n";

    o << "<div class='layout'><div class='left' id='leftpane'>\n";

    /* ── left: SubIndex list ───────────────────────────────────────────── */
    o << "<div class='section-hdr'>SubIndex (" << idx.subindex.size() << " entries)</div>";
    o << "<table><tr><th>#</th><th>Block</th><th>Lead term</th></tr>\n";
    for (size_t i=0; i<idx.subindex.size(); ++i) {
        auto& e = idx.subindex[i];
        o << "<tr onclick=\"showBlock(" << e.block_seq << ")\" style='cursor:pointer'>"
          << "<td class='num'>" << i << "</td>"
          << "<td class='num'>" << e.block_seq << "</td>"
          << "<td class='mono'>" << esc(e.term) << "</td></tr>\n";
    }
    o << "</table>\n";

    o << "</div><div class='right'>\n";

    /* ── right: Header panel ──────────────────────────────────────────── */
    o << "<div class='panel active' id='panel-header'>\n";
    o << "<div class='info-grid'>";
    auto card = [&](const char* label, const std::string& val) {
        o << "<div class='info-card'><dt>" << label << "</dt><dd>" << val << "</dd></div>";
    };
    char mbuf[32]; snprintf(mbuf,sizeof(mbuf),"%.8s",H.magic);
    card("Magic",    std::string(mbuf,8));
    card("Version",  std::to_string(H.version));
    card("Documents",std::to_string(H.num_documents));
    card("Terms",    std::to_string(H.num_terms));
    card("SubIndex off", "0x"+hex8(H.subindex_off)+" ("+std::to_string(H.subindex_size)+" B)");
    card("DocData off",  "0x"+hex8(H.docdata_off) +" ("+std::to_string(H.docdata_size)+" B)");
    card("Blocks off",   "0x"+hex8(H.blocks_off));
    card("Blocks count", std::to_string(H.num_blocks));
    card("File size",    std::to_string(idx.file_bytes.size())+" B");
    o << "</div></div>\n";

    /* ── right: DocData panel ─────────────────────────────────────────── */
    o << "<div class='panel' id='panel-docdata'>\n";
    o << "<table><tr><th>doc_id</th><th>importance</th><th>doc_len</th><th>path</th></tr>\n";
    for (auto& r : idx.docdata) {
        auto it = meta.find(r.doc_id);
        std::string path = it!=meta.end() ? esc(it->second) : "<span style='color:#888'>unknown</span>";
        char imp[16]; snprintf(imp,sizeof(imp),"%.3f",r.importance);
        o << "<tr><td class='mono num'>" << hex8(r.doc_id) << "</td>"
          << "<td class='num'>" << imp << "</td>"
          << "<td class='num'>" << r.doc_len << "</td>"
          << "<td class='path'>" << path << "</td></tr>\n";
    }
    o << "</table></div>\n";

    /* ── right: SubIndex panel ────────────────────────────────────────── */
    o << "<div class='panel' id='panel-subindex'>\n";
    o << "<table><tr><th>Index</th><th>Block</th><th>Lead term</th></tr>\n";
    for (size_t i=0; i<idx.subindex.size(); ++i) {
        auto& e = idx.subindex[i];
        o << "<tr onclick=\"showBlock(" << e.block_seq << ")\" style='cursor:pointer'>"
          << "<td class='num'>" << i << "</td>"
          << "<td class='num'>" << e.block_seq << "</td>"
          << "<td class='mono'>" << esc(e.term) << "</td></tr>\n";
    }
    o << "</table></div>\n";

    /* ── right: Blocks panel ──────────────────────────────────────────── */
    o << "<div class='panel' id='panel-blocks'>\n";
    for (auto& blk : idx.blocks) {
        o << "<div class='block-card' id='blk" << blk.seq << "'>";
        o << "<div class='block-card-hdr' onclick=\"toggleBlock(" << blk.seq << ")\">";
        o << "<span class='keyword'>Block</span> <span class='num'>" << blk.seq << "</span>";
        if (blk.is_continuation) o << "<span class='badge badge-cont'>continuation</span>";
        if (blk.has_more)        o << "<span class='badge badge-more'>HAS_MORE</span>";
        if (!blk.terms.empty())  o << "<span style='color:#888;font-size:11px'> &nbsp;" << blk.terms.size() << " terms</span>";
        o << "</div>";
        o << "<div class='block-card-body' id='blkbody" << blk.seq << "' style='display:none'>";

        if (blk.is_continuation) {
            o << "<div style='color:#888;padding:4px'>Continuation block — raw bytes follow:</div>";
            o << "<div class='hexview'>" << esc(hexdump(blk.raw.data(),std::min(blk.raw.size(),(size_t)128))) << "</div>";
        } else {
            /* term table */
            if (!blk.terms.empty()) {
                o << "<table><tr><th>Term</th><th>freq</th><th>data_len</th><th>Postings</th></tr>\n";
                for (auto& te : blk.terms) {
                    o << "<tr class='term-entry' data-term='" << esc(te.key) << "'>"
                      << "<td class='mono term-key'>" << esc(te.key);
                    if (te.is_last && blk.has_more) o << "<span class='badge badge-more'>→cont</span>";
                    o << "</td><td class='num'>" << te.freq
                      << "</td><td class='num'>" << te.data_len << "</td><td>";
                    for (auto& p : te.postings) {
                        auto it = meta.find(p.doc_id);
                        std::string ppath = it!=meta.end() ? it->second : hex8(p.doc_id);
                        o << "<div class='posting'>id=" << hex8(p.doc_id) << " tf=" << p.tf;
                        if (it!=meta.end()) o << " <span class='path'>" << esc(ppath) << "</span>";
                        o << "</div>";
                    }
                    o << "</td></tr>\n";
                }
                o << "</table>\n";
            }
            /* hex dump */
            o << "<details><summary style='cursor:pointer;color:#888;padding:4px'>Hex dump (IB_Data)</summary>"
              << "<div class='hexview'>" << esc(hexdump(blk.raw.data(),blk.raw.size())) << "</div></details>";
        }
        o << "</div></div>\n";
    }
    o << "</div>\n"; /* panel-blocks */

    o << "</div></div>\n"; /* right + layout */

    /* ── JavaScript ───────────────────────────────────────────────────── */
    o << R"js(<script>
function switchTab(name) {
    document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
    var p = document.getElementById('panel-'+name);
    if (p) p.classList.add('active');
    event.target.classList.add('active');
}
function showBlock(seq) {
    switchTabByName('blocks');
    var el = document.getElementById('blk'+seq);
    if (!el) return;
    el.scrollIntoView({behavior:'smooth',block:'start'});
    var body = document.getElementById('blkbody'+seq);
    if (body) body.style.display='block';
    document.querySelectorAll('.block-card').forEach(b=>b.style.outline='');
    el.style.outline='2px solid #007acc';
}
function switchTabByName(name) {
    document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
    var p = document.getElementById('panel-'+name);
    if (p) p.classList.add('active');
    var tabs = document.querySelectorAll('.tab');
    tabs.forEach(t=>{ if (t.textContent.trim().toLowerCase().startsWith(name)) t.classList.add('active'); });
}
function toggleBlock(seq) {
    var body = document.getElementById('blkbody'+seq);
    if (body) body.style.display = body.style.display==='none'?'block':'none';
}
function filterTerms(q) {
    q = q.toLowerCase();
    document.querySelectorAll('.term-entry').forEach(function(row) {
        var term = (row.getAttribute('data-term')||'').toLowerCase();
        row.style.display = (!q || term.includes(q)) ? '' : 'none';
    });
    if (q) switchTabByName('blocks');
}
// Expand block when clicked from SubIndex
document.querySelectorAll('#panel-subindex tr[onclick]').forEach(function(r){
    r.addEventListener('click', function(){
        var onclick = r.getAttribute('onclick');
        var m = onclick.match(/showBlock\((\d+)\)/);
        if (m) showBlock(parseInt(m[1]));
    });
});
</script>)js";

    o << "</body></html>\n";
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: moon_inspect <index.idx> [index.idx.meta] [-o out.html]\n");
        return 1;
    }

    const char* idx_path  = argv[1];
    const char* meta_path = nullptr;
    const char* out_path  = nullptr;

    for (int i=2; i<argc; ++i) {
        if (strcmp(argv[i],"-o")==0 && i+1<argc) { out_path = argv[++i]; }
        else meta_path = argv[i];
    }

    Index idx = parse(idx_path);
    if (!idx.error.empty()) {
        fprintf(stderr, "Error: %s\n", idx.error.c_str());
        return 1;
    }

    std::unordered_map<uint64_t,std::string> meta;
    if (meta_path)            meta = load_meta(meta_path);

    /* try auto-loading .meta if not specified */
    if (meta.empty()) {
        std::string auto_meta = std::string(idx_path) + ".meta";
        meta = load_meta(auto_meta.c_str());
    }

    if (out_path) {
        std::ofstream f(out_path);
        emit(f, idx, meta, idx_path);
        fprintf(stderr, "Written to %s\n", out_path);
    } else {
        emit(std::cout, idx, meta, idx_path);
    }
    return 0;
}
