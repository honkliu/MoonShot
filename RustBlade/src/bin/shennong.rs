use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::Instant;

use rustblade::IndexContext;
use rustblade::executor::IndexSearchExecutor;

struct Options {
    port: u16,
    index_path: String,
}

fn home_dir() -> PathBuf {
    if cfg!(windows) {
        std::env::var_os("USERPROFILE").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("C:/Users/Default"))
    } else {
        std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/tmp"))
    }
}

fn default_index_path() -> String {
    home_dir().join("moon.idx").to_string_lossy().to_string()
}

fn parse_args() -> Result<Options, String> {
    let mut options = Options { port: 9000, index_path: default_index_path() };
    let args: Vec<String> = std::env::args().collect();
    let mut index = 1usize;
    while index < args.len() {
        match args[index].as_str() {
            "--port" if index + 1 < args.len() => {
                options.port = args[index + 1].parse::<u16>().map_err(|_| "invalid --port value".to_string())?;
                index += 2;
            }
            "--index" if index + 1 < args.len() => {
                options.index_path = expand_user_path(&args[index + 1]);
                index += 2;
            }
            "--help" | "-h" => {
                println!("usage: shennong_rs [--port 9000] [--index ~/moon.idx]");
                std::process::exit(0);
            }
            other => return Err(format!("unknown or incomplete argument: {other}")),
        }
    }
    Ok(options)
}

fn expand_user_path(path: &str) -> String {
    if path == "~" { return home_dir().to_string_lossy().to_string(); }
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
            b'+' => { out.push(b' '); index += 1; }
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
            value => { out.push(value); index += 1; }
        }
    }
    String::from_utf8_lossy(&out).to_string()
}

