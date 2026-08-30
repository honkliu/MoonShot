//! MoonShot CLI translation; cross-language API names intentionally match `moon.cpp`.
#![allow(non_snake_case, non_upper_case_globals)]

use std::collections::{HashMap, HashSet};
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufRead, BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

// Rust command-line counterpart to examples/moon.cpp.

use rustblade::block_table::{
    DocDataDecodeScore, DocDataEntry, IndexFileHeader, DOC_REC_SIZE, DOC_VECTOR_DIM,
    INDEX_FILE_HEADER_SIZE, LEAF_TERM_CACHE_BYTES, PAGE_SIZE, TERM_MPHF_HEADER_SIZE,
};
use rustblade::eval_expression::{EvalNode, EvalTree, OrNode, TermNode, WeakAndBuildMode};
use rustblade::file_access::FileAccess;
use rustblade::index_reader::{
    IndexReader, ReaderDocumentIDSourceMask, ReaderDocumentIDValue, READER_SOURCE_ANCHOR,
    READER_SOURCE_BODY, READER_SOURCE_TITLE, READER_SOURCE_URL, READER_SOURCE_VECTOR,
};
use rustblade::index_serializer::IndexSerializer;
use rustblade::tokenizer_interface::Tokenizer;
use rustblade::{
    Document, GetQueryCompileModeParameters, IndexContext, IndexWriter, QueryCompileMode,
    SearchResult, SmartTokenizer,
};

const MAX_INDEX_FILE_BYTES: u64 = 8 * 1024 * 1024;
const BGE_MAX_TEXT_BYTES: usize = 65_536;
const BGE_DOCUMENT_MARKER: &str = "__MOONSHOT_BGE_DOCUMENT__\n";

#[derive(Clone)]
struct SearchOptions {
    inverted: bool,
    vector: bool,
    bge: bool,
    bge_sidecar: bool,
    top_k: usize,
    ef_search: usize,
    bge_host: String,
    bge_port: u16,
    bge_python: String,
    bge_script: String,
    bge_model: String,
}

impl Default for SearchOptions {
    fn default() -> Self {
        Self {
            inverted: false,
            vector: false,
            bge: false,
            bge_sidecar: false,
            top_k: 1000,
            ef_search: 1000,
            bge_host: "127.0.0.1".into(),
            bge_port: 8765,
            bge_python: String::new(),
            bge_script: String::new(),
            bge_model: "BAAI/bge-small-en-v1.5".into(),
        }
    }
}

#[derive(Default)]
struct BeirBuildOptions {
    data_path: PathBuf,
    doc_vectors_path: Option<PathBuf>,
    limit: u64,
    build_vectors: bool,
}

#[derive(Default)]
struct BeirPatchVectorOptions {
    source_index_path: PathBuf,
    doc_vectors_path: PathBuf,
    limit: u64,
}

struct BeirEvalOptions {
    data_path: PathBuf,
    qrels: String,
    run_out: Option<PathBuf>,
    dump_features_path: Option<PathBuf>,
    query_vectors_path: Option<PathBuf>,
    streams: String,
    mode: String,
    weak_and_shape: String,
    at: Vec<usize>,
    limit: u64,
    vector_ef: usize,
    query_threads: usize,
    no_mphf: bool,
    leaf_cache_mb: u64,
    leaf_cache_match_mphf: bool,
    use_enqueue: bool,
}

impl Default for BeirEvalOptions {
    fn default() -> Self {
        Self {
            data_path: PathBuf::new(),
            qrels: "test".into(),
            run_out: None,
            dump_features_path: None,
            query_vectors_path: None,
            streams: "TB".into(),
            mode: "weakandbigram".into(),
            weak_and_shape: "flat".into(),
            at: vec![10, 100, 1000],
            limit: 0,
            vector_ef: 1000,
            query_threads: 1,
            no_mphf: false,
            leaf_cache_mb: 0,
            leaf_cache_match_mphf: false,
            use_enqueue: false,
        }
    }
}

fn home_dir() -> PathBuf {
    if cfg!(windows) {
        std::env::var_os("USERPROFILE")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("C:/Users/Default"))
    } else {
        std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/tmp"))
    }
}

fn default_idx_path() -> PathBuf {
    home_dir().join("moon.idx")
}

fn absolute_path(path: impl AsRef<Path>) -> PathBuf {
    let path = path.as_ref();
    path.canonicalize().unwrap_or_else(|_| {
        if path.is_absolute() {
            path.to_path_buf()
        } else {
            std::env::current_dir().unwrap_or_default().join(path)
        }
    })
}

fn delta_index_path(path: &Path) -> PathBuf {
    match (path.file_stem(), path.extension()) {
        (Some(stem), Some(ext)) => {
            let mut name = stem.to_os_string();
            name.push(".delta.");
            name.push(ext);
            path.with_file_name(name)
        }
        _ => PathBuf::from(format!("{}.delta.idx", path.display())),
    }
}

fn parse_extensions(value: &str) -> Vec<String> {
    value
        .split(',')
        .map(|item| item.trim().trim_start_matches('.').to_ascii_lowercase())
        .filter(|item| !item.is_empty())
        .collect()
}

fn truncate_utf8(text: &mut String, max_bytes: usize) {
    if text.len() <= max_bytes {
        return;
    }
    let mut end = max_bytes;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    text.truncate(end);
}

fn collect_files(
    path: &Path,
    extensions: &[String],
    recursive: bool,
    single: bool,
) -> io::Result<Vec<PathBuf>> {
    fn add(path: &Path, extensions: &[String], check_extension: bool, files: &mut Vec<PathBuf>) {
        let Ok(metadata) = fs::metadata(path) else {
            return;
        };
        let matches = path
            .extension()
            .and_then(|ext| ext.to_str())
            .map(|ext| {
                extensions
                    .iter()
                    .any(|value| value.eq_ignore_ascii_case(ext))
            })
            .unwrap_or(false);
        if metadata.is_file()
            && metadata.len() <= MAX_INDEX_FILE_BYTES
            && (!check_extension || matches)
        {
            files.push(path.canonicalize().unwrap_or_else(|_| path.to_path_buf()));
        }
    }
    fn visit(
        path: &Path,
        extensions: &[String],
        recursive: bool,
        files: &mut Vec<PathBuf>,
    ) -> io::Result<()> {
        for entry in fs::read_dir(path)? {
            let child = entry?.path();
            if child.is_dir() {
                if recursive {
                    visit(&child, extensions, true, files)?;
                }
            } else {
                add(&child, extensions, true, files);
            }
        }
        Ok(())
    }
    let mut files = Vec::new();
    if single {
        add(path, extensions, false, &mut files);
    } else if path.is_dir() {
        visit(path, extensions, recursive, &mut files)?;
    }
    Ok(files)
}

fn add_files(
    context: &mut IndexContext,
    files: &[PathBuf],
    first_id: u64,
    preserve_id_slots: bool,
    build_vector: bool,
    bge: Option<&SearchOptions>,
) -> (u64, u64) {
    let mut kept = 0;
    let mut skipped = 0;
    let mut bge_vectors = 0;
    for (file_index, file) in files.iter().enumerate() {
        let Ok(body) = fs::read_to_string(file) else {
            skipped += 1;
            continue;
        };
        if body.is_empty() {
            skipped += 1;
            continue;
        }
        let title = file
            .file_stem()
            .and_then(|stem| stem.to_str())
            .unwrap_or("")
            .replace(['_', '-'], " ");
        let doc_id = first_id
            + if preserve_id_slots {
                file_index as u64
            } else {
                kept
            };
        let mut embedding_text = if title.is_empty() {
            body.clone()
        } else {
            format!("{title}\n{body}")
        };
        truncate_utf8(
            &mut embedding_text,
            BGE_MAX_TEXT_BYTES - BGE_DOCUMENT_MARKER.len(),
        );
        let doc = Document {
            doc_id,
            path: file.to_string_lossy().into_owned(),
            title,
            body,
            ..Document::default()
        };
        context.AddDocument(&doc, build_vector && bge.is_none());
        if let Some(options) = bge {
            match bge_vector(&embedding_text, true, options) {
                Ok(vector) => {
                    context.GetWriter().SetDocVector(doc_id, vector);
                    bge_vectors += 1;
                }
                Err(error) => eprintln!(
                    "  warning: BGE document embedding failed; added without vector: {} ({error})",
                    file.display()
                ),
            }
        }
        kept += 1;
    }
    if bge.is_some() {
        println!("  embedded {bge_vectors} BGE document vector(s)");
    }
    (kept, skipped)
}

