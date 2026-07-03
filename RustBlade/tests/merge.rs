use rustblade::executor::IndexSearchExecutor;
use rustblade::index_writer::IndexWriter;
use rustblade::block_table::INDEX_FILE_HEADER_SIZE;
use rustblade::posting_store::PostingStore;
use rustblade::serializer::{IndexFileHeader, IndexSerializer};
use rustblade::tokenizer::Tokenizer;
use rustblade::vector_index::build_hashed_embedding;
use rustblade::{Document, IndexContext, SmartTokenizer};
use std::fs::File;
use std::io::Read;

fn delta_index_path(path: &std::path::Path) -> String {
    let text = path.to_string_lossy();
    if let Some(dot) = text.rfind('.') {
        if text.rfind(['/', '\\']).map(|slash| dot > slash).unwrap_or(true) {
            return format!("{}{}.{}", &text[..dot], ".delta", &text[dot + 1..]);
        }
    }
    format!("{text}.delta.idx")
}

fn add_doc(ctx: &mut IndexContext, doc_id: u64, title: &str, body: &str, path: &str) {
    let tokenizer = SmartTokenizer::new();
    let title_tokens = tokenizer.Tokenize(title);
    let body_tokens = tokenizer.Tokenize(body);
    let mut vector_tokens = title_tokens.clone();
    vector_tokens.extend(body_tokens.iter().cloned());

    let mut writer = ctx.GetWriter();
    writer.Write(title_tokens, doc_id, "Title");
    writer.Write(body_tokens, doc_id, "Body");
    writer.SetDocImportance(doc_id, 0.1);
    writer.SetDocVector(doc_id, build_hashed_embedding(&vector_tokens));
    writer.SetDocPath(doc_id, path.to_string());
}

fn add_doc_via_context(ctx: &mut IndexContext, doc_id: u64, title: &str, body: &str, path: &str) {
    let doc = Document {
        doc_id,
        title: title.to_string(),
        body: body.to_string(),
        path: path.to_string(),
        importance: 0.1,
        ..Document::default()
    };
    ctx.AddDocument(&doc, true);
}

fn search_doc_ids(ctx: &mut IndexContext, query: &str) -> Vec<u64> {
    let mut reader = ctx.GetReaderForQuery(query, "AUTB");
    let store = ctx.GetStore();
    let store = store.lock().unwrap();
    let executor = IndexSearchExecutor::new(&store);
    executor.Execute(reader.as_mut(), 0).into_iter().map(|result| result.doc_id).collect()
}

fn read_header(path: &std::path::Path) -> IndexFileHeader {
    let mut file = File::open(path).unwrap();
    let mut bytes = [0u8; INDEX_FILE_HEADER_SIZE];
    file.read_exact(&mut bytes).unwrap();
    IndexFileHeader::parse(&bytes).unwrap()
}

#[test]
fn build_blocks_does_not_emit_term_mphf_for_v19_indexes() {
    let mut store = PostingStore::new();
    let mut terms = vec!["t28".to_string(), "t66".to_string()];
    let mut suffix = 0;
    while terms.len() < 256 {
        terms.push(format!("x{suffix}"));
        suffix += 1;
    }

    for (doc_id, term) in terms.iter().enumerate() {
        store.AddPosting(term, doc_id as u64, 1);
    }

    let built = IndexSerializer::BuildBlocks(&store);
    assert_eq!(built.BBR_TotalTerms, terms.len() as u64);
    assert_eq!(built.BBR_TermMphfHeader.TMH_Magic, 0);
    assert!(built.BBR_TermMphfDisplacements.is_empty());
    assert!(built.BBR_TermMphfEntryPages.is_empty());
}

