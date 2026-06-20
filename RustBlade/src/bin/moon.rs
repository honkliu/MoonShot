use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use rustblade::index_writer::IndexWriter;
use rustblade::tokenizer::Tokenizer;
use rustblade::{IndexContext, SmartTokenizer};

fn home_dir() -> PathBuf {
    if cfg!(windows) {
        std::env::var_os("USERPROFILE").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("C:/Users/Default"))
    } else {
        std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/tmp"))
    }
}

fn default_idx_path() -> PathBuf {
    home_dir().join("moon.idx")
}

fn is_indexable(path: &Path) -> bool {
    matches!(path.extension().and_then(|ext| ext.to_str()).map(|ext| ext.to_ascii_lowercase()), Some(ext) if ext == "txt" || ext == "md")
}

fn collect_files(path: &Path, files: &mut Vec<PathBuf>) -> io::Result<()> {
    if path.is_file() {
        if is_indexable(path) { files.push(path.to_path_buf()); }
        return Ok(());
    }
    if path.is_dir() {
        for entry in fs::read_dir(path)? {
            let entry = entry?;
            collect_files(&entry.path(), files)?;
        }
    }
    Ok(())
}

fn index_path(path: &Path) -> io::Result<()> {
    let mut files = Vec::new();
    collect_files(path, &mut files)?;
    files.sort();

    let mut context = IndexContext::new();
    let tokenizer = SmartTokenizer::new();
    let mut writer = context.get_writer();

    for (doc_id, file) in files.iter().enumerate() {
        let Ok(body) = fs::read_to_string(file) else { continue; };
        let title = file.file_stem().and_then(|stem| stem.to_str()).unwrap_or("");
        writer.write(tokenizer.tokenize(title), doc_id as u64, "Title");
        writer.write(tokenizer.tokenize(&body), doc_id as u64, "Body");
        writer.set_doc_importance(doc_id as u64, 0.1);
        let abs = file.canonicalize().unwrap_or_else(|_| file.clone());
        writer.set_doc_path(doc_id as u64, abs.to_string_lossy().to_string());
    }

    let idx = default_idx_path();
    context.save_index(idx.to_string_lossy().as_ref()).map_err(|err| io::Error::new(io::ErrorKind::Other, format!("{err:?}")))?;
    println!("Rebuilt index with {} readable document(s)", files.len());
    println!("Indexed input: {}", path.display());
    println!("Total:   {} document(s) in {}", files.len(), idx.display());
    Ok(())
}

fn interactive() -> io::Result<()> {
    let idx = default_idx_path();
    let mut context = IndexContext::new();
    let loaded = context.load_index(idx.to_string_lossy().as_ref()).is_ok();
    let docs = context.with_store(|store| store.total_docs());
    if !loaded {
        eprintln!("Failed to load index: {}", idx.display());
    }
    println!("moon search — {} document(s)", docs);
    println!("Type a query, or 'quit' to exit.");

    let mut line = String::new();
    loop {
        print!("> ");
        io::stdout().flush()?;
        line.clear();
        if io::stdin().read_line(&mut line)? == 0 { break; }
        let query = line.trim();
        if query.eq_ignore_ascii_case("quit") || query.eq_ignore_ascii_case("exit") { break; }
        if query.is_empty() { continue; }
        if docs == 0 {
            println!("(no results)");
            continue;
        }
        let results = context.search(query, 0, "AUTB");
        if results.is_empty() {
            println!("(no results)");
            continue;
        }
        println!("{} result(s)", results.len());
        for result in results.iter().take(20) {
            let path = context.with_store(|store| store.get_doc_path(result.doc_id).to_string());
            println!("{}", if path.is_empty() { "[unknown]".to_string() } else { path });
        }
    }
    Ok(())
}

fn usage() {
    eprintln!("usage:");
    eprintln!("  moon_rs -name <file-or-dir>");
    eprintln!("  moon_rs -i");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let result = match args.get(1).map(|arg| arg.as_str()) {
        Some("-name") if args.len() >= 3 => index_path(Path::new(&args[2])),
        Some("-i") => interactive(),
        _ => { usage(); Ok(()) }
    };
    if let Err(error) = result {
        eprintln!("moon_rs: {error}");
        std::process::exit(1);
    }
}