fn add_assigned_files(
    context: &mut IndexContext,
    files: &[(PathBuf, u64)],
    build_vector: bool,
    bge: Option<&SearchOptions>,
) -> (u64, u64) {
    let mut kept = 0;
    let mut skipped = 0;
    let mut bge_vectors = 0;
    for (file, doc_id) in files {
        let Ok(body) = fs::read_to_string(file) else {
            skipped += 1;
            continue;
        };
        if body.is_empty() {
            skipped += 1;
            continue;
        }
        let title = file
            .file_stem()
            .and_then(|stem| stem.to_str())
            .unwrap_or("")
            .replace(['_', '-'], " ");
        let mut embedding_text = if title.is_empty() {
            body.clone()
        } else {
            format!("{title}\n{body}")
        };
        truncate_utf8(
            &mut embedding_text,
            BGE_MAX_TEXT_BYTES - BGE_DOCUMENT_MARKER.len(),
        );
        let doc = Document {
            doc_id: *doc_id,
            path: file.to_string_lossy().into_owned(),
            title,
            body,
            ..Document::default()
        };
        context.AddDocument(&doc, build_vector && bge.is_none());
        if let Some(options) = bge {
            match bge_vector(&embedding_text, true, options) {
                Ok(vector) => {
                    context.GetWriter().SetDocVector(*doc_id, vector);
                    bge_vectors += 1;
                }
                Err(error) => eprintln!(
                    "  warning: BGE document embedding failed; added without vector: {} ({error})",
                    file.display()
                ),
            }
        }
        kept += 1;
    }
    if bge.is_some() {
        println!("  embedded {bge_vectors} BGE document vector(s)");
    }
    (kept, skipped)
}

fn index_path(
    index: &Path,
    input: &Path,
    extensions: &[String],
    recursive: bool,
    single: bool,
    batch_size: usize,
    bge: Option<&SearchOptions>,
) -> io::Result<()> {
    let files = collect_files(input, extensions, recursive, single)?;
    if files.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("No readable indexable files found: {}", input.display()),
        ));
    }
    let delta = delta_index_path(index);
    if IndexSerializer::IsValidIndex(&index.to_string_lossy())
        && IndexSerializer::IsValidIndex(&delta.to_string_lossy())
    {
        let mut context = IndexContext::with_path(Some(index.to_string_lossy().into_owned()));
        context
            .Merge(&index.to_string_lossy())
            .map_err(rust_error)?;
        let _ = fs::remove_file(&delta);
    }
    let next_id = if IndexSerializer::IsValidIndex(&index.to_string_lossy()) {
        IndexContext::with_path_and_load_delta(Some(index.to_string_lossy().into_owned()), false)
            .AllocateDocumentID()
    } else {
        0
    };
    let batch = PathBuf::from(format!("{}.batch.tmp", index.display()));
    let mut skipped = 0;
    let mut saved_batches = 0;
    let pending_files: Vec<_> = files
        .iter()
        .cloned()
        .enumerate()
        .map(|(offset, file)| (file, next_id + offset as u64))
        .collect();
    for chunk in pending_files.chunks(batch_size) {
        let mut batch_files = chunk.to_vec();
        batch_files.sort_by(|left, right| left.0.cmp(&right.0));
        let mut context = IndexContext::new();
        let (batch_kept, batch_skipped) = add_assigned_files(&mut context, &batch_files, true, bge);
        if batch_kept == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Failed to save batch index: {}", batch.display()),
            ));
        }
        context
            .SaveIndex(&batch.to_string_lossy())
            .map_err(rust_error)?;
        if !IndexSerializer::IsValidIndex(&index.to_string_lossy()) {
            fs::rename(&batch, index)?;
        } else {
            let _ = fs::remove_file(&delta);
            fs::rename(&batch, &delta)?;
            let mut merge = IndexContext::with_path(Some(index.to_string_lossy().into_owned()));
            merge.Merge(&index.to_string_lossy()).map_err(rust_error)?;
            let _ = fs::remove_file(&delta);
        }
        skipped += batch_skipped;
        saved_batches += 1;
    }
    let total = if IndexSerializer::IsValidIndex(&index.to_string_lossy()) {
        IndexContext::with_path_and_load_delta(Some(index.to_string_lossy().into_owned()), false)
            .DocumentCount()
    } else {
        0
    };
    println!("Indexed input: {}\nFiles:   {} (appended {})\nBatch size: {}\nSaved batches: {}\nSaved:   {} document(s){} to {}\nTotal:   {} indexed document(s)",
        input.display(), files.len(), files.len(), batch_size, saved_batches, total,
        if skipped > 0 { format!(" (skipped {skipped})") } else { String::new() }, index.display(), total);
    Ok(())
}

fn rust_error(error: impl std::fmt::Debug) -> io::Error {
    io::Error::other(format!("{error:?}"))
}

fn parse_u64(value: &str, message: &str) -> io::Result<u64> {
    value
        .parse()
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, message))
}

fn default_bge_python() -> String {
    let local = if cfg!(windows) {
        Path::new(".venv-bge/Scripts/python.exe")
    } else {
        Path::new(".venv-bge/bin/python")
    };
    if local.is_file() {
        local.to_string_lossy().into_owned()
    } else {
        "python".into()
    }
}

fn default_bge_script() -> String {
    Path::new("Tools/embed_query.py")
        .to_string_lossy()
        .into_owned()
}

fn read_single_i8_vector(path: &Path) -> io::Result<Vec<f32>> {
    let mut input = BufReader::new(File::open(path)?);
    let mut magic = [0u8; 8];
    let mut value = [0u8; 4];
    input.read_exact(&mut magic)?;
    input.read_exact(&mut value)?;
    let dim = u32::from_le_bytes(value) as usize;
    input.read_exact(&mut value)?;
    let id_bytes = u32::from_le_bytes(value) as usize;
    if &magic != b"MSVECI81" || dim != DOC_VECTOR_DIM || id_bytes == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid BGE vector output",
        ));
    }
    let mut id = vec![0; id_bytes];
    let mut encoded = vec![0; dim];
    input.read_exact(&mut id)?;
    input.read_exact(&mut encoded)?;
    Ok(encoded
        .into_iter()
        .map(|value| value as i8 as f32 / 128.0)
        .collect())
}

fn bge_service_vector(
    text: &str,
    document_mode: bool,
    options: &SearchOptions,
) -> io::Result<Vec<f32>> {
    let mut stream = TcpStream::connect((options.bge_host.as_str(), options.bge_port)).map_err(|error| io::Error::new(error.kind(),
        format!("BGE service unavailable at {}:{}; start Tools/bge_embedding_service.py: {error}", options.bge_host, options.bge_port)))?;
    let payload = if document_mode {
        format!("{BGE_DOCUMENT_MARKER}{text}")
    } else {
        text.to_string()
    };
    let bytes = payload.as_bytes();
    let length = bytes.len().min(BGE_MAX_TEXT_BYTES);
    stream.write_all(&(length as u32).to_le_bytes())?;
    stream.write_all(&bytes[..length])?;
    let mut dim = [0u8; 4];
    stream.read_exact(&mut dim)?;
    let dim = u32::from_le_bytes(dim) as usize;
    if dim != DOC_VECTOR_DIM {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "BGE vector dimension mismatch",
        ));
    }
    let mut encoded = vec![0; dim];
    stream.read_exact(&mut encoded)?;
    Ok(encoded
        .into_iter()
        .map(|value| value as i8 as f32 / 128.0)
        .collect())
}

fn bge_sidecar_vector(
    text: &str,
    document_mode: bool,
    options: &SearchOptions,
) -> io::Result<Vec<f32>> {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let prefix = if document_mode {
        "moonshot_bge_doc"
    } else {
        "moonshot_bge_query"
    };
    let text_path =
        std::env::temp_dir().join(format!("{prefix}_{}_{}.txt", std::process::id(), stamp));
    let vector_path =
        std::env::temp_dir().join(format!("{prefix}_{}_{}.i8bin", std::process::id(), stamp));
    fs::write(&text_path, text)?;
    let python = if options.bge_python.is_empty() {
        default_bge_python()
    } else {
        options.bge_python.clone()
    };
    let script = if options.bge_script.is_empty() {
        default_bge_script()
    } else {
        options.bge_script.clone()
    };
    let mut command = Command::new(&python);
    command
        .arg(&script)
        .arg("--text-file")
        .arg(&text_path)
        .arg("--output")
        .arg(&vector_path)
        .arg("--model")
        .arg(&options.bge_model);
    if document_mode {
        command.arg("--no-default-prefix");
    }
    let result = match command.status() {
        Ok(status) if status.success() => read_single_i8_vector(&vector_path),
        Ok(status) => Err(io::Error::other(format!(
            "BGE sidecar exited with {status}"
        ))),
        Err(error) => Err(error),
    };
    let _ = fs::remove_file(text_path);
    let _ = fs::remove_file(vector_path);
    result
}

fn bge_vector(text: &str, document_mode: bool, options: &SearchOptions) -> io::Result<Vec<f32>> {
    if options.bge_sidecar {
        bge_sidecar_vector(text, document_mode, options)
    } else {
        bge_service_vector(text, document_mode, options)
    }
}

fn source_mask(mask: u8) -> String {
    let mut value = ['-'; 5];
    if mask & READER_SOURCE_ANCHOR != 0 {
        value[0] = 'A';
    }
    if mask & READER_SOURCE_URL != 0 {
        value[1] = 'U';
    }
    if mask & READER_SOURCE_TITLE != 0 {
        value[2] = 'T';
    }
    if mask & READER_SOURCE_BODY != 0 {
        value[3] = 'B';
    }
    if mask & READER_SOURCE_VECTOR != 0 {
        value[4] = 'V';
    }
    value.iter().collect()
}

