use rustblade::executor::IndexSearchExecutor;
use rustblade::index_writer::IndexWriter;
use rustblade::tokenizer::Tokenizer;
use rustblade::vector_index::build_hashed_embedding;
use rustblade::{IndexContext, SmartTokenizer};

fn add_doc(ctx: &mut IndexContext, doc_id: u64, title: &str, body: &str) {
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
    writer.SetDocPath(doc_id, format!("doc{doc_id}.txt"));
}

fn sync_results(ctx: &IndexContext, query: &str) -> Vec<u64> {
    let mut reader = ctx.GetReaderForQuery(query, "AUTB");
    let store = ctx.GetStore();
    let store = store.read().unwrap();
    let executor = IndexSearchExecutor::new(&store);
    executor.Execute(reader.as_mut(), 10).into_iter().map(|result| result.doc_id).collect()
}

#[test]
fn enqueue_matches_sync_search() {
    let mut ctx = IndexContext::new();
    add_doc(&mut ctx, 0, "Quick Fox", "the quick brown fox jumps over the lazy dog");
    add_doc(&mut ctx, 1, "Rust Search", "rust systems programming and fast search");
    add_doc(&mut ctx, 2, "Lazy Dog", "the lazy dog sleeps while the fox runs");
    ctx.Build();

    let expected = sync_results(&ctx, "fox lazy");
    let actual: Vec<u64> = ctx.Enqueue("fox lazy", Vec::new(), "AUTB", 10)
        .Wait()
        .into_iter()
        .map(|result| result.doc_id)
        .collect();
    assert!(!expected.is_empty());
    assert_eq!(actual, expected);
}

#[test]
fn enqueue_handles_many_tasks() {
    let mut ctx = IndexContext::new();
    add_doc(&mut ctx, 0, "Quick Fox", "the quick brown fox jumps over the lazy dog");
    add_doc(&mut ctx, 1, "Rust Search", "rust systems programming and fast search");
    add_doc(&mut ctx, 2, "Lazy Dog", "the lazy dog sleeps while the fox runs");
    ctx.Build();

    let mut tasks = Vec::new();
    for i in 0..64 {
        let query = if i % 2 == 0 { "fox lazy" } else { "rust search" };
        tasks.push((i, ctx.Enqueue(query, Vec::new(), "AUTB", 10)));
    }

    for (i, task) in tasks {
        let docs: Vec<u64> = task.Wait().into_iter().map(|result| result.doc_id).collect();
        assert!(!docs.is_empty());
        if i % 2 == 0 {
            assert!(docs.contains(&0));
        } else {
            assert!(docs.contains(&1));
        }
    }
}