#[test]
fn merge_base_and_delta_indexes() {
    let temp = tempfile::tempdir().unwrap();
    let base_path = temp.path().join("merge.idx");
    let delta_path = delta_index_path(&base_path);
    let base_path_text = base_path.to_string_lossy().to_string();

    let mut base = IndexContext::new();
    add_doc(&mut base, 0, "alpha", "shared base", "base.txt");
    base.SaveIndex(&base_path_text).unwrap();

    let mut delta = IndexContext::new();
    add_doc(&mut delta, 1, "beta", "shared delta", "delta.txt");
    delta.SaveIndex(&delta_path).unwrap();

    let mut merge = IndexContext::with_path(Some(base_path_text.clone()));
    merge.Merge(&base_path_text).unwrap();

    let mut merged = IndexContext::with_path(Some(base_path_text));
    assert_eq!(merged.DocumentCount(), 2);
    assert_eq!(merged.GetDocPath(0), "base.txt");
    assert_eq!(merged.GetDocPath(1), "delta.txt");

    assert!(search_doc_ids(&mut merged, "alpha").contains(&0));
    assert!(search_doc_ids(&mut merged, "beta").contains(&1));
    let shared = search_doc_ids(&mut merged, "shared");
    assert!(shared.contains(&0));
    assert!(shared.contains(&1));
}

#[test]
fn add_document_indexes_all_default_streams() {
    let temp = tempfile::tempdir().unwrap();
    let index_path = temp.path().join("adddoc.idx");
    let index_path_text = index_path.to_string_lossy().to_string();

    let mut ctx = IndexContext::new();
    let doc = Document {
        doc_id: 0,
        path: "doc.txt".to_string(),
        url: "urlword".to_string(),
        title: "titleword".to_string(),
        body: "bodyword".to_string(),
        anchor: "anchorword".to_string(),
        meta: "metaword".to_string(),
        importance: 0.5,
    };
    ctx.AddDocument(&doc, true);
    ctx.SaveIndex(&index_path_text).unwrap();

    let mut loaded = IndexContext::with_path(Some(index_path_text));
    assert!(search_doc_ids(&mut loaded, "titleword").contains(&0));
    assert!(search_doc_ids(&mut loaded, "bodyword").contains(&0));
    assert!(search_doc_ids(&mut loaded, "anchorword").contains(&0));
    assert!(search_doc_ids(&mut loaded, "urlword").contains(&0));
}

#[test]
fn load_from_bytes_supports_text_search_and_paths() {
    let temp = tempfile::tempdir().unwrap();
    let index_path = temp.path().join("bytes.idx");
    let index_path_text = index_path.to_string_lossy().to_string();

    let mut ctx = IndexContext::new();
    add_doc_via_context(&mut ctx, 0, "alpha", "beta gamma", "dir/alpha.md");
    ctx.SaveIndex(&index_path_text).unwrap();

    let bytes = std::fs::read(&index_path).unwrap();
    let mut loaded = IndexContext::new();
    loaded.LoadFromBytes(&bytes).unwrap();
    assert_eq!(loaded.GetDocPath(0), "dir/alpha.md");
    assert!(search_doc_ids(&mut loaded, "alpha").contains(&0));
}

#[test]
fn title_score_uses_title_length_instead_of_body_length() {
    let mut ctx = IndexContext::new();
    let short = Document {
        doc_id: 0,
        title: "needle".to_string(),
        body: "short".to_string(),
        importance: 0.0,
        ..Document::default()
    };
    let long = Document {
        doc_id: 1,
        title: "needle".to_string(),
        body: std::iter::repeat("filler").take(200).collect::<Vec<_>>().join(" "),
        importance: 0.0,
        ..Document::default()
    };
    ctx.AddDocument(&short, true);
    ctx.AddDocument(&long, true);
    ctx.Build();

    let token = ctx.GetTokenizer().Tokenize("needle").into_iter().next().unwrap();
    let stream_key = format!("{token}T");
    let mut reader = ctx.GetStreamReader(&stream_key);
    let store = ctx.GetStore();
    let store = store.lock().unwrap();
    let executor = IndexSearchExecutor::new(&store);
    let results = executor.Execute(reader.as_mut(), 0);
    let short_score = results.iter().find(|result| result.doc_id == 0).unwrap().score;
    let long_score = results.iter().find(|result| result.doc_id == 1).unwrap().score;
    assert!((short_score - long_score).abs() < 1e-6, "short_score={short_score}, long_score={long_score}");
}