fn execute_search(
    context: &mut IndexContext,
    query: &str,
    options: &SearchOptions,
) -> io::Result<Vec<String>> {
    let streams = format!(
        "{}{}",
        if options.inverted { "AUTB" } else { "" },
        if options.vector && !options.bge {
            "V"
        } else {
            ""
        }
    );
    let results = if options.bge {
        context
            .EnqueueWithMode(
                query,
                bge_vector(query, false, options)?,
                &streams,
                options.top_k as i32,
                QueryCompileMode::WeakAndBigram,
                options.ef_search,
            )
            .Wait()
    } else {
        let mut tree = context.Compile(query, &streams);
        tree.vector_ef_search = options.ef_search;
        let vector = if tree.HasTextQuery() && tree.HasVectorQuery() {
            Some(tree.vector_query.clone())
        } else {
            None
        };
        let mut reader = context.GetReader(tree);
        context
            .GetExecutor()
            .ExecuteWithVector(reader.as_mut(), 0, vector.as_deref())
    };
    if results.is_empty() {
        println!("(no results)");
        return Ok(Vec::new());
    }
    let paths: Vec<_> = results
        .iter()
        .map(|result| context.GetDocPath(result.doc_id))
        .collect();
    println!("{} result(s)", results.len());
    for offset in (0..results.len()).step_by(20) {
        let end = (offset + 20).min(results.len());
        if results.len() > 20 {
            println!("-- showing {}-{end} of {} --", offset + 1, results.len());
        }
        for index in offset..end {
            let result = &results[index];
            println!(
                "{} {:05.2} {} {}",
                index + 1,
                result.score.max(0.0),
                source_mask(ReaderDocumentIDSourceMask(result.doc_id)),
                if paths[index].is_empty() {
                    "[unknown]"
                } else {
                    &paths[index]
                }
            );
        }
        if end < results.len() {
            print!("-- press Enter for next page, q then Enter to stop --");
            io::stdout().flush()?;
            let mut answer = String::new();
            io::stdin().read_line(&mut answer)?;
            if answer.trim().eq_ignore_ascii_case("q") {
                break;
            }
        }
    }
    Ok(paths)
}

fn page_file(path: &Path) -> io::Result<()> {
    let input = BufReader::new(File::open(path)?);
    let mut lines_on_page = 0usize;
    for line in input.lines() {
        println!("{}", line?);
        lines_on_page += 1;
        if lines_on_page == 20 {
            print!("-- More -- (Enter to continue, q then Enter to quit)");
            io::stdout().flush()?;
            let mut answer = String::new();
            io::stdin().read_line(&mut answer)?;
            println!();
            if answer.trim().eq_ignore_ascii_case("q") {
                return Ok(());
            }
            lines_on_page = 0;
        }
    }
    Ok(())
}

fn interactive(index: &Path, options: SearchOptions) -> io::Result<()> {
    let load_start = Instant::now();
    let mut context = IndexContext::with_path(Some(index.to_string_lossy().into_owned()));
    let mut document_count = context.DocumentCount();
    if let Some(delta) = context.GetDeltaContext() {
        document_count += delta.DocumentCount();
    }
    let mode = if options.bge && options.inverted {
        "inverted+BGE"
    } else if options.bge {
        "BGE vector"
    } else if options.inverted && options.vector {
        "inverted+vector"
    } else if options.inverted {
        "inverted"
    } else {
        "vector"
    };
    println!(
        "moon search — {document_count} document(s) (loaded in {} ms)\nMode: {mode}",
        load_start.elapsed().as_millis()
    );
    if options.vector && !options.inverted {
        let started = Instant::now();
        print!("Building vector graph...");
        io::stdout().flush()?;
        context.Build();
        let mut vectors = context.VectorCount();
        if let Some(delta) = context.GetDeltaContext() {
            delta.Build();
            vectors += delta.VectorCount();
        }
        println!(
            " {vectors} vector(s) in {} ms",
            started.elapsed().as_millis()
        );
    }
    println!("Type a query, or /h for commands.");

    let mut line = String::new();
    let mut last_paths = Vec::new();
    loop {
        print!("> ");
        io::stdout().flush()?;
        line.clear();
        if io::stdin().read_line(&mut line)? == 0 {
            break;
        }
        let query = line.trim();
        if query == "/q" {
            break;
        }
        if query.is_empty() {
            continue;
        }
        if query == "/h" {
            println!("Commands:\n  /h Show help\n  /q Quit\n  /a <file> Add a document\n  /a <dir> -e md,txt [-r] Add documents\n  /s Save pending additions as delta\n  /m Merge delta and reload\n  @N Open result N");
        } else if let Some(arguments) = query.strip_prefix("/a ") {
            let parts: Vec<_> = arguments.split_whitespace().collect();
            let path = parts.first().ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "usage: /a <file> | /a <dir> -e md,txt [-r]",
                )
            })?;
            let mut extensions = parse_extensions("md,txt");
            let mut recursive = false;
            let mut position = 1;
            while position < parts.len() {
                match parts[position] {
                    "-e" if position + 1 < parts.len() => {
                        position += 1;
                        extensions = parse_extensions(parts[position]);
                    }
                    "-r" => recursive = true,
                    option => {
                        println!("unknown /a option: {option}");
                        position += 1;
                        continue;
                    }
                }
                position += 1;
            }
            let path = Path::new(path);
            let files = collect_files(path, &extensions, recursive, path.is_file())?;
            if files.is_empty() {
                println!("no readable indexable files found: {}", path.display());
                continue;
            }
            let first_id = context.AllocateDocumentID();
            let (kept, skipped) = add_files(
                &mut context,
                &files,
                first_id,
                false,
                true,
                options.bge.then_some(&options),
            );
            println!(
                "added {kept} document(s) to memory{}; run /s to publish as delta",
                if skipped > 0 {
                    format!(" (skipped {skipped})")
                } else {
                    String::new()
                }
            );
        } else if query == "/s" {
            if IndexSerializer::IsValidIndex(&index.to_string_lossy()) {
                let delta = delta_index_path(index);
                context
                    .SaveIndex(&delta.to_string_lossy())
                    .map_err(rust_error)?;
                println!("saved and published delta: {}", delta.display());
            } else {
                context
                    .SaveIndex(&index.to_string_lossy())
                    .map_err(rust_error)?;
                context
                    .LoadIndex(&index.to_string_lossy())
                    .map_err(rust_error)?;
                println!("saved and loaded index: {}", index.display());
            }
        } else if query == "/m" {
            if context.HasDelta() {
                context
                    .Merge(&index.to_string_lossy())
                    .map_err(rust_error)?;
                let _ = fs::remove_file(delta_index_path(index));
                context
                    .LoadIndex(&index.to_string_lossy())
                    .map_err(rust_error)?;
                println!(
                    "merged delta into main index and reloaded: {}",
                    index.display()
                );
            } else {
                println!("no delta loaded");
            }
        } else if query.starts_with('@') {
            let Ok(number) = query[1..].parse::<usize>() else {
                println!("usage: @N (for example, @1)");
                continue;
            };
            if number == 0 || number > last_paths.len() {
                println!("result {number} is not available; run a search first");
            } else if last_paths[number - 1].is_empty() {
                println!("result {number} has no file path");
            } else if page_file(Path::new(&last_paths[number - 1])).is_err() {
                println!("unable to read: {}", last_paths[number - 1]);
            }
        } else if query.starts_with('/') {
            println!("unknown command: {query} (try /h)");
        } else {
            last_paths = execute_search(&mut context, query, &options)?;
        }
    }
    Ok(())
}

fn usage() {
    println!("MoonShot document indexing and search\n\nUSAGE\n  moon [global-options] <command> [command-options]\n\nCOMMANDS\n  -file <path>              Index one file\n  -dir <path>               Index files in a directory\n  -i                        Interactive inverted-index search\n  -v                        Interactive vector search\n  -i -v                     Interactive hybrid search\n  -sample-merge             Run the base/delta/merge example\n  -beir-build               Build an index from a BEIR corpus\n  -beir-patch-vectors       Copy an index and replace vectors\n  -beir-eval                Evaluate BEIR Recall@k\n\nGLOBAL OPTIONS\n  -idx <path>               Index path (default: {})\n  -h, --help                Show this help\n\nINDEX OPTIONS\n  -ext <list>               Extensions (default: md,txt)\n  -r                        Traverse recursively\n  -b <count>                Delta batch size (default/minimum: 10000)\n  -bge                      Store BGE document vectors; never aliases TFIDF\n\nSEARCH OPTIONS\n  -bge                      Embed vector queries with BGE; never aliases TFIDF\n  -k <count>                Vector candidates (default: 1000)\n  -ef <count>               Vector efSearch (default: 1000)\n\nBGE OPTIONS\n  -bge-host <host>          Service host (default: 127.0.0.1)\n  -bge-port <port>          Service port (default: 8765)\n  -bge-sidecar              Start a Python sidecar per request\n  -bge-python <path>        Sidecar Python executable\n  -bge-script <path>        Sidecar embedding script\n  -bge-model <name>         Embedding model\n\nBEIR BUILD\n  -data <dir> [-doc-vectors <file>] [-build-vectors] [-limit N]\n\nBEIR PATCH VECTORS\n  -src-index <index> -doc-vectors <file> [-limit N]\n\nBEIR EVALUATION\n  -data <dir> [-qrels test] [-k 10,100,1000] [-streams TB]\n  [-mode bow|weakandbigram|weakandbigramboost|weakandbigramboostdoc|vector|hybrid|hybridboost|hybridboostdoc|compile]\n  [-weakand-shape flat|or|or-prune] [-query-vectors <file>] [-vector-ef N]\n  [-query-threads N] [-enqueue] [-run-out <path>] [-dump-features <path>]\n  [-no-mphf] [-leaf-cache-mb N] [-leaf-cache-match-mphf] [-limit N]\n\nENVIRONMENT\n  MOONSHOT_BLOCK_ACCESS=worker  Use dedicated block I/O workers", default_idx_path().display());
}

