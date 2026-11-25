use reqwest::{
    Client, Response, StatusCode, Url,
    header::{self, HeaderMap, HeaderValue},
};
use std::{
    fs::File,
    io::{self, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering::Relaxed},
    },
    time::Duration,
};
use tempfile::Builder;
use tokio::task::JoinSet;

type ErrBox = Box<dyn std::error::Error + Send + Sync>;
type R<T> = Result<T, ErrBox>;
const IDLE: Duration = Duration::from_secs(15);
const MAX_BYTES: u64 = 1 << 40;
#[derive(Default)]
pub struct Progress {
    pub resolved_path: Mutex<Option<PathBuf>>,
    pub cancel: AtomicBool,
    pub received: AtomicU64,
    pub total: AtomicU64,
    pub connections: AtomicUsize,
    pub retries: AtomicUsize,
}

pub fn url(s: &str) -> R<Url> {
    let mut u = Url::parse(s).map_err(|_| io::Error::other("链接格式无效"))?;
    if !matches!(u.scheme(), "http" | "https")
        || !u.username().is_empty()
        || u.password().is_some()
        || u.host_str().is_none()
    {
        return Err("只支持不带嵌入式凭据的 HTTP / HTTPS 链接".into());
    }
    u.set_fragment(None);
    Ok(u)
}

fn safe_leaf(name: &str) -> Option<String> {
    (name.encode_utf16().count() <= 255
        && !name.trim().is_empty()
        && crate::files::safe_name(name).is_ok())
    .then(|| name.to_owned())
}

pub fn url_filename(url: &Url) -> Option<String> {
    url.query_pairs()
        .filter(|(key, _)| {
            key.eq_ignore_ascii_case("filename") || key.eq_ignore_ascii_case("file_name")
        })
        .find_map(|(_, value)| safe_leaf(&value))
        .or_else(|| {
            let segment = url.path_segments()?.next_back()?;
            safe_leaf(
                &percent_encoding::percent_decode_str(segment)
                    .decode_utf8()
                    .ok()?,
            )
        })
}

// Tokenize parameters outside quoted strings: semicolons inside filenames are data.
fn disposition_name(raw: &str) -> Option<String> {
    let mut plain = None;
    let mut quoted = false;
    let mut escape = false;
    let mut start = 0;
    for (i, c) in raw.char_indices().chain(std::iter::once((raw.len(), ';'))) {
        if escape {
            escape = false;
            continue;
        }
        if quoted && c == '\\' {
            escape = true;
            continue;
        }
        if c == '"' {
            quoted = !quoted;
            continue;
        }
        if c != ';' || quoted {
            continue;
        }
        let param = &raw[start..i];
        start = i + 1;
        let Some((key, value)) = param.split_once('=') else {
            continue;
        };
        let value = value.trim();
        if key.trim().eq_ignore_ascii_case("filename*") {
            let mut parts = value.splitn(3, '\'');
            if let (Some(charset), Some(_lang), Some(data)) =
                (parts.next(), parts.next(), parts.next())
                && charset.eq_ignore_ascii_case("UTF-8")
                && data.as_bytes().iter().enumerate().all(|(i, b)| {
                    *b != b'%'
                        || data
                            .as_bytes()
                            .get(i + 1..i + 3)
                            .is_some_and(|pair| pair.iter().all(u8::is_ascii_hexdigit))
                })
                && let Ok(name) = percent_encoding::percent_decode_str(data).decode_utf8()
                && let Some(name) = safe_leaf(&name)
            {
                return Some(name);
            }
        } else if key.trim().eq_ignore_ascii_case("filename") && plain.is_none() {
            let value = value
                .strip_prefix('"')
                .and_then(|v| v.strip_suffix('"'))
                .unwrap_or(value);
            plain = safe_leaf(value);
        }
    }
    plain
}

