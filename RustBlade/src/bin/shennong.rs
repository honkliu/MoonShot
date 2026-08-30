//! Shennong service translation; cross-language API names intentionally match the C++ service.
#![allow(non_snake_case, non_upper_case_globals)]

use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Instant;

use rustblade::IndexContext;

struct Options {
    port: u16,
    index_path: String,
    gbe_host: String,
    gbe_port: u16,
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

fn default_index_path() -> String {
    home_dir().join("moon.idx").to_string_lossy().to_string()
}

fn parse_args() -> Result<Options, String> {
    let mut options = Options {
        port: 9000,
        index_path: default_index_path(),
        gbe_host: "127.0.0.1".to_string(),
        gbe_port: 8765,
    };
    let args: Vec<String> = std::env::args().collect();
    let mut index = 1usize;
    while index < args.len() {
        match args[index].as_str() {
            "--port" if index + 1 < args.len() => {
                options.port = args[index + 1]
                    .parse::<u16>()
                    .map_err(|_| "invalid --port value".to_string())?;
                index += 2;
            }
            "--index" if index + 1 < args.len() => {
                options.index_path = expand_user_path(&args[index + 1]);
                index += 2;
            }
            "--gbe-host" | "--bge-host" if index + 1 < args.len() => {
                options.gbe_host = args[index + 1].clone();
                index += 2;
            }
            "--gbe-port" | "--bge-port" if index + 1 < args.len() => {
                options.gbe_port = args[index + 1]
                    .parse::<u16>()
                    .ok()
                    .filter(|value| *value > 0)
                    .ok_or_else(|| format!("invalid {} value", args[index]))?;
                index += 2;
            }
            "--help" | "-h" => {
                println!("usage: shennong [--port 9000] [--index ~/moon.idx] [--gbe-host 127.0.0.1] [--gbe-port 8765]");
                std::process::exit(0);
            }
            other => return Err(format!("unknown or incomplete argument: {other}")),
        }
    }
    Ok(options)
}

fn expand_user_path(path: &str) -> String {
    if path == "~" {
        return home_dir().to_string_lossy().to_string();
    }
    if let Some(rest) = path.strip_prefix("~/").or_else(|| path.strip_prefix("~\\")) {
        return home_dir().join(rest).to_string_lossy().to_string();
    }
    path.to_string()
}

fn url_decode(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut index = 0usize;
    while index < bytes.len() {
        match bytes[index] {
            b'+' => {
                out.push(b' ');
                index += 1;
            }
            b'%' if index + 2 < bytes.len() => {
                let hex = &input[index + 1..index + 3];
                if let Ok(value) = u8::from_str_radix(hex, 16) {
                    out.push(value);
                    index += 3;
                } else {
                    out.push(bytes[index]);
                    index += 1;
                }
            }
            value => {
                out.push(value);
                index += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).to_string()
}

fn parse_query(query: &str) -> HashMap<String, String> {
    let mut values = HashMap::new();
    for part in query.split('&') {
        if part.is_empty() {
            continue;
        }
        let mut pieces = part.splitn(2, '=');
        let key = url_decode(pieces.next().unwrap_or(""));
        let value = url_decode(pieces.next().unwrap_or(""));
        values.insert(key, value);
    }
    values
}

fn json_escape(input: &str) -> String {
    let mut out = String::with_capacity(input.len() + 8);
    for ch in input.chars() {
        match ch {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            ch if ch < ' ' => out.push_str(&format!("\\u{:04x}", ch as u32)),
            ch => out.push(ch),
        }
    }
    out
}

fn query_int(
    query: &HashMap<String, String>,
    key: &str,
    default_value: usize,
    min_value: usize,
    max_value: usize,
) -> usize {
    query
        .get(key)
        .and_then(|value| value.parse::<usize>().ok())
        .unwrap_or(default_value)
        .clamp(min_value, max_value)
}

fn parse_vector_param(value: &str) -> Vec<f32> {
    value
        .split(',')
        .filter_map(|part| part.parse::<f32>().ok())
        .collect()
}

fn embed_text_with_gbe_service(text: &str, host: &str, port: u16) -> Option<Vec<f32>> {
    if text.is_empty() {
        return None;
    }
    let mut stream = TcpStream::connect((host, port)).ok()?;
    let payload = &text.as_bytes()[..text.len().min(65_536)];
    stream
        .write_all(&(payload.len() as u32).to_le_bytes())
        .ok()?;
    stream.write_all(payload).ok()?;
    let mut dim = [0u8; 4];
    stream.read_exact(&mut dim).ok()?;
    if u32::from_le_bytes(dim) as usize != rustblade::block_table::DOC_VECTOR_DIM {
        return None;
    }
    let mut encoded = vec![0; rustblade::block_table::DOC_VECTOR_DIM];
    stream.read_exact(&mut encoded).ok()?;
    Some(
        encoded
            .into_iter()
            .map(|value| value as i8 as f32 / 128.0)
            .collect(),
    )
}

struct SearchService {
    index_path: String,
    gbe_host: String,
    gbe_port: u16,
    context: Mutex<IndexContext>,
}

impl SearchService {
    fn new(index_path: String, gbe_host: String, gbe_port: u16) -> Result<Self, String> {
        let mut context = IndexContext::new();
        context
            .LoadIndex(&index_path)
            .map_err(|error| format!("{error:?}"))?;
        let docs = context.DocumentCount();
        if docs == 0 {
            return Err(format!(
                "index loaded with zero docs or failed to load: {index_path}"
            ));
        }
        Ok(Self {
            index_path,
            gbe_host,
            gbe_port,
            context: Mutex::new(context),
        })
    }

    fn health_json(&self) -> String {
        let context = self.context.lock().unwrap();
        let documents = context.DocumentCount();
        let avg_doc_len = context.AvgDocLen();
        format!(
            "{{\"status\":\"ok\",\"index\":\"{}\",\"gbe_host\":\"{}\",\"gbe_port\":{},\"documents\":{},\"avg_doc_len\":{},\"vector_count\":{},\"vector_dim\":{}}}",
            json_escape(&self.index_path), json_escape(&self.gbe_host), self.gbe_port, documents, avg_doc_len, context.VectorCount(), context.VectorDimension())
    }

    fn search_json(&self, params: &HashMap<String, String>) -> (u16, String) {
        let query = params.get("q").cloned().unwrap_or_default();
        if query.is_empty() {
            return (400, "{\"error\":\"missing q parameter\"}".to_string());
        }
        let streams = params
            .get("streams")
            .filter(|value| !value.is_empty())
            .cloned()
            .unwrap_or_else(|| "AUTB".to_string());
        let offset = query_int(params, "offset", 0, 0, 1_000_000_000);
        let limit = query_int(params, "limit", 20, 1, 1000);
        let ef_search = query_int(params, "efSearch", 200, 1, 1_000_000);
        let started = Instant::now();

        let vector = embed_text_with_gbe_service(&query, &self.gbe_host, self.gbe_port);
        let vector_ready = vector.is_some();
        let task = {
            let mut context = self.context.lock().unwrap();
            context.EnqueueWithMode(
                &query,
                vector.unwrap_or_default(),
                &streams,
                0,
                rustblade::QueryCompileMode::WeakAndBigramBoostForDoc,
                ef_search,
            )
        };
        let results = task.Wait();
        let elapsed_ms = started.elapsed().as_secs_f64() * 1000.0;
        let total = results.len();
        let begin = offset.min(total);
        let end = (begin + limit).min(total);

        let mut body = format!(
            "{{\"query\":\"{}\",\"streams\":\"{}\",\"total\":{},\"offset\":{},\"limit\":{},\"elapsed_ms\":{},\"vector_ready\":{},\"results\":[",
            json_escape(&query), json_escape(&streams), total, begin, limit, elapsed_ms, vector_ready);
        for (rank, result) in results[begin..end].iter().enumerate() {
            if rank > 0 {
                body.push(',');
            }
            let path = self.context.lock().unwrap().GetDocPath(result.doc_id);
            body.push_str(&format!(
                "{{\"rank\":{},\"doc_id\":{},\"score\":{},\"path\":\"{}\"}}",
                begin + rank + 1,
                result.doc_id,
                result.score,
                json_escape(&path)
            ));
        }
        body.push_str("]}");
        (200, body)
    }

    #[allow(non_snake_case)]
    fn vector_search_json(&self, params: &HashMap<String, String>) -> (u16, String) {
        let query = params.get("q").cloned().unwrap_or_default();
        let offset = query_int(params, "offset", 0, 0, 1_000_000_000);
        let limit = query_int(params, "limit", 20, 1, 1000);
        let efSearch = query_int(params, "efSearch", 200, 1, 1_000_000);
        let started = Instant::now();

        let task = {
            let mut context = self.context.lock().unwrap();
            let vectorQuery =
                if let Some(value) = params.get("vector").filter(|value| !value.is_empty()) {
                    parse_vector_param(value)
                } else {
                    if query.is_empty() {
                        return (
                            400,
                            "{\"error\":\"missing q or vector parameter\"}".to_string(),
                        );
                    }
                    context.CompileToVector(&query)
                };
            if vectorQuery.len() != rustblade::block_table::DOC_VECTOR_DIM {
                return (400, "{\"error\":\"empty query vector\"}".to_string());
            }
            context.EnqueueWithMode(
                "",
                vectorQuery,
                "AUTB",
                0,
                rustblade::QueryCompileMode::WeakAndBigramBoostForDoc,
                efSearch,
            )
        };
        let results = task.Wait();
        let elapsed_ms = started.elapsed().as_secs_f64() * 1000.0;
        let total = results.len();
        let begin = offset.min(total);
        let end = (begin + limit).min(total);

        let mut body = format!(
            "{{\"query\":\"{}\",\"vector_dim\":{},\"vector_count\":{},\"efSearch\":{},\"total\":{},\"offset\":{},\"limit\":{},\"elapsed_ms\":{},\"results\":[",
            json_escape(&query), rustblade::block_table::DOC_VECTOR_DIM, self.context.lock().unwrap().VectorCount(), efSearch, total, begin, limit, elapsed_ms);
        for (rank, result) in results[begin..end].iter().enumerate() {
            if rank > 0 {
                body.push(',');
            }
            let path = self.context.lock().unwrap().GetDocPath(result.doc_id);
            body.push_str(&format!(
                "{{\"rank\":{},\"doc_id\":{},\"score\":{},\"path\":\"{}\"}}",
                begin + rank + 1,
                result.doc_id,
                result.score,
                json_escape(&path)
            ));
        }
        body.push_str("]}");
        (200, body)
    }
}

fn http_response(status: u16, status_text: &str, body: &str) -> String {
    format!(
        "HTTP/1.1 {} {}\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n{}",
        status, status_text, body.as_bytes().len(), body)
}

fn handle_request(service: &SearchService, request: &str) -> String {
    let mut first = request.lines().next().unwrap_or("").split_whitespace();
    let method = first.next().unwrap_or("");
    let target = first.next().unwrap_or("/");
    if method == "OPTIONS" {
        return http_response(204, "No Content", "");
    }
    if method != "GET" {
        return http_response(
            405,
            "Method Not Allowed",
            "{\"error\":\"method not allowed\"}",
        );
    }
    let (path, query) = target.split_once('?').unwrap_or((target, ""));
    let params = parse_query(query);
    match path {
        "/health" => http_response(200, "OK", &service.health_json()),
        "/search" => {
            let (status, body) = service.search_json(&params);
            http_response(status, if status == 200 { "OK" } else { "Bad Request" }, &body)
        }
        "/vector-search" => {
            let (status, body) = service.vector_search_json(&params);
            http_response(status, if status == 200 { "OK" } else { "Bad Request" }, &body)
        },
        "/" | "/help" => http_response(200, "OK", "{\"service\":\"shennong\",\"endpoints\":[\"/health\",\"/search?q=usage&offset=0&limit=20&streams=AUTB\",\"/vector-search?q=usage&offset=0&limit=20\"]}"),
        _ => http_response(404, "Not Found", "{\"error\":\"not found\"}"),
    }
}

fn serve_client(mut stream: TcpStream, service: Arc<SearchService>) {
    let mut buffer = [0u8; 8192];
    let mut request = Vec::new();
    while request.windows(4).all(|window| window != b"\r\n\r\n") && request.len() < 64 * 1024 {
        let Ok(read) = stream.read(&mut buffer) else {
            return;
        };
        if read == 0 {
            break;
        }
        request.extend_from_slice(&buffer[..read]);
    }
    let request = String::from_utf8_lossy(&request);
    let response = handle_request(&service, &request);
    let _ = stream.write_all(response.as_bytes());
}

fn main() {
    let options = match parse_args() {
        Ok(options) => options,
        Err(error) => {
            eprintln!("shennong_rs: {error}");
            std::process::exit(1);
        }
    };
    if !std::path::Path::new(&options.index_path).is_file() {
        eprintln!("index not found: {}", options.index_path);
        std::process::exit(2);
    }
    println!("ShenNong HTTP service starting");
    println!("Index: {}", options.index_path);
    println!("Listen: 0.0.0.0:{}", options.port);
    let service = match SearchService::new(
        options.index_path.clone(),
        options.gbe_host,
        options.gbe_port,
    ) {
        Ok(service) => service,
        Err(error) => {
            eprintln!("shennong_rs: {error}");
            std::process::exit(1);
        }
    };
    println!("Index loaded: {}", service.health_json());
    let service = Arc::new(service);

    let listener = TcpListener::bind(("0.0.0.0", options.port)).expect("bind failed");
    println!(
        "Ready: http://localhost:{}/search?q=usage&offset=0&limit=20",
        options.port
    );
    for stream in listener.incoming().flatten() {
        let service = Arc::clone(&service);
        thread::spawn(move || serve_client(stream, service));
    }
}