fn parse_search_options(args: &[String]) -> io::Result<SearchOptions> {
    let mut options = SearchOptions::default();
    let mut index = 0;
    while index < args.len() {
        match args[index].as_str() {
            "-i" => options.inverted = true,
            "-v" => options.vector = true,
            "-bge" => {
                options.bge = true;
                options.vector = true;
            }
            "-bge-sidecar" => options.bge_sidecar = true,
            "-bge-python" if index + 1 < args.len() => {
                index += 1;
                options.bge_python = args[index].clone();
            }
            "-bge-script" if index + 1 < args.len() => {
                index += 1;
                options.bge_script = args[index].clone();
            }
            "-bge-model" if index + 1 < args.len() => {
                index += 1;
                options.bge_model = args[index].clone();
            }
            "-bge-host" if index + 1 < args.len() => {
                index += 1;
                options.bge_host = args[index].clone();
            }
            "-bge-port" if index + 1 < args.len() => {
                index += 1;
                options.bge_port = args[index]
                    .parse()
                    .ok()
                    .filter(|value| *value > 0)
                    .ok_or_else(|| {
                        io::Error::new(io::ErrorKind::InvalidInput, "-bge-port must be 1..65535")
                    })?;
            }
            "-k" if index + 1 < args.len() => {
                index += 1;
                options.top_k = args[index]
                    .parse()
                    .ok()
                    .filter(|value| *value > 0)
                    .ok_or_else(|| {
                        io::Error::new(io::ErrorKind::InvalidInput, "-k must be positive")
                    })?;
            }
            "-ef" if index + 1 < args.len() => {
                index += 1;
                options.ef_search = args[index]
                    .parse()
                    .ok()
                    .filter(|value| *value > 0)
                    .ok_or_else(|| {
                        io::Error::new(io::ErrorKind::InvalidInput, "-ef must be positive")
                    })?;
            }
            option => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("Unknown search option: {option}"),
                ))
            }
        }
        index += 1;
    }
    if !options.inverted && !options.vector {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Usage: moon [-idx <index>] -i [-v] | moon [-idx <index>] -v [-bge]",
        ));
    }
    Ok(options)
}

fn parse_at_list(value: &str) -> io::Result<Vec<usize>> {
    let mut values: Vec<_> = value
        .split(',')
        .filter(|item| !item.trim().is_empty())
        .map(|item| {
            item.trim()
                .parse::<usize>()
                .ok()
                .filter(|value| *value > 0)
                .ok_or_else(|| {
                    io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "-k must be a comma-separated list of positive integers",
                    )
                })
        })
        .collect::<io::Result<_>>()?;
    values.sort_unstable();
    values.dedup();
    if values.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "-k must not be empty",
        ));
    }
    Ok(values)
}

fn parse_beir_build_options(args: &[String]) -> io::Result<BeirBuildOptions> {
    let mut options = BeirBuildOptions::default();
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "-data" if index + 1 < args.len() => {
                index += 1;
                options.data_path = PathBuf::from(&args[index]);
            }
            "-doc-vectors" if index + 1 < args.len() => {
                index += 1;
                options.doc_vectors_path = Some(PathBuf::from(&args[index]));
            }
            "-build-vectors" => options.build_vectors = true,
            "-limit" if index + 1 < args.len() => {
                index += 1;
                options.limit = parse_u64(&args[index], "-limit must be a non-negative integer")?;
            }
            option => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("Unknown BEIR build option: {option}"),
                ))
            }
        }
        index += 1;
    }
    if options.data_path.as_os_str().is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Usage: moon [-idx <index>] -beir-build -data <beir-dir> [-limit N]",
        ));
    }
    Ok(options)
}

fn parse_beir_patch_options(args: &[String]) -> io::Result<BeirPatchVectorOptions> {
    let mut options = BeirPatchVectorOptions::default();
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "-src-index" if index + 1 < args.len() => {
                index += 1;
                options.source_index_path = PathBuf::from(&args[index]);
            }
            "-doc-vectors" if index + 1 < args.len() => {
                index += 1;
                options.doc_vectors_path = PathBuf::from(&args[index]);
            }
            "-limit" if index + 1 < args.len() => {
                index += 1;
                options.limit = parse_u64(&args[index], "-limit must be a non-negative integer")?;
            }
            option => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("Unknown BEIR patch option: {option}"),
                ))
            }
        }
        index += 1;
    }
    if options.source_index_path.as_os_str().is_empty()
        || options.doc_vectors_path.as_os_str().is_empty()
    {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "Usage: moon [-idx <output-index>] -beir-patch-vectors -src-index <index> -doc-vectors <vectors>"));
    }
    Ok(options)
}

fn parse_beir_eval_options(args: &[String]) -> io::Result<BeirEvalOptions> {
    let mut options = BeirEvalOptions::default();
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "-data" if index + 1 < args.len() => {
                index += 1;
                options.data_path = PathBuf::from(&args[index]);
            }
            "-qrels" if index + 1 < args.len() => {
                index += 1;
                options.qrels = args[index].clone();
            }
            "-run-out" if index + 1 < args.len() => {
                index += 1;
                options.run_out = Some(PathBuf::from(&args[index]));
            }
            "-dump-features" if index + 1 < args.len() => {
                index += 1;
                options.dump_features_path = Some(PathBuf::from(&args[index]));
            }
            "-query-vectors" if index + 1 < args.len() => {
                index += 1;
                options.query_vectors_path = Some(PathBuf::from(&args[index]));
            }
            "-k" if index + 1 < args.len() => {
                index += 1;
                options.at = parse_at_list(&args[index])?;
            }
            "-streams" if index + 1 < args.len() => {
                index += 1;
                options.streams = args[index].clone();
            }
            "-mode" if index + 1 < args.len() => {
                index += 1;
                if !matches!(
                    args[index].as_str(),
                    "bow"
                        | "weakandbigram"
                        | "weakandbigramboost"
                        | "weakandbigramboostdoc"
                        | "vector"
                        | "hybrid"
                        | "hybridboost"
                        | "hybridboostdoc"
                        | "compile"
                ) {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "invalid BEIR eval mode",
                    ));
                }
                options.mode = args[index].clone();
            }
            "-weakand-shape" if index + 1 < args.len() => {
                index += 1;
                if !matches!(args[index].as_str(), "flat" | "or" | "or-prune") {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "-weakand-shape must be flat, or, or-prune",
                    ));
                }
                options.weak_and_shape = args[index].clone();
            }
            "-no-mphf" => options.no_mphf = true,
            "-leaf-cache-mb" if index + 1 < args.len() => {
                index += 1;
                options.leaf_cache_mb =
                    parse_u64(&args[index], "-leaf-cache-mb must be a positive integer")?;
                if options.leaf_cache_mb == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "-leaf-cache-mb must be a positive integer",
                    ));
                }
            }
            "-leaf-cache-match-mphf" => options.leaf_cache_match_mphf = true,
            "-limit" if index + 1 < args.len() => {
                index += 1;
                options.limit = parse_u64(&args[index], "-limit must be a non-negative integer")?;
            }
            "-vector-ef" if index + 1 < args.len() => {
                index += 1;
                options.vector_ef =
                    parse_u64(&args[index], "-vector-ef must be a positive integer")? as usize;
                if options.vector_ef == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "-vector-ef must be a positive integer",
                    ));
                }
            }
            "-query-threads" if index + 1 < args.len() => {
                index += 1;
                options.query_threads =
                    parse_u64(&args[index], "-query-threads must be a positive integer")? as usize;
                if options.query_threads == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "-query-threads must be a positive integer",
                    ));
                }
            }
            "-enqueue" => options.use_enqueue = true,
            option => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("Unknown BEIR eval option: {option}"),
                ))
            }
        }
        index += 1;
    }
    if options.data_path.as_os_str().is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Usage: moon [-idx <index>] -beir-eval -data <beir-dir> [-qrels test] [-k 10,100,1000]",
        ));
    }
    if options.streams.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "-streams must not be empty",
        ));
    }
    Ok(options)
}

struct ExternalVectorStream {
    input: BufReader<File>,
    binary: bool,
    id_bytes: usize,
}

fn parse_external_vector(text: &str) -> Option<Vec<f32>> {
    let mut vector = vec![0.0; DOC_VECTOR_DIM];
    for (slot, piece) in text
        .split(',')
        .filter(|piece| !piece.is_empty())
        .take(DOC_VECTOR_DIM)
        .enumerate()
    {
        vector[slot] = piece.parse().ok()?;
    }
    let norm = vector.iter().map(|value| value * value).sum::<f32>().sqrt();
    if norm <= 0.0 {
        return None;
    }
    for value in &mut vector {
        *value /= norm;
    }
    Some(vector)
}