#[test]
fn load_index_discovers_delta_context() {
    let temp = tempfile::tempdir().unwrap();
    let base_path = temp.path().join("delta-load.idx");
    let delta_path = delta_index_path(&base_path);
    let base_path_text = base_path.to_string_lossy().to_string();

    let mut base = IndexContext::new();
    add_doc_via_context(&mut base, 0, "base", "base body", "base.txt");
    base.SaveIndex(&base_path_text).unwrap();

    let mut delta = IndexContext::new();
    add_doc_via_context(&mut delta, 1, "delta", "delta body", "delta.txt");
    delta.SaveIndex(&delta_path).unwrap();

    let mut loaded = IndexContext::with_path(Some(base_path_text));
    assert!(loaded.HasDelta());
    assert!(search_doc_ids(&mut loaded, "delta").contains(&1));
    assert_eq!(loaded.GetDocPath(1), "delta.txt");
}

#[test]
fn sparse_standalone_index_uses_compact_docdata_first_doc_id() {
    let temp = tempfile::tempdir().unwrap();
    let index_path = temp.path().join("sparse.idx");
    let index_path_text = index_path.to_string_lossy().to_string();

    let mut ctx = IndexContext::new();
    add_doc(&mut ctx, 3, "gamma", "three four five", "delta3.txt");
    ctx.SaveIndex(&index_path_text).unwrap();

    let header = read_header(&index_path);
    assert_eq!(header.IFH_NumDocuments, 1);
    assert_eq!(header.IFH_AvgDocLength, 4.0);

    let mut loaded = IndexContext::with_path(Some(index_path_text));
    assert_eq!(loaded.DocumentCount(), 1);
    assert_eq!(loaded.GetDocPath(3), "delta3.txt");
    assert!(search_doc_ids(&mut loaded, "gamma").contains(&3));
}

#[test]
fn merge_base_and_contiguous_delta_preserves_header_average() {
    let temp = tempfile::tempdir().unwrap();
    let base_path = temp.path().join("contiguous.idx");
    let delta_path = delta_index_path(&base_path);
    let base_path_text = base_path.to_string_lossy().to_string();

    let mut base = IndexContext::new();
    add_doc(&mut base, 0, "alpha", "one two", "base.txt");
    base.SaveIndex(&base_path_text).unwrap();

    let mut delta = IndexContext::new();
    add_doc(&mut delta, 1, "gamma", "three four five", "delta1.txt");
    delta.SaveIndex(&delta_path).unwrap();

    let mut merge = IndexContext::with_path(Some(base_path_text.clone()));
    merge.Merge(&base_path_text).unwrap();

    let header = read_header(&base_path);
    assert_eq!(header.IFH_NumDocuments, 2);
    assert_eq!(header.IFH_AvgDocLength, 3.5);

    let mut merged = IndexContext::with_path(Some(base_path_text));
    assert_eq!(merged.DocumentCount(), 2);
    assert_eq!(merged.AvgDocLen(), 3.5);
    assert_eq!(merged.GetDocPath(0), "base.txt");
    assert_eq!(merged.GetDocPath(1), "delta1.txt");
    assert!(search_doc_ids(&mut merged, "gamma").contains(&1));
}

#[test]
fn merge_continuation_postings() {
    const BASE_DOCS: u64 = 760;
    const DELTA_DOCS: u64 = 760;

    let temp = tempfile::tempdir().unwrap();
    let base_path = temp.path().join("continuation.idx");
    let delta_path = delta_index_path(&base_path);
    let base_path_text = base_path.to_string_lossy().to_string();

    let mut base = IndexContext::new();
    for doc_id in 0..BASE_DOCS {
        add_doc(&mut base, doc_id, "longmerge", "base common", &format!("base-{doc_id}.txt"));
    }
    base.SaveIndex(&base_path_text).unwrap();

    let mut delta = IndexContext::new();
    for doc_id in BASE_DOCS..BASE_DOCS + DELTA_DOCS {
        add_doc(&mut delta, doc_id, "longmerge", "delta common", &format!("delta-{doc_id}.txt"));
    }
    delta.SaveIndex(&delta_path).unwrap();

    let mut merge = IndexContext::with_path(Some(base_path_text.clone()));
    merge.Merge(&base_path_text).unwrap();

    let mut merged = IndexContext::with_path(Some(base_path_text));
    let mut docs = search_doc_ids(&mut merged, "longmerge");
    docs.sort_unstable();
    assert_eq!(docs.len() as u64, BASE_DOCS + DELTA_DOCS);
    assert_eq!(docs.first().copied(), Some(0));
    assert_eq!(docs.last().copied(), Some(BASE_DOCS + DELTA_DOCS - 1));
}