enum Target<'a> {
    Exact(&'a Path),
    Automatic(&'a Path, Option<&'a str>),
}
impl Target<'_> {
    fn resolve(&self, response: &Response, original: &Url, p: &Progress) -> R<PathBuf> {
        let path = match self {
            Self::Exact(path) => path.to_path_buf(),
            Self::Automatic(dir, suggested) => {
                let name = response
                    .headers()
                    .get(header::CONTENT_DISPOSITION)
                    .and_then(|h| std::str::from_utf8(h.as_bytes()).ok())
                    .and_then(disposition_name)
                    .or_else(|| suggested.and_then(safe_leaf))
                    .or_else(|| url_filename(response.url()))
                    .or_else(|| url_filename(original))
                    .ok_or("服务器和链接均未提供有效文件名，请点「另存为」手动命名")?;
                dir.join(name)
            }
        };
        if path.symlink_metadata().is_ok() {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                format!("同名目标已存在，未覆盖：{}", path.display()),
            )
            .into());
        }
        *p.resolved_path
            .lock()
            .map_err(|_| io::Error::other("下载路径锁失效"))? = Some(path.clone());
        Ok(path)
    }
}

pub fn run(raw: &str, dest: &Path, connections: usize, progress: Arc<Progress>) -> R<PathBuf> {
    run_to(raw, Target::Exact(dest), connections, progress)
}
pub fn run_auto(
    raw: &str,
    dir: &Path,
    suggested: Option<&str>,
    connections: usize,
    progress: Arc<Progress>,
) -> R<PathBuf> {
    run_to(
        raw,
        Target::Automatic(dir, suggested),
        connections,
        progress,
    )
}
fn run_to(
    raw: &str,
    target: Target<'_>,
    connections: usize,
    progress: Arc<Progress>,
) -> R<PathBuf> {
    let u = url(raw)?;
    if !(1..=512).contains(&connections) {
        return Err("并发数必须在 1–512".into());
    }
    let parent = match target {
        Target::Exact(dest) => {
            if dest.symlink_metadata().is_ok() {
                return Err(
                    io::Error::new(io::ErrorKind::AlreadyExists, "同名目标已存在，未覆盖").into(),
                );
            }
            dest.parent()
                .filter(|p| !p.as_os_str().is_empty())
                .ok_or("请选择保存目录")?
        }
        Target::Automatic(dir, _) => dir,
    };
    let mut temp = Builder::new()
        .prefix(".twinfile-download-")
        .suffix(".part")
        .tempfile_in(parent)
        .map_err(|e| {
            io::Error::new(
                e.kind(),
                format!("无法在 {} 创建下载临时文件：{e}", parent.display()),
            )
        })?;
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()?;
    let dest = rt.block_on(async {
        tokio::select! {
            biased;
            _=wait_cancel(&progress)=>Err::<PathBuf,ErrBox>("下载已取消".into()),
            result=tokio::time::timeout(Duration::from_secs(24*60*60), download(u,temp.as_file_mut(),connections,progress.clone(),&target))=>result?
        }
    })?;
    temp.as_file().sync_all()?;
    if progress.cancel.load(Relaxed) {
        return Err("下载已取消".into());
    }
    temp.persist_noclobber(&dest).map_err(|e| e.error)?;
    Ok(dest)
}