fn open_external_vector_stream(path: &Path) -> io::Result<ExternalVectorStream> {
    let mut input = BufReader::new(File::open(path)?);
    let mut magic = [0u8; 8];
    if input.read_exact(&mut magic).is_ok() && &magic == b"MSVECI81" {
        let mut value = [0u8; 4];
        input.read_exact(&mut value)?;
        let dim = u32::from_le_bytes(value) as usize;
        input.read_exact(&mut value)?;
        let id_bytes = u32::from_le_bytes(value) as usize;
        if dim != DOC_VECTOR_DIM || id_bytes == 0 || id_bytes > 1024 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid external vector header",
            ));
        }
        return Ok(ExternalVectorStream {
            input,
            binary: true,
            id_bytes,
        });
    }
    input.seek(SeekFrom::Start(0))?;
    Ok(ExternalVectorStream {
        input,
        binary: false,
        id_bytes: 0,
    })
}

fn read_external_vector_record(
    stream: &mut ExternalVectorStream,
) -> io::Result<Option<(String, Vec<f32>)>> {
    if stream.binary {
        let mut id = vec![0u8; stream.id_bytes];
        match stream.input.read_exact(&mut id) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
            Err(error) => return Err(error),
        }
        let mut encoded = vec![0u8; DOC_VECTOR_DIM];
        stream.input.read_exact(&mut encoded)?;
        let end = id.iter().position(|value| *value == 0).unwrap_or(id.len());
        let id = String::from_utf8_lossy(&id[..end]).into_owned();
        if id.is_empty() {
            return Ok(None);
        }
        return Ok(Some((
            id,
            encoded
                .into_iter()
                .map(|value| value as i8 as f32 / 128.0)
                .collect(),
        )));
    }
    let mut line = String::new();
    loop {
        line.clear();
        if stream.input.read_line(&mut line)? == 0 {
            return Ok(None);
        }
        let line = line.trim_end_matches(['\r', '\n']);
        let Some((id, values)) = line.split_once('\t') else {
            continue;
        };
        if id.is_empty() {
            continue;
        }
        if let Some(vector) = parse_external_vector(values) {
            return Ok(Some((id.to_string(), vector)));
        }
    }
}

fn json_string(value: &serde_json::Value, key: &str) -> Option<String> {
    value.get(key)?.as_str().map(str::to_string)
}

fn beir_file(data_path: &Path, relative: impl AsRef<Path>) -> PathBuf {
    data_path.join(relative)
}

fn run_beir_build(index: &Path, options: &BeirBuildOptions) -> io::Result<()> {
    let corpus_path = beir_file(&options.data_path, "corpus.jsonl");
    let corpus = BufReader::new(File::open(&corpus_path).map_err(|_| {
        io::Error::new(
            io::ErrorKind::NotFound,
            format!("BEIR corpus not found: {}", corpus_path.display()),
        )
    })?);
    let mut vectors = options
        .doc_vectors_path
        .as_deref()
        .map(open_external_vector_stream)
        .transpose()?;
    if let Some(path) = &options.doc_vectors_path {
        println!("  streaming external document vectors: {}", path.display());
    }
    let _ = fs::remove_file(index);
    let _ = fs::remove_file(delta_index_path(index));
    let mut context = IndexContext::new();
    let mut doc_id = 0u64;
    let mut skipped = 0u64;
    let mut vector_docs = 0u64;
    let started = Instant::now();
    for line in corpus.lines() {
        if options.limit > 0 && doc_id >= options.limit {
            break;
        }
        let Ok(value) = serde_json::from_str::<serde_json::Value>(&line?) else {
            skipped += 1;
            continue;
        };
        let (Some(id), Some(body)) = (json_string(&value, "_id"), json_string(&value, "text"))
        else {
            skipped += 1;
            continue;
        };
        let doc = Document {
            doc_id,
            path: id.clone(),
            url: json_string(&value, "url").unwrap_or_default(),
            title: json_string(&value, "title").unwrap_or_default(),
            body,
            importance: 0.1,
            ..Document::default()
        };
        context.AddDocument(&doc, options.build_vectors && vectors.is_none());
        if let Some(stream) = vectors.as_mut() {
            let Some((vector_id, vector)) = read_external_vector_record(stream)? else {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    format!("Document vector file ended before corpus doc {id}"),
                ));
            };
            if vector_id != id {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Document vector id mismatch: expected {id} got {vector_id}"),
                ));
            }
            context.GetWriter().SetDocVector(doc_id, vector);
            vector_docs += 1;
        }
        doc_id += 1;
        if doc_id % 1000 == 0 {
            println!("  BEIR indexed {doc_id} docs");
        }
    }
    if doc_id == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "BEIR corpus had no readable docs: {}",
                corpus_path.display()
            ),
        ));
    }
    println!("  writing BEIR index: {}", index.display());
    context
        .SaveIndex(&index.to_string_lossy())
        .map_err(rust_error)?;
    print!("BEIR build complete docs={doc_id}");
    if vectors.is_some() || options.build_vectors {
        print!(" vector_docs={vector_docs}");
    }
    if skipped > 0 {
        print!(" skipped={skipped}");
    }
    println!(" elapsed_ms={}", started.elapsed().as_millis());
    Ok(())
}

fn read_index_header(path: &Path) -> io::Result<IndexFileHeader> {
    let mut input = File::open(path)?;
    let mut bytes = [0u8; INDEX_FILE_HEADER_SIZE];
    input.read_exact(&mut bytes)?;
    IndexFileHeader::parse(&bytes).map_err(rust_error)
}

fn run_beir_patch_vectors(index: &Path, options: &BeirPatchVectorOptions) -> io::Result<()> {
    let header = read_index_header(&options.source_index_path).map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "Source index not found or invalid: {}",
                options.source_index_path.display()
            ),
        )
    })?;
    if index == options.source_index_path {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "Output index must differ from source index for vector patching",
        ));
    }
    fs::copy(&options.source_index_path, index)?;
    let mut vectors = open_external_vector_stream(&options.doc_vectors_path)?;
    let mut output = OpenOptions::new().read(true).write(true).open(index)?;
    let dim_offset = std::mem::offset_of!(DocDataEntry, DDE_VectorDim) as u64;
    let format_offset = std::mem::offset_of!(DocDataEntry, DDE_VectorFormat) as u64;
    let data_offset = std::mem::offset_of!(DocDataEntry, DDE_VectorData) as u64;
    let mut patched = 0u64;
    for doc_id in 0..header.IFH_NumDocuments {
        if options.limit > 0 && patched >= options.limit {
            break;
        }
        let Some((_vector_id, vector)) = read_external_vector_record(&mut vectors)? else {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                format!("Vector file ended before docId {doc_id}"),
            ));
        };
        let entry_offset = header.IFH_DocDataOffset + doc_id * DOC_REC_SIZE as u64;
        output.seek(SeekFrom::Start(entry_offset + dim_offset))?;
        output.write_all(&(DOC_VECTOR_DIM as u16).to_le_bytes())?;
        output.seek(SeekFrom::Start(entry_offset + format_offset))?;
        output.write_all(&1u16.to_le_bytes())?;
        let quantized: Vec<u8> = vector
            .iter()
            .take(DOC_VECTOR_DIM)
            .map(|value| (value * 128.0).clamp(-128.0, 127.0).round() as i8 as u8)
            .collect();
        output.seek(SeekFrom::Start(entry_offset + data_offset))?;
        output.write_all(&quantized)?;
        patched += 1;
        if patched % 100_000 == 0 {
            println!("  patched {patched} vectors");
        }
    }
    println!(
        "BEIR vector patch complete docs={patched} output={}",
        index.display()
    );
    Ok(())
}

fn load_beir_qrels(path: &Path) -> io::Result<HashMap<String, HashSet<String>>> {
    let mut qrels: HashMap<String, HashSet<String>> = HashMap::new();
    for (index, line) in BufReader::new(File::open(path)?).lines().enumerate() {
        let line = line?;
        if index == 0 && (line.contains("query-id") || line.contains("corpus-id")) {
            continue;
        }
        let columns: Vec<_> = line.split('\t').collect();
        if columns.len() < 3 || columns[2].parse::<f64>().unwrap_or(0.0) <= 0.0 {
            continue;
        }
        qrels
            .entry(columns[0].to_string())
            .or_default()
            .insert(columns[1].to_string());
    }
    Ok(qrels)
}

fn load_external_vectors(path: &Path) -> io::Result<HashMap<String, Vec<f32>>> {
    let mut stream = open_external_vector_stream(path)?;
    let mut vectors = HashMap::new();
    while let Some((id, vector)) = read_external_vector_record(&mut stream)? {
        vectors.entry(id).or_insert(vector);
    }
    Ok(vectors)
}

fn beir_compile_mode(mode: &str) -> QueryCompileMode {
    match mode {
        "weakandbigramboost" | "hybridboost" => QueryCompileMode::WeakAndBigramBoost,
        "weakandbigramboostdoc" | "hybridboostdoc" => QueryCompileMode::WeakAndBigramBoostForDoc,
        _ => QueryCompileMode::WeakAndBigram,
    }
}

