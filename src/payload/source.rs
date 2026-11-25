use super::*;
use reqwest::{blocking::Client, header as h};
use std::time::{Duration, SystemTime};
const MAX: u64 = 1 << 40;
#[derive(Debug)]
pub(super) enum Temporary {
    File { _file: tempfile::NamedTempFile },
    Dir { _dir: tempfile::TempDir },
}
#[derive(Debug)]
pub(super) enum Source {
    Local {
        path: PathBuf,
        _tmp: Option<Temporary>,
        len: u64,
        modified: Option<SystemTime>,
    },
    Http {
        client: Client,
        url: reqwest::Url,
        len: u64,
        validator: h::HeaderValue,
    },
}
impl Source {
    fn local(path: PathBuf, tmp: Option<Temporary>) -> R<Self> {
        let m = fs::metadata(&path)?;
        guard(
            m.is_file() && m.len() <= MAX,
            "来源不是普通文件或超过 1 TiB",
        )?;
        Ok(Self::Local {
            path,
            _tmp: tmp,
            len: m.len(),
            modified: m.modified().ok(),
        })
    }
    pub fn len(&self) -> u64 {
        match self {
            Self::Local { len, .. } | Self::Http { len, .. } => *len,
        }
    }
    pub fn reader<'a>(&'a self, cancel: &'a AtomicBool) -> R<Reader<'a>> {
        let file = if let Self::Local {
            path,
            len,
            modified,
            ..
        } = self
        {
            let f = File::open(path)?;
            let m = f.metadata()?;
            guard(
                m.len() == *len && m.modified().ok() == *modified,
                "Payload 来源已变化，请重新读取",
            )?;
            Some(f)
        } else {
            None
        };
        Ok(Reader {
            source: self,
            pos: 0,
            file,
            cache: Vec::new(),
            cache_start: 0,
            cancel,
        })
    }
}
fn copy_cancel(
    mut r: impl Read,
    mut w: impl Write,
    cancel: &AtomicBool,
    max: u64,
    label: &str,
    report: &Report<'_>,
) -> R<u64> {
    let mut buf = vec![0; 256 * 1024];
    let mut n = 0;
    loop {
        check(cancel)?;
        let got = r.read(&mut buf)?;
        if got == 0 {
            return Ok(n);
        }
        n += got as u64;
        guard(n <= max, "下载 / 解压大小超过限制")?;
        w.write_all(&buf[..got])?;
        report(label, n, if max == MAX { 0 } else { max });
    }
}
fn open(raw: &str, cancel: &AtomicBool, report: &Report<'_>) -> R<Source> {
    if !raw.starts_with("http://") && !raw.starts_with("https://") {
        return Source::local(PathBuf::from(raw), None);
    }
    let client = Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::custom(|attempt| {
            let target = attempt.url();
            if attempt.previous().len() >= 5
                || !matches!(target.scheme(), "http" | "https")
                || !target.username().is_empty()
                || target.password().is_some()
                || attempt
                    .previous()
                    .last()
                    .is_some_and(|prior| prior.scheme() == "https" && target.scheme() != "https")
            {
                attempt.stop()
            } else {
                attempt.follow()
            }
        }))
        .connect_timeout(Duration::from_secs(15))
        .timeout(Duration::from_secs(180))
        .user_agent("TwinFile-Rust/0.1")
        .build()
        .map_err(err)?;
    let url = crate::download::url(raw).map_err(err)?;
    let req = || {
        client
            .get(url.clone())
            .header(h::ACCEPT_ENCODING, "identity")
    };
    let response = req()
        .header(h::RANGE, "bytes=0-0")
        .send()
        .map_err(|e| err(e.without_url()))?;
    let cache_free = super::space(&std::env::temp_dir());
    if response.status() == 206 {
        let hs = response.headers();
        let content = hs
            .get(h::CONTENT_RANGE)
            .and_then(|x| x.to_str().ok())
            .and_then(|x| x.strip_prefix("bytes 0-0/"))
            .and_then(|x| x.parse::<u64>().ok())
            .ok_or_else(|| err("HTTP Content-Range 不匹配"))?;
        guard(content > 0 && content <= MAX, "远程文件过大或为空")?;
        let validator = hs
            .get(h::ETAG)
            .filter(|v| {
                let b = v.as_bytes();
                b.len() >= 2
                    && b.first() == Some(&b'"')
                    && b.last() == Some(&b'"')
                    && !b[1..b.len() - 1].contains(&b'"')
            })
            .cloned();
        if let Some(validator) = validator {
            let endpoint = response.url().clone();
            let mut probe = Vec::new();
            response.take(2).read_to_end(&mut probe)?;
            guard(probe.len() == 1, "无效 HTTP Range 探测响应")?;
            return Ok(Source::Http {
                client,
                url: endpoint,
                len: content,
                validator,
            });
        }
        guard(
            cache_free.is_none_or(|n| n >= content),
            "临时目录空间不足，无法完整缓存 OTA",
        )?;
        report("服务器未提供强 ETag，完整缓存 OTA 后读取", 0, content);
    } else if response.status() == 200
        && response
            .content_length()
            .is_some_and(|len| len > MAX || cache_free.is_some_and(|free| free < len))
    {
        return Err(err("临时目录空间不足，或远程文件超限"));
    } else if response.status() != 200 {
        return Err(err(format!("HTTP 来源返回 {}", response.status())));
    } else {
        report(
            "服务器不支持 Range，完整缓存 OTA 后读取",
            0,
            response.content_length().unwrap_or(0),
        );
    }
    // A full OTA can take hours: use the existing idle-timed, cancellable downloader
    // instead of applying a metadata-sized blocking request timeout to the whole file.
    drop(response);
    let tmp = tempfile::tempdir()?;
    let path = tmp.path().join("source.ota");
    let state = std::sync::Arc::new(crate::download::Progress::default());
    std::thread::scope(|s| -> R<()> {
        let job = s.spawn(|| crate::download::run(url.as_str(), &path, 1, state.clone()));
        while !job.is_finished() {
            if cancel.load(Relaxed) {
                state.cancel.store(true, Relaxed)
            }
            report(
                "缓存完整 OTA",
                state.received.load(Relaxed),
                state.total.load(Relaxed),
            );
            std::thread::sleep(Duration::from_millis(80));
        }
        job.join().map_err(|_| err("下载线程异常"))?.map_err(err)?;
        Ok(())
    })?;
    Source::local(path, Some(Temporary::Dir { _dir: tmp }))
}
pub(super) struct Reader<'a> {
    source: &'a Source,
    pos: u64,
    file: Option<File>,
    cache: Vec<u8>,
    cache_start: u64,
    cancel: &'a AtomicBool,
}
impl Seek for Reader<'_> {
    fn seek(&mut self, from: SeekFrom) -> R<u64> {
        let v = match from {
            SeekFrom::Start(n) => i128::from(n),
            SeekFrom::End(d) => i128::from(self.source.len()) + i128::from(d),
            SeekFrom::Current(d) => i128::from(self.pos) + i128::from(d),
        };
        guard(v >= 0 && v <= u64::MAX as i128, "无效文件偏移")?;
        self.pos = v as u64;
        if let Some(f) = &mut self.file {
            f.seek(SeekFrom::Start(self.pos))?;
        }
        Ok(self.pos)
    }
}
impl Read for Reader<'_> {
    fn read(&mut self, out: &mut [u8]) -> R<usize> {
        check(self.cancel)?;
        if out.is_empty() || self.pos >= self.source.len() {
            return Ok(0);
        }
        if let Some(f) = &mut self.file {
            let n = f.read(out)?;
            self.pos += n as u64;
            return Ok(n);
        }
        let Source::Http {
            client,
            url,
            len,
            validator,
        } = self.source
        else {
            unreachable!()
        };
        if self.pos < self.cache_start || self.pos >= self.cache_start + self.cache.len() as u64 {
            let end = (*len - 1).min(self.pos + 1024 * 1024 - 1);
            let expected = (end - self.pos + 1) as usize;
            let resp = client
                .get(url.clone())
                .header(h::ACCEPT_ENCODING, "identity")
                .header(h::IF_RANGE, validator.clone())
                .header(h::RANGE, format!("bytes={}-{}", self.pos, end))
                .timeout(Duration::from_secs(60))
                .send()
                .map_err(|e| err(e.without_url()))?;
            guard(
                resp.status() == 206
                    && resp.url() == url
                    && resp
                        .headers()
                        .get(h::CONTENT_ENCODING)
                        .is_none_or(|v| v.as_bytes().eq_ignore_ascii_case(b"identity"))
                    && resp
                        .headers()
                        .get(h::CONTENT_RANGE)
                        .and_then(|s| s.to_str().ok())
                        == Some(format!("bytes {}-{}/{}", self.pos, end, len).as_str())
                    && resp.headers().get(h::ETAG).is_some_and(|v| v == validator),
                "HTTP Range 不匹配或远程文件已变化",
            )?;
            self.cache.clear();
            resp.take(expected as u64 + 1)
                .read_to_end(&mut self.cache)?;
            guard(self.cache.len() == expected, "HTTP Range 返回长度错误")?;
            self.cache_start = self.pos;
        }
        let off = (self.pos - self.cache_start) as usize;
        let n = out.len().min(self.cache.len() - off);
        out[..n].copy_from_slice(&self.cache[off..off + n]);
        self.pos += n as u64;
        Ok(n)
    }
}
pub(super) fn locate(raw: &str, cancel: &AtomicBool, report: &Report<'_>) -> R<(Source, u64, u64)> {
    check(cancel)?;
    let src = open(raw, cancel, report)?;
    let mut reader = src.reader(cancel)?;
    let mut head = [0; 4];
    reader.read_exact(&mut head)?;
    if head == *b"CrAU" {
        let n = src.len();
        return Ok((src, 0, n));
    }
    guard(head.starts_with(b"PK"), "不是 CrAU Payload 或 OTA ZIP")?;
    reader.seek(SeekFrom::Start(0))?;
    let mut zip = zip::ZipArchive::new(reader).map_err(err)?;
    let mut entry = zip
        .by_name("payload.bin")
        .map_err(|e| err(format!("ZIP 没有可读取的 payload.bin：{e}")))?;
    let n = entry.size();
    guard(
        !entry.encrypted() && n <= MAX && n >= 24,
        "ZIP 内 payload.bin 大小或加密状态不支持",
    )?;
    if entry.compression() == zip::CompressionMethod::Stored {
        let offset = entry.data_start();
        guard(
            entry.compressed_size() == n && offset <= src.len() && n <= src.len() - offset,
            "ZIP payload.bin 越界",
        )?;
        drop(entry);
        drop(zip);
        return Ok((src, offset, n));
    }
    guard(
        entry.compression() == zip::CompressionMethod::Deflated,
        "ZIP 内 payload.bin 仅支持 Stored/Deflate",
    )?;
    guard(
        super::space(&std::env::temp_dir()).is_none_or(|available| available >= n),
        "临时目录磁盘空间不足，无法展开 ZIP",
    )?;
    let mut tmp = tempfile::NamedTempFile::new()?;
    guard(
        copy_cancel(
            &mut entry,
            &mut tmp,
            cancel,
            n,
            "展开 ZIP 中的 payload.bin",
            report,
        )? == n,
        "ZIP 解压长度不符",
    )?;
    let path = tmp.path().to_owned();
    Ok((
        Source::local(path, Some(Temporary::File { _file: tmp }))?,
        0,
        n,
    ))
}