async fn wait_cancel(p: &Progress) {
    while !p.cancel.load(Relaxed) {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

#[derive(Clone)]
struct Version {
    header: header::HeaderName,
    value: HeaderValue,
}
fn version(h: &HeaderMap) -> Option<Version> {
    if let Some(tag) = h.get(header::ETAG) {
        let b = tag.as_bytes();
        return if b.len() >= 2
            && b[0] == b'"'
            && b[b.len() - 1] == b'"'
            && !b[1..b.len() - 1].contains(&b'"')
        {
            Some(Version {
                header: header::ETAG,
                value: tag.clone(),
            })
        } else {
            None
        };
    }
    let last = h.get(header::LAST_MODIFIED)?;
    let lm = httpdate::parse_http_date(last.to_str().ok()?).ok()?;
    let date = httpdate::parse_http_date(h.get(header::DATE)?.to_str().ok()?).ok()?;
    if date.duration_since(lm).ok()? >= Duration::from_secs(60) {
        Some(Version {
            header: header::LAST_MODIFIED,
            value: last.clone(),
        })
    } else {
        None
    }
}

fn content_range(r: &Response) -> Option<(u64, u64, u64)> {
    let raw = r
        .headers()
        .get(header::CONTENT_RANGE)?
        .to_str()
        .ok()?
        .strip_prefix("bytes ")?;
    let (range, total) = raw.split_once('/')?;
    let (a, b) = range.split_once('-')?;
    let triple: (u64, u64, u64) = (a.parse().ok()?, b.parse().ok()?, total.parse().ok()?);
    (triple.0 <= triple.1 && triple.1 < triple.2).then_some(triple)
}

fn identity(r: &Response) -> bool {
    !r.headers().contains_key(header::CONTENT_ENCODING)
        || r.headers()
            .get(header::CONTENT_ENCODING)
            .is_some_and(|v| v.as_bytes().eq_ignore_ascii_case(b"identity"))
}

async fn request(
    c: &Client,
    u: &Url,
    range: Option<(u64, u64)>,
    version: Option<&Version>,
) -> R<Response> {
    let mut b = c.get(u.clone()).header(header::ACCEPT_ENCODING, "identity");
    if let Some((a, z)) = range {
        b = b.header(header::RANGE, format!("bytes={a}-{z}"));
    }
    if let Some(v) = version {
        b = b.header(header::IF_RANGE, &v.value);
    }
    Ok(b.send().await.map_err(|e| e.without_url())?)
}

struct Connection<'a>(&'a Progress);
impl<'a> Connection<'a> {
    fn new(p: &'a Progress) -> Self {
        p.connections.fetch_add(1, Relaxed);
        Self(p)
    }
}
impl Drop for Connection<'_> {
    fn drop(&mut self) {
        self.0.connections.fetch_sub(1, Relaxed);
    }
}