fn is_hybrid_mode(mode: &str) -> bool {
    matches!(mode, "hybrid" | "hybridboost" | "hybridboostdoc")
}
fn is_weak_mode(mode: &str) -> bool {
    matches!(
        mode,
        "weakandbigram" | "weakandbigramboost" | "weakandbigramboostdoc"
    )
}

fn build_beir_bow_reader(
    context: &mut IndexContext,
    tokenizer: &SmartTokenizer,
    query: &str,
    stream_set: &str,
) -> Box<dyn IndexReader> {
    let mut streams: Vec<_> = stream_set
        .chars()
        .filter(|stream| matches!(stream, 'A' | 'U' | 'T' | 'B' | 'M'))
        .collect();
    if streams.is_empty() {
        streams.push('T');
    }
    let tokens = tokenizer.Tokenize(query);
    let stopwords: HashSet<&str> = [
        "a", "an", "and", "are", "as", "at", "be", "been", "by", "for", "from", "has", "have",
        "in", "into", "is", "it", "its", "of", "on", "or", "that", "the", "their", "there",
        "these", "this", "to", "was", "were", "with", "without", "can", "could", "may", "might",
        "must", "should", "than", "then", "which", "while", "during", "between", "within", "using",
        "used", "use",
    ]
    .into_iter()
    .collect();
    let mut keys = Vec::new();
    for token in tokens
        .iter()
        .filter(|token| token.len() > 1 && !stopwords.contains(token.as_str()))
    {
        for stream in &streams {
            keys.push(format!("{token}{stream}"));
        }
    }
    if keys.is_empty() {
        for token in tokens.iter().filter(|token| !token.is_empty()) {
            for stream in &streams {
                keys.push(format!("{token}{stream}"));
            }
        }
    }
    keys.sort();
    keys.dedup();
    let root = EvalNode::Or(OrNode {
        children: keys
            .into_iter()
            .map(|key| EvalNode::Term(TermNode::new(key)))
            .collect(),
    });
    context.GetReader(EvalTree::new(Some(root)))
}

#[derive(Default)]
struct CandidateFeature {
    doc_id_text: String,
    weak_score: f32,
    bigram_score: f32,
    weak_source: u8,
    bigram_source: u8,
}

fn add_feature_rows(
    context: &IndexContext,
    rows: &mut HashMap<u64, CandidateFeature>,
    mut reader: Box<dyn IndexReader>,
    bigram: bool,
) {
    while !reader.IsEnd() {
        let doc_id = reader.GetDocumentID();
        if let Some(entry) = context.GetDocDataEntry(doc_id) {
            let score = reader.GetScore(entry);
            let source = reader.GetSourceMask();
            let row = rows.entry(ReaderDocumentIDValue(doc_id)).or_default();
            if row.doc_id_text.is_empty() {
                row.doc_id_text = context.GetDocPath(doc_id);
            }
            if bigram {
                row.bigram_score = row.bigram_score.max(score);
                row.bigram_source |= source;
            } else {
                row.weak_score = row.weak_score.max(score);
                row.weak_source |= source;
            }
        }
        reader.GoNext();
    }
}

fn collect_candidate_features(
    context: &mut IndexContext,
    tree: &EvalTree,
) -> HashMap<u64, CandidateFeature> {
    let (weak, bigram) = match tree.root.as_ref() {
        Some(EvalNode::Or(node)) => (
            node.children.first().cloned(),
            node.children.get(1).cloned(),
        ),
        root => (root.cloned(), None),
    };
    let mut rows = HashMap::new();
    if let Some(root) = weak {
        let reader = context.GetReader(EvalTree::new(Some(root)));
        add_feature_rows(context, &mut rows, reader, false);
    }
    if let Some(root) = bigram {
        let reader = context.GetReader(EvalTree::new(Some(root)));
        add_feature_rows(context, &mut rows, reader, true);
    }
    rows
}

fn moon_doc_prior(entry: &DocDataEntry) -> f32 {
    let body_length = entry.DDE_BodyLength;
    let quality = entry.DDE_QualityScore;
    let authority = entry.DDE_AuthorityScore;
    let spam = entry.DDE_SpamScore;
    let length_quality = (1.0 - ((body_length.max(1) as f32).log2() - 6.0).abs() / 4.0).max(0.0);
    0.15 * length_quality
        + 0.10 * DocDataDecodeScore(quality)
        + 0.05 * DocDataDecodeScore(authority)
        - 0.10 * DocDataDecodeScore(spam)
}

fn write_feature_rows(
    output: &mut dyn Write,
    context: &IndexContext,
    qrels: &HashMap<String, HashSet<String>>,
    qid: &str,
    rows: HashMap<u64, CandidateFeature>,
) -> io::Result<()> {
    let mut doc_ids: Vec<_> = rows.keys().copied().collect();
    doc_ids.sort_unstable();
    for doc_id in doc_ids {
        let row = &rows[&doc_id];
        let Some(entry) = context.GetDocDataEntry(doc_id) else {
            continue;
        };
        if row.doc_id_text.is_empty() {
            continue;
        }
        let label = qrels
            .get(qid)
            .map(|docs| docs.contains(&row.doc_id_text))
            .unwrap_or(false) as u8;
        let static_rank = entry.DDE_StaticRank;
        let body_length = entry.DDE_BodyLength;
        let quality = entry.DDE_QualityScore;
        let authority = entry.DDE_AuthorityScore;
        let spam = entry.DDE_SpamScore;
        let title_length = entry.DDE_TitleLength;
        let diversity = entry.DDE_DiversityScore;
        let length_quality = entry.DDE_LengthQualityScore;
        writeln!(
            output,
            "{qid}\t{}\t{label}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            row.doc_id_text,
            row.weak_score,
            row.bigram_score,
            (row.weak_score != 0.0) as u8,
            (row.bigram_score != 0.0) as u8,
            row.weak_source,
            row.bigram_source,
            DocDataDecodeScore(static_rank),
            moon_doc_prior(entry),
            body_length,
            DocDataDecodeScore(quality),
            DocDataDecodeScore(authority),
            DocDataDecodeScore(spam),
            title_length,
            body_length,
            diversity,
            length_quality
        )?;
    }
    Ok(())
}

fn add_recall(
    context: &IndexContext,
    results: &[SearchResult],
    relevant: &HashSet<String>,
    at: &[usize],
    macro_recall: &mut [f64],
    micro_hits: &mut [u64],
    micro_relevant: &mut u64,
) {
    let mut hits = 0u64;
    let mut next_at = 0;
    for (rank, result) in results.iter().enumerate() {
        if relevant.contains(&context.GetDocPath(result.doc_id)) {
            hits += 1;
        }
        while next_at < at.len() && rank + 1 == at[next_at] {
            micro_hits[next_at] += hits;
            macro_recall[next_at] += hits as f64 / relevant.len() as f64;
            next_at += 1;
        }
    }
    while next_at < at.len() {
        micro_hits[next_at] += hits;
        macro_recall[next_at] += hits as f64 / relevant.len() as f64;
        next_at += 1;
    }
    *micro_relevant += relevant.len() as u64;
}

fn print_block_stats(context: &IndexContext) {
    if std::env::var_os("MOONSHOT_BLOCK_STATS").is_none() {
        return;
    }
    let stats = context.GetBlockAccessStats();
    println!("BlockAccess direct_gets={} direct_releases={} worker_gets={} worker_releases={} cache_hits={} cache_misses={} disk_reads={}",
        stats.DirectGets, stats.DirectReleases, stats.WorkerGets, stats.WorkerReleases, stats.CacheHits, stats.CacheMisses, stats.DiskReads);
    let io_stats = FileAccess::GetIoStats();
    if io_stats.IoUringReads != 0
        || io_stats.PreadFallbackReads != 0
        || io_stats.IoUringSetupOk != 0
        || io_stats.IoUringSetupFailed != 0
    {
        println!("FileAccess io_uring_reads={} pread_fallback_reads={} io_uring_setup_ok={} io_uring_setup_failed={}",
            io_stats.IoUringReads, io_stats.PreadFallbackReads, io_stats.IoUringSetupOk, io_stats.IoUringSetupFailed);
    }
}

