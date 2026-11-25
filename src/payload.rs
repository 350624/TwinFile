//! CrAU v2 full OTA inspection and verified selective partition extraction.
use sha2::{Digest, Sha256};
use std::{
    collections::HashSet,
    fmt::Display,
    fs::{self, File},
    io::{self, BufRead, BufReader, Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    sync::{
        Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed},
    },
};
mod manifest;
mod source;
pub use manifest::Partition;
type R<T> = io::Result<T>;
type Report<'a> = dyn Fn(&str, u64, u64) + Sync + 'a;
fn err(e: impl Display) -> io::Error {
    io::Error::other(e.to_string())
}
fn guard(ok: bool, m: &str) -> R<()> {
    if ok { Ok(()) } else { Err(err(m)) }
}
fn check(c: &AtomicBool) -> R<()> {
    guard(!c.load(Relaxed), "Payload 任务已取消")
}
#[derive(Debug)]
pub struct Payload {
    pub partitions: Vec<Partition>,
    source: source::Source,
    data: u64,
    block: u64,
}
pub fn inspect(raw: &str, cancel: &AtomicBool, report: &Report<'_>) -> Result<Payload, String> {
    (|| -> R<Payload> {
        report("读取 Payload 元数据", 0, 0);
        let (src, base, len) = source::locate(raw, cancel, report)?;
        let mut r = src.reader(cancel)?;
        r.seek(SeekFrom::Start(base))?;
        let mut h = [0; 24];
        r.read_exact(&mut h)?;
        guard(
            h[..4] == *b"CrAU" && u64::from_be_bytes(h[4..12].try_into().unwrap()) == 2,
            "仅支持 CrAU v2 Payload",
        )?;
        let size = u64::from_be_bytes(h[12..20].try_into().unwrap());
        guard((1..=64 << 20).contains(&size), "Payload 元数据大小无效")?;
        let off = 24 + size + u32::from_be_bytes(h[20..24].try_into().unwrap()) as u64;
        guard(off <= len, "Payload 元数据越界")?;
        let mut b = vec![0; size as usize];
        r.read_exact(&mut b)?;
        drop(r);
        let (block, partitions) = manifest::parse(&b, len - off, cancel)?;
        Ok(Payload {
            partitions,
            source: src,
            data: base + off,
            block,
        })
    })()
    .map_err(|e| e.to_string())
}
struct HashRead<Rd> {
    r: Rd,
    hash: Sha256,
}
impl<Rd: Read> Read for HashRead<Rd> {
    fn read(&mut self, b: &mut [u8]) -> R<usize> {
        let n = self.r.read(b)?;
        self.hash.update(&b[..n]);
        Ok(n)
    }
}
struct StageProgress<'a> {
    processed: Mutex<u64>,
    total: u64,
    report: &'a Report<'a>,
}
impl StageProgress<'_> {
    fn add(&self, label: &str, n: u64) {
        let mut val = self.processed.lock().unwrap();
        *val += n;
        (self.report)(label, *val, self.total);
    }
}
fn partition(
    p: &Payload,
    part: &Partition,
    path: &Path,
    cancel: &AtomicBool,
    abort: &AtomicBool,
    prog: &StageProgress<'_>,
) -> R<()> {
    let mut r = p.source.reader(cancel)?;
    let mut out = File::options()
        .read(true)
        .write(true)
        .create_new(true)
        .open(path)?;
    out.set_len(part.size)?;
    let writing = format!("提取 {}", part.name);
    let verify = format!("校验 {}", part.name);
    let mut buf = vec![0; 256 * 1024];
    let mut written = 0;
    for op in &part.ops {
        check(cancel)?;
        check(abort)?;
        if matches!(op.kind, 6 | 7) {
            let n = op.ext.iter().map(|(_, n)| n * p.block).sum::<u64>();
            prog.add(&writing, n);
            written += n;
            continue;
        }
        r.seek(SeekFrom::Start(p.data + op.off))?;
        let mut input = BufReader::new(HashRead {
            r: (&mut r).take(op.len),
            hash: Sha256::new(),
        });
        {
            let mut decoded: Box<dyn Read + '_> = match op.kind {
                0 => Box::new(&mut input),
                1 => Box::new(bzip2::bufread::BzDecoder::new(&mut input)),
                8 => Box::new(xz2::bufread::XzDecoder::new_stream(
                    &mut input,
                    xz2::stream::Stream::new_stream_decoder(256 << 20, 0).map_err(err)?,
                )),
                14 => {
                    let mut d =
                        zstd::stream::read::Decoder::with_buffer(&mut input)?.single_frame();
                    d.window_log_max(28)?;
                    Box::new(d)
                }
                _ => unreachable!(),
            };
            for &(a, n) in &op.ext {
                out.seek(SeekFrom::Start(a * p.block))?;
                let mut left = n * p.block;
                while left > 0 {
                    check(cancel)?;
                    check(abort)?;
                    let step = left.min(buf.len() as u64) as usize;
                    let got = decoded.read(&mut buf[..step])?;
                    guard(got > 0, "解码输出短于目标 extent")?;
                    out.write_all(&buf[..got])?;
                    left -= got as u64;
                    written += got as u64;
                    prog.add(&writing, got as u64);
                }
            }
            guard(decoded.read(&mut buf[..1])? == 0, "解码输出长于目标 extent")?;
        }
        guard(
            input.fill_buf()?.is_empty() && input.get_ref().r.limit() == 0,
            "压缩数据被截断或含尾随内容",
        )?;
        if let Some(expected) = op.hash {
            guard(
                <[u8; 32]>::from(input.into_inner().hash.finalize()) == expected,
                "操作数据 SHA256 校验失败",
            )?;
        }
    }
    if written < part.size {
        prog.add(&writing, part.size - written)
    }
    if let Some(expected) = part.hash {
        out.seek(SeekFrom::Start(0))?;
        let mut hash = Sha256::new();
        loop {
            check(cancel)?;
            check(abort)?;
            let n = out.read(&mut buf)?;
            if n == 0 {
                break;
            }
            hash.update(&buf[..n]);
            prog.add(&verify, n as u64);
        }
        guard(
            <[u8; 32]>::from(hash.finalize()) == expected,
            "分区 SHA256 校验失败",
        )?;
    }
    out.sync_all()?;
    Ok(())
}
#[cfg(windows)]
fn space(dir: &Path) -> Option<u64> {
    use std::os::windows::ffi::OsStrExt;
    let wide = dir.as_os_str().encode_wide().chain([0]).collect::<Vec<_>>();
    let mut free = 0;
    let ok = unsafe {
        windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW(
            wide.as_ptr(),
            &mut free,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    (ok != 0).then_some(free)
}
#[cfg(not(windows))]
fn space(_dir: &Path) -> Option<u64> {
    None
}
pub fn extract(
    p: &Payload,
    names: &[String],
    dir: &Path,
    workers: usize,
    cancel: &AtomicBool,
    report: &Report<'_>,
) -> Result<Vec<PathBuf>, String> {
    (|| -> R<Vec<PathBuf>> {
        check(cancel)?;
        let ns: HashSet<_> = names.iter().collect();
        let selected: Vec<_> = p
            .partitions
            .iter()
            .filter(|part| ns.contains(&part.name))
            .collect();
        guard(
            !selected.is_empty() && selected.len() == ns.len() && dir.is_dir(),
            "请选择有效分区和已有输出目录",
        )?;
        let mut total = 0;
        let mut disk = 0;
        for part in &selected {
            let path = dir.join(format!("{}.img", part.name));
            if fs::symlink_metadata(&path).is_ok() {
                return Err(err(format!("目标已存在，不覆盖：{}", path.display())));
            }
            disk += part.size;
            total += part.size * if part.hash.is_some() { 2 } else { 1 };
        }
        guard(
            space(dir).is_none_or(|free| free >= disk),
            "目标磁盘剩余空间不足",
        )?;
        let tmp = tempfile::Builder::new()
            .prefix(".twinfile-payload-")
            .tempdir_in(dir)?;
        let prog = StageProgress {
            processed: Mutex::new(0),
            total,
            report,
        };
        let next = AtomicUsize::new(0);
        let abort = AtomicBool::new(false);
        let failure = Mutex::new(None);
        std::thread::scope(|s| {
            for _ in 0..workers.clamp(1, 8).min(selected.len()) {
                s.spawn(|| {
                    while !abort.load(Relaxed) {
                        let Some(part) = selected.get(next.fetch_add(1, Relaxed)) else {
                            break;
                        };
                        if let Err(e) = partition(
                            p,
                            part,
                            &tmp.path().join(format!("{}.img", part.name)),
                            cancel,
                            &abort,
                            &prog,
                        ) {
                            let mut f = failure.lock().unwrap();
                            if f.is_none() {
                                *f = Some(err(format!("{}：{e}", part.name)));
                            }
                            abort.store(true, Relaxed);
                            break;
                        }
                    }
                });
            }
        });
        if let Some(e) = failure.into_inner().unwrap() {
            return Err(e);
        }
        check(cancel)?;
        let mut published: Vec<PathBuf> = vec![];
        for part in selected {
            let name = format!("{}.img", part.name);
            let to = dir.join(&name);
            if let Err(e) =
                check(cancel).and_then(|_| crate::files::rename_new(&tmp.path().join(name), &to))
            {
                let mut failed = vec![];
                for path in published.iter().rev() {
                    if crate::files::rename_new(path, &tmp.path().join(path.file_name().unwrap()))
                        .is_err()
                    {
                        failed.push(path.display().to_string())
                    }
                }
                return Err(err(format!("提交输出失败：{e}，未能撤回：{failed:?}")));
            }
            published.push(to)
        }
        Ok(published)
    })()
    .map_err(|e| e.to_string())
}
#[cfg(test)]
mod tests;