fn parse_query(query: &str) -> HashMap<String, String> {
    let mut values = HashMap::new();
    for part in query.split('&') {
        if part.is_empty() { continue; }
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

fn query_int(query: &HashMap<String, String>, key: &str, default_value: usize, min_value: usize, max_value: usize) -> usize {
    query.get(key)
        .and_then(|value| value.parse::<usize>().ok())
        .unwrap_or(default_value)
        .clamp(min_value, max_value)
}

struct SearchService {
    index_path: String,
    context: Mutex<IndexContext>,
}

impl SearchService {
    fn new(index_path: String) -> Result<Self, String> {
        let mut context = IndexContext::new();
        context.LoadIndex(&index_path).map_err(|error| format!("{error:?}"))?;
        let docs = context.DocumentCount();
        if docs == 0 { return Err(format!("index loaded with zero docs or failed to load: {index_path}")); }
        Ok(Self { index_path, context: Mutex::new(context) })
    }

    fn health_json(&self) -> String {
        let context = self.context.lock().unwrap();
        let documents = context.DocumentCount();
        let avg_doc_len = context.AvgDocLen();
        format!(
            "{{\"status\":\"ok\",\"index\":\"{}\",\"documents\":{},\"avg_doc_len\":{}}}",
            json_escape(&self.index_path), documents, avg_doc_len)
    }

    fn search_json(&self, params: &HashMap<String, String>) -> (u16, String) {
        let query = params.get("q").cloned().unwrap_or_default();
        if query.is_empty() { return (400, "{\"error\":\"missing q parameter\"}".to_string()); }
        let streams = params.get("streams").filter(|value| !value.is_empty()).cloned().unwrap_or_else(|| "AUTB".to_string());
        let offset = query_int(params, "offset", 0, 0, 1_000_000_000);
        let limit = query_int(params, "limit", 20, 1, 1000);
        let started = Instant::now();

        let mut context = self.context.lock().unwrap();
        let tree = context.Compile(&query, &streams);
        let mut reader = context.GetReader(tree);
        let store = context.GetStore();
        let store = store.lock().unwrap();
        let executor = IndexSearchExecutor::new(&store);
        let results = executor.Execute(reader.as_mut(), 0);
        let elapsed_ms = started.elapsed().as_secs_f64() * 1000.0;
        let total = results.len();
        let begin = offset.min(total);
        let end = (begin + limit).min(total);

        let mut body = format!(
            "{{\"query\":\"{}\",\"streams\":\"{}\",\"total\":{},\"offset\":{},\"limit\":{},\"elapsed_ms\":{},\"results\":[",
            json_escape(&query), json_escape(&streams), total, begin, limit, elapsed_ms);
        for (rank, result) in results[begin..end].iter().enumerate() {
            if rank > 0 { body.push(','); }
            let path = context.GetDocPath(result.doc_id);
            body.push_str(&format!(
                "{{\"rank\":{},\"doc_id\":{},\"score\":{},\"path\":\"{}\"}}",
                begin + rank + 1, result.doc_id, result.score, json_escape(&path)));
        }
        body.push_str("]}");
        (200, body)
    }

    #[allow(non_snake_case)]
    fn vector_search_json(&self, params: &HashMap<String, String>) -> (u16, String) {
        let query = params.get("q").cloned().unwrap_or_default();
        if query.is_empty() { return (400, "{\"error\":\"missing q parameter\"}".to_string()); }
        let offset = query_int(params, "offset", 0, 0, 1_000_000_000);
        let limit = query_int(params, "limit", 20, 1, 1000);
        let efSearch = query_int(params, "ef", 200, 1, 10_000);
        let started = Instant::now();

        let mut context = self.context.lock().unwrap();
        let vectorQuery = context.CompileToVector(&query);
        let results = context.VectorSearch(&vectorQuery, 0, efSearch);
        let elapsed_ms = started.elapsed().as_secs_f64() * 1000.0;
        let total = results.len();
        let begin = offset.min(total);
        let end = (begin + limit).min(total);

        let mut body = format!(
            "{{\"query\":\"{}\",\"total\":{},\"offset\":{},\"limit\":{},\"elapsed_ms\":{},\"results\":[",
            json_escape(&query), total, begin, limit, elapsed_ms);
        for (rank, result) in results[begin..end].iter().enumerate() {
            if rank > 0 { body.push(','); }
            let path = context.GetDocPath(result.doc_id);
            body.push_str(&format!(
                "{{\"rank\":{},\"doc_id\":{},\"score\":{},\"path\":\"{}\"}}",
                begin + rank + 1, result.doc_id, result.score, json_escape(&path)));
        }
        body.push_str("]}");
        (200, body)
    }
}

fn http_response(status: u16, status_text: &str, body: &str) -> String {
    format!(
        "HTTP/1.1 {} {}\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nConnection: Close\r\n\r\n{}",
        status, status_text, body.as_bytes().len(), body)
}

fn handle_request(service: &SearchService, request: &str) -> String {
    let mut first = request.lines().next().unwrap_or("").split_whitespace();
    let method = first.next().unwrap_or("");
    let target = first.next().unwrap_or("/");
    if method == "OPTIONS" { return http_response(204, "No Content", ""); }
    if method != "GET" { return http_response(405, "Method Not Allowed", "{\"error\":\"method not allowed\"}"); }
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
        "/" | "/help" => http_response(200, "OK", "{\"service\":\"shennong_rs\",\"endpoints\":[\"/health\",\"/search?q=usage&offset=0&limit=20&streams=AUTB\",\"/vector-search?q=usage&offset=0&limit=20\"]}"),
        _ => http_response(404, "Not Found", "{\"error\":\"not found\"}"),
    }
}

fn serve_client(mut stream: TcpStream, service: &SearchService) {
    let mut buffer = [0u8; 8192];
    let mut request = Vec::new();
    while request.windows(4).all(|window| window != b"\r\n\r\n") && request.len() < 64 * 1024 {
        let Ok(read) = stream.read(&mut buffer) else { return; };
        if read == 0 { break; }
        request.extend_from_slice(&buffer[..read]);
    }
    let request = String::from_utf8_lossy(&request);
    let response = handle_request(service, &request);
    let _ = stream.write_all(response.as_bytes());
}

fn main() {
    let options = match parse_args() {
        Ok(options) => options,
        Err(error) => { eprintln!("shennong_rs: {error}"); std::process::exit(1); }
    };
    if !std::path::Path::new(&options.index_path).is_file() {
        eprintln!("index not found: {}", options.index_path);
        std::process::exit(2);
    }
    println!("ShenNong Rust HTTP service starting");
    println!("Index: {}", options.index_path);
    println!("Listen: 0.0.0.0:{}", options.port);
    let service = match SearchService::new(options.index_path.clone()) {
        Ok(service) => service,
        Err(error) => { eprintln!("shennong_rs: {error}"); std::process::exit(1); }
    };
    println!("Index loaded: {}", service.health_json());

    let listener = TcpListener::bind(("0.0.0.0", options.port)).expect("bind failed");
    println!("Ready: http://localhost:{}/search?q=usage&offset=0&limit=20", options.port);
    for stream in listener.incoming().flatten() {
        serve_client(stream, &service);
    }
}