fn run_beir_eval(index: &Path, options: &BeirEvalOptions) -> io::Result<()> {
    if !IndexSerializer::IsValidIndex(&index.to_string_lossy()) {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("BEIR index not found or invalid: {}", index.display()),
        ));
    }
    let query_path = beir_file(&options.data_path, "queries.jsonl");
    let qrels_path = if Path::new(&options.qrels).is_file() {
        PathBuf::from(&options.qrels)
    } else {
        beir_file(&options.data_path, format!("qrels/{}.tsv", options.qrels))
    };
    let qrels = load_beir_qrels(&qrels_path)?;
    if qrels.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("BEIR qrels not found or empty: {}", qrels_path.display()),
        ));
    }
    let query_vectors = options
        .query_vectors_path
        .as_deref()
        .map(load_external_vectors)
        .transpose()?
        .unwrap_or_default();
    if let Some(path) = &options.query_vectors_path {
        println!("  loaded external query vectors: {}", query_vectors.len());
        println!("  source: {}", path.display());
    }
    let header = read_index_header(index)?;
    let leaf_cache_bytes = if options.leaf_cache_mb > 0 {
        options.leaf_cache_mb * 1024 * 1024
    } else if options.leaf_cache_match_mphf {
        LEAF_TERM_CACHE_BYTES
            + header.IFH_TermMphfHeaderCount * TERM_MPHF_HEADER_SIZE as u64
            + header.IFH_TermMphfDisplacementCount * 4
            + header.IFH_TermMphfEntryPageCount * PAGE_SIZE as u64
    } else {
        0
    };
    let mut context = IndexContext::new();
    if leaf_cache_bytes > 0 {
        context.SetLeafTermCacheBytes(leaf_cache_bytes);
    }
    context
        .LoadIndex(&index.to_string_lossy())
        .map_err(rust_error)?;
    context.SetTermMphfEnabled(!options.no_mphf);
    context.SetDirectBlockAccessEnabled(
        std::env::var("MOONSHOT_BLOCK_ACCESS").as_deref() != Ok("worker"),
    );
    let compile_mode = beir_compile_mode(&options.mode);
    let parameters = *GetQueryCompileModeParameters(compile_mode);
    context.SetQueryParameters(parameters);
    rustblade::index_search_executor::IndexSearchExecutor::SetScoringParameters(parameters);
    context.SetWeakAndBuildMode(match options.weak_and_shape.as_str() {
        "or" => WeakAndBuildMode::OrChildren,
        "or-prune" => WeakAndBuildMode::OrChildrenPruned,
        _ => WeakAndBuildMode::FlatPruned,
    });
    let max_k = *options.at.iter().max().unwrap();
    let mut queries = Vec::new();
    let mut missing_qrels = 0u64;
    for line in BufReader::new(File::open(&query_path)?).lines() {
        let Ok(value) = serde_json::from_str::<serde_json::Value>(&line?) else {
            continue;
        };
        let (Some(qid), Some(query)) = (json_string(&value, "_id"), json_string(&value, "text"))
        else {
            continue;
        };
        let Some(relevant) = qrels.get(&qid) else {
            missing_qrels += 1;
            continue;
        };
        if relevant.is_empty() {
            missing_qrels += 1;
            continue;
        }
        if options.limit > 0 && queries.len() as u64 >= options.limit {
            break;
        }
        queries.push((qid, query));
    }
    let mut run_output: Option<BufWriter<File>> = options
        .run_out
        .as_ref()
        .map(File::create)
        .transpose()?
        .map(BufWriter::new);
    let mut feature_output: Option<BufWriter<File>> = options
        .dump_features_path
        .as_ref()
        .map(File::create)
        .transpose()?
        .map(BufWriter::new);
    if feature_output.is_some() && options.mode != "weakandbigram" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "-dump-features currently supports -mode weakandbigram only",
        ));
    }
    if let Some(output) = feature_output.as_mut() {
        writeln!(output, "qid\tdocid\tlabel\tweak_score\tbigram_score\tweak_hit\tbigram_hit\tweak_source\tbigram_source\tstatic_rank\tdoc_prior\tdoc_len\tquality\tauthority\tspam\ttitle_len\tbody_len\tdiversity\tlength_quality")?;
    }
    let started = Instant::now();
    let mut macro_recall = vec![0.0; options.at.len()];
    let mut micro_hits = vec![0u64; options.at.len()];
    let mut micro_relevant = 0u64;
    let mut evaluated = 0u64;
    let tokenizer = SmartTokenizer::new();
    if options.query_threads > 1 && !options.use_enqueue && feature_output.is_none() {
        return Err(io::Error::new(io::ErrorKind::Unsupported,
            "RustBlade does not expose a shareable independent-reader context for non-queued concurrent BEIR evaluation; use -enqueue or -query-threads 1"));
    }
    if options.query_threads > 1 && options.use_enqueue && feature_output.is_none() {
        if options.mode == "bow" {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "-enqueue is supported for weakand/vector/hybrid BEIR modes only",
            ));
        }
        let mut tasks = Vec::new();
        for (qid, query) in queries {
            let vector = query_vectors
                .get(&qid)
                .cloned()
                .or_else(|| {
                    query_vectors
                        .is_empty()
                        .then(|| context.CompileToVector(&query))
                })
                .unwrap_or_default();
            let query_text = if options.mode == "vector" { "" } else { &query };
            let task = context.EnqueueWithMode(
                query_text,
                if options.mode == "vector" || is_hybrid_mode(&options.mode) {
                    vector
                } else {
                    Vec::new()
                },
                &options.streams,
                max_k as i32,
                compile_mode,
                options.vector_ef,
            );
            tasks.push((qid, task));
        }
        for (qid, task) in tasks {
            let results = task.Wait();
            if let Some(output) = run_output.as_mut() {
                for (rank, result) in results.iter().enumerate() {
                    let doc_id = context.GetDocPath(result.doc_id);
                    if !doc_id.is_empty() {
                        writeln!(
                            output,
                            "{qid} Q0 {doc_id} {} {:.9} moon-{}",
                            rank + 1,
                            result.score,
                            options.mode
                        )?;
                    }
                }
            }
            add_recall(
                &context,
                &results,
                &qrels[&qid],
                &options.at,
                &mut macro_recall,
                &mut micro_hits,
                &mut micro_relevant,
            );
            evaluated += 1;
            if evaluated % 100 == 0 {
                println!("  BEIR evaluated {evaluated} queries");
            }
        }
    } else {
        for (qid, query) in queries {
            if let Some(output) = feature_output.as_mut() {
                let tree = context.CompileWithMode(
                    &query,
                    &options.streams,
                    QueryCompileMode::WeakAndBigram,
                );
                let rows = collect_candidate_features(&mut context, &tree);
                write_feature_rows(output, &context, &qrels, &qid, rows)?;
                evaluated += 1;
                continue;
            }
            let vector = query_vectors
                .get(&qid)
                .cloned()
                .or_else(|| {
                    query_vectors
                        .is_empty()
                        .then(|| context.CompileToVector(&query))
                })
                .unwrap_or_default();
            let results = if options.use_enqueue {
                if options.mode == "bow" {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        "-enqueue is supported for weakand/vector/hybrid BEIR modes only",
                    ));
                }
                let query_text = if options.mode == "vector" { "" } else { &query };
                context
                    .EnqueueWithMode(
                        query_text,
                        if options.mode == "vector" || is_hybrid_mode(&options.mode) {
                            vector
                        } else {
                            Vec::new()
                        },
                        &options.streams,
                        max_k as i32,
                        compile_mode,
                        options.vector_ef,
                    )
                    .Wait()
            } else if options.mode == "bow" {
                let mut reader =
                    build_beir_bow_reader(&mut context, &tokenizer, &query, &options.streams);
                context.GetExecutor().Execute(reader.as_mut(), max_k as i32)
            } else if is_weak_mode(&options.mode) {
                let tree = context.CompileWithMode(&query, &options.streams, compile_mode);
                let vector_query = (tree.HasTextQuery() && tree.HasVectorQuery())
                    .then(|| tree.vector_query.clone());
                let mut reader = context.GetReader(tree);
                context.GetExecutor().ExecuteWithVector(
                    reader.as_mut(),
                    max_k as i32,
                    vector_query.as_deref(),
                )
            } else if options.mode == "vector" || is_hybrid_mode(&options.mode) {
                let mut tree = if is_hybrid_mode(&options.mode) {
                    context.CompileWithMode(&query, &options.streams, compile_mode)
                } else {
                    EvalTree::empty()
                };
                tree.vector_query = vector;
                tree.vector_ef_search = options.vector_ef;
                let vector_query = (tree.HasTextQuery() && tree.HasVectorQuery())
                    .then(|| tree.vector_query.clone());
                let mut reader = context.GetReader(tree);
                context.GetExecutor().ExecuteWithVector(
                    reader.as_mut(),
                    max_k as i32,
                    vector_query.as_deref(),
                )
            } else {
                let mut reader = context.GetReaderForQuery(&query, &options.streams);
                context.GetExecutor().Execute(reader.as_mut(), max_k as i32)
            };
            if let Some(output) = run_output.as_mut() {
                for (rank, result) in results.iter().enumerate() {
                    let doc_id = context.GetDocPath(result.doc_id);
                    if !doc_id.is_empty() {
                        writeln!(
                            output,
                            "{qid} Q0 {doc_id} {} {:.9} moon-{}",
                            rank + 1,
                            result.score,
                            options.mode
                        )?;
                    }
                }
            }
            add_recall(
                &context,
                &results,
                &qrels[&qid],
                &options.at,
                &mut macro_recall,
                &mut micro_hits,
                &mut micro_relevant,
            );
            evaluated += 1;
            if evaluated % 100 == 0 {
                println!("  BEIR evaluated {evaluated} queries");
            }
        }
    }
    if feature_output.is_some() {
        println!("BEIR feature dump index={} data={} qrels={} streams={} mode={} bigram_weight={} queries={} output={}",
            index.display(), options.data_path.display(), qrels_path.display(), options.streams, options.mode, parameters.QMP_BigramWeight,
            evaluated, options.dump_features_path.as_ref().unwrap().display());
        return Ok(());
    }
    if evaluated == 0 || micro_relevant == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "No BEIR queries with qrels were evaluated",
        ));
    }
    println!("BEIR eval index={} data={} qrels={} streams={} mode={} weakand_shape={} bigram_weight={} mphf={} leaf_cache_mb={} query_threads={}{} queries={} missing_qrels={} elapsed_ms={}",
        index.display(), options.data_path.display(), qrels_path.display(), options.streams, options.mode, options.weak_and_shape,
        parameters.QMP_BigramWeight, if options.no_mphf { "off" } else { "on" },
        if leaf_cache_bytes > 0 { leaf_cache_bytes / (1024 * 1024) } else { LEAF_TERM_CACHE_BYTES / (1024 * 1024) },
        options.query_threads, if options.use_enqueue { " enqueue=on" } else { "" }, evaluated, missing_qrels, started.elapsed().as_millis());
    print_block_stats(&context);
    for index in 0..options.at.len() {
        println!(
            "Recall@{} macro={:.4} micro={:.4} hits={}/{}",
            options.at[index],
            macro_recall[index] / evaluated as f64,
            micro_hits[index] as f64 / micro_relevant as f64,
            micro_hits[index],
            micro_relevant
        );
    }
    Ok(())
}