async fn download(
    u: Url,
    file: &mut File,
    n: usize,
    p: Arc<Progress>,
    target: &Target<'_>,
) -> R<PathBuf> {
    let client = Client::builder()
        .no_proxy()
        .connect_timeout(IDLE)
        .read_timeout(IDLE)
        .redirect(reqwest::redirect::Policy::custom(|r| {
            if r.previous().len() >= 5
                || !matches!(r.url().scheme(), "http" | "https")
                || !r.url().username().is_empty()
                || r.url().password().is_some()
                || r.previous()
                    .last()
                    .is_some_and(|prior| prior.scheme() == "https" && r.url().scheme() != "https")
            {
                r.stop()
            } else {
                r.follow()
            }
        }))
        .user_agent(concat!("TwinFileRust/", env!("CARGO_PKG_VERSION")))
        .build()?;
    if n > 1 {
        let probe = {
            let _connection = Connection::new(&p);
            let mut r = request(&client, &u, Some((0, 0)), None).await?;
            match (
                r.status(),
                content_range(&r),
                version(r.headers()),
                identity(&r),
            ) {
                (StatusCode::PARTIAL_CONTENT, Some((0, 0, total)), Some(v), true) if total > 1 => {
                    let final_url = r.url().clone();
                    let mut bytes = 0;
                    while let Some(chunk) = r.chunk().await.map_err(|e| e.without_url())? {
                        bytes += chunk.len();
                        if bytes > 1 {
                            break;
                        }
                    }
                    if bytes == 1 {
                        Some((total, v, final_url, target.resolve(&r, &u, &p)?))
                    } else {
                        None
                    }
                }
                _ => None,
            }
        };
        if let Some((total, v, final_url, dest)) = probe {
            if total > MAX_BYTES {
                return Err("下载文件超过 1 TiB".into());
            }
            p.total.store(total, Relaxed);
            file.set_len(total)?;
            let shared = Arc::new(Mutex::new(file.try_clone()?));
            let count = n.min(total as usize);
            let mut workers = JoinSet::new();
            let mut start = 0;
            for i in 0..count as u64 {
                let len = total / count as u64 + u64::from(i < total % count as u64);
                let end = start + len - 1;
                let (c, u, v, p, writer) = (
                    client.clone(),
                    final_url.clone(),
                    v.clone(),
                    p.clone(),
                    shared.clone(),
                );
                let from = start;
                start = end + 1;
                workers.spawn(async move {
                    let mut at = from;
                    for attempt in 0..=4 {
                        let _connection = Connection::new(&p);
                        let result = async {
                            let mut r = match request(&c, &u, Some((at, end)), Some(&v)).await {
                                Ok(r) => r,
                                Err(e)
                                    if e.downcast_ref::<reqwest::Error>().is_some_and(|e| {
                                        e.is_request() || e.is_connect() || e.is_timeout()
                                    }) =>
                                {
                                    return Ok(false);
                                }
                                Err(e) => return Err(e),
                            };
                            if r.status() == StatusCode::TOO_MANY_REQUESTS
                                || r.status().is_server_error()
                            {
                                return Ok(false);
                            }
                            if r.status() != StatusCode::PARTIAL_CONTENT
                                || r.url() != &u
                                || content_range(&r) != Some((at, end, total))
                                || r.headers().get(&v.header) != Some(&v.value)
                                || !identity(&r)
                                || r.content_length().is_some_and(|len| len != end - at + 1)
                            {
                                return Err::<bool, ErrBox>(
                                    "服务器分段或文件版本不一致，未发布文件".into(),
                                );
                            }
                            loop {
                                let chunk = match r.chunk().await {
                                    Ok(Some(b)) => b,
                                    Ok(None) => return Ok(at > end),
                                    Err(_) if at <= end => return Ok(false),
                                    Err(e) => return Err(e.without_url().into()),
                                };
                                if chunk.len() as u64 > end.saturating_add(1).saturating_sub(at) {
                                    return Err("分段超长，未发布文件".into());
                                }
                                {
                                    let mut f = writer
                                        .lock()
                                        .map_err(|_| io::Error::other("文件锁失效"))?;
                                    f.seek(SeekFrom::Start(at))?;
                                    f.write_all(&chunk)?;
                                }
                                at += chunk.len() as u64;
                                p.received.fetch_add(chunk.len() as u64, Relaxed);
                            }
                        }
                        .await;
                        match result {
                            Ok(true) => return Ok(()),
                            Ok(false) if attempt < 4 => {
                                p.retries.fetch_add(1, Relaxed);
                                drop(_connection);
                                tokio::time::sleep(Duration::from_millis(500 * (attempt + 1)))
                                    .await;
                            }
                            Ok(false) => return Err("分段中断，已重试四次，未发布文件".into()),
                            Err(e) => return Err(e),
                        }
                    }
                    unreachable!()
                });
            }
            while let Some(r) = workers.join_next().await {
                r??;
            }
            return Ok(dest);
        }
    }
    let _connection = Connection::new(&p);
    let mut r = request(&client, &u, None, None).await?;
    if r.status() != StatusCode::OK || !identity(&r) {
        return Err(format!("服务器未返回完整原始文件：HTTP {}", r.status()).into());
    }
    let dest = target.resolve(&r, &u, &p)?;
    let expected = r.content_length();
    if expected.is_some_and(|n| n > MAX_BYTES) {
        return Err("下载文件超过 1 TiB".into());
    }
    p.total.store(expected.unwrap_or_default(), Relaxed);
    while let Some(chunk) = r.chunk().await.map_err(|e| e.without_url())? {
        if chunk.len() as u64 > MAX_BYTES.saturating_sub(p.received.load(Relaxed)) {
            return Err("下载文件超过 1 TiB".into());
        }
        file.write_all(&chunk)?;
        p.received.fetch_add(chunk.len() as u64, Relaxed);
    }
    if expected.is_some_and(|n| n != p.received.load(Relaxed)) {
        return Err("完整下载大小不符，未发布文件".into());
    }
    Ok(dest)
}