fn has_search_results(context: &mut IndexContext, query: &str) -> bool {
    if query.is_empty() || context.DocumentCount() == 0 {
        return false;
    }
    let tree = context.Compile(query, "AUTBV");
    if tree.IsEmpty() {
        return false;
    }
    let mut reader = context.GetReader(tree);
    !context.GetExecutor().Execute(reader.as_mut(), 5).is_empty()
}

fn sample_merge(args: &[String]) -> io::Result<()> {
    let mut directory = None;
    let mut output = None;
    let mut extensions = parse_extensions("cpp,h,rs");
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "-dir" if index + 1 < args.len() => {
                index += 1;
                directory = Some(PathBuf::from(&args[index]));
            }
            "-out" if index + 1 < args.len() => {
                index += 1;
                output = Some(PathBuf::from(&args[index]));
            }
            "-ext" if index + 1 < args.len() => {
                index += 1;
                extensions = parse_extensions(&args[index]);
            }
            option => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("Unknown sample merge option: {option}"),
                ))
            }
        }
        index += 1;
    }
    let directory = directory.ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "-sample-merge requires -dir")
    })?;
    let output = absolute_path(output.ok_or_else(|| {
        io::Error::new(io::ErrorKind::InvalidInput, "-sample-merge requires -out")
    })?);
    let mut files = collect_files(&directory, &extensions, true, false)?;
    files.sort();
    if files.len() < 2 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "sample-merge needs at least two readable files",
        ));
    }
    let base_path = PathBuf::from(format!("{}.base.tmp", output.display()));
    let delta_path = delta_index_path(&base_path);
    for path in [&output, &base_path, &delta_path] {
        let _ = fs::remove_file(path);
    }
    let split = files.len().div_ceil(2);
    let mut base = IndexContext::new();
    let (base_kept, _) = add_files(&mut base, &files[..split], 0, false, true, None);
    if base_kept == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "sample-merge: failed to save base index: {}",
                base_path.display()
            ),
        ));
    }
    base.SaveIndex(&base_path.to_string_lossy())
        .map_err(rust_error)?;
    let mut delta = IndexContext::with_path_and_load_delta(
        Some(base_path.to_string_lossy().into_owned()),
        false,
    );
    let (delta_kept, _) = add_files(&mut delta, &files[split..], base_kept, false, true, None);
    if delta_kept == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "sample-merge: failed to save delta index: {}",
                delta_path.display()
            ),
        ));
    }
    delta
        .SaveIndex(&delta_path.to_string_lossy())
        .map_err(rust_error)?;
    let mut merged = IndexContext::with_path(Some(base_path.to_string_lossy().into_owned()));
    merged
        .Merge(&output.to_string_lossy())
        .map_err(rust_error)?;
    let mut verify =
        IndexContext::with_path_and_load_delta(Some(output.to_string_lossy().into_owned()), false);
    if verify.DocumentCount() != base_kept + delta_kept {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "sample-merge verification failed",
        ));
    }
    let first_content = files
        .iter()
        .find_map(|path| fs::read_to_string(path).ok())
        .unwrap_or_default();
    let fallback = SmartTokenizer::new()
        .Tokenize(&first_content)
        .into_iter()
        .find(|token| !token.is_empty())
        .unwrap_or_default();
    let sanity_query = ["include", "class", fallback.as_str()]
        .into_iter()
        .find(|query| has_search_results(&mut verify, query))
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "sample-merge: verification failed, no search results for sanity query",
            )
        })?;
    println!(
        "sample-merge: verified docs={} query=\"{sanity_query}\"\nsample-merge: success",
        verify.DocumentCount()
    );
    Ok(())
}

fn run() -> io::Result<()> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut index_path_value = default_idx_path();
    let mut filtered = Vec::new();
    let mut position = 0;
    while position < args.len() {
        if args[position] == "-idx" {
            if position + 1 >= args.len() {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "Usage: moon -idx <index-path> <command>",
                ));
            }
            position += 1;
            index_path_value = absolute_path(&args[position]);
        } else {
            filtered.push(args[position].clone());
        }
        position += 1;
    }
    args = filtered;
    let command = args.first().map(String::as_str).unwrap_or("");
    match command {
        "" | "-h" | "--help" | "help" => {
            usage();
            Ok(())
        }
        "-sample-merge" => sample_merge(&args),
        "-beir-build" => run_beir_build(&index_path_value, &parse_beir_build_options(&args)?),
        "-beir-patch-vectors" => {
            run_beir_patch_vectors(&index_path_value, &parse_beir_patch_options(&args)?)
        }
        "-beir-eval" => run_beir_eval(&index_path_value, &parse_beir_eval_options(&args)?),
        "-i" | "-v" => interactive(&index_path_value, parse_search_options(&args)?),
        "-file" | "-dir" | "-ext" | "-b" | "-r" => {
            let mut file = None;
            let mut directory = None;
            let mut extensions = parse_extensions("md,txt");
            let mut recursive = false;
            let mut batch_size = 10_000usize;
            let mut bge_options = SearchOptions::default();
            let mut index = 0;
            while index < args.len() {
                match args[index].as_str() {
                    "-file" if index + 1 < args.len() => {
                        index += 1;
                        file = Some(PathBuf::from(&args[index]));
                    }
                    "-dir" if index + 1 < args.len() => {
                        index += 1;
                        directory = Some(PathBuf::from(&args[index]));
                    }
                    "-ext" if index + 1 < args.len() => {
                        index += 1;
                        extensions = parse_extensions(&args[index]);
                        if extensions.is_empty() {
                            return Err(io::Error::new(
                                io::ErrorKind::InvalidInput,
                                "-ext must include at least one extension",
                            ));
                        }
                    }
                    "-b" if index + 1 < args.len() => {
                        index += 1;
                        batch_size = args[index]
                            .parse()
                            .ok()
                            .filter(|value| *value >= 10_000)
                            .ok_or_else(|| {
                                io::Error::new(
                                    io::ErrorKind::InvalidInput,
                                    "-b must be at least 10000 for indexing performance",
                                )
                            })?;
                    }
                    "-r" => recursive = true,
                    "-bge" => bge_options.bge = true,
                    "-bge-sidecar" => bge_options.bge_sidecar = true,
                    "-bge-python" if index + 1 < args.len() => {
                        index += 1;
                        bge_options.bge_python = args[index].clone();
                    }
                    "-bge-script" if index + 1 < args.len() => {
                        index += 1;
                        bge_options.bge_script = args[index].clone();
                    }
                    "-bge-model" if index + 1 < args.len() => {
                        index += 1;
                        bge_options.bge_model = args[index].clone();
                    }
                    "-bge-host" if index + 1 < args.len() => {
                        index += 1;
                        bge_options.bge_host = args[index].clone();
                    }
                    "-bge-port" if index + 1 < args.len() => {
                        index += 1;
                        bge_options.bge_port = args[index]
                            .parse()
                            .ok()
                            .filter(|value| *value > 0)
                            .ok_or_else(|| {
                                io::Error::new(
                                    io::ErrorKind::InvalidInput,
                                    "-bge-port must be 1..65535",
                                )
                            })?;
                    }
                    option => {
                        return Err(io::Error::new(
                            io::ErrorKind::InvalidInput,
                            format!("Unknown option: {option}"),
                        ))
                    }
                }
                index += 1;
            }
            if file.is_some() == directory.is_some() {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "Use either -file or -dir, not both",
                ));
            }
            let input = file.as_ref().or(directory.as_ref()).unwrap();
            index_path(
                &index_path_value,
                input,
                &extensions,
                recursive,
                file.is_some(),
                batch_size,
                bge_options.bge.then_some(&bge_options),
            )
        }
        option => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("Unknown option: {option}"),
        )),
    }
}

fn main() {
    let result = run();
    if let Err(error) = result {
        eprintln!("moon: {error}");
        std::process::exit(1);
    }
}
