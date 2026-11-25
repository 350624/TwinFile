use crate::{archive, files};
use std::{
    collections::BTreeMap,
    fs::File,
    io,
    path::{Path, PathBuf},
    sync::{Mutex, atomic::AtomicBool},
};
use tempfile::NamedTempFile;
use zip::{CompressionMethod, ZipArchive, ZipWriter, write::SimpleFileOptions};

type R<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;
#[derive(Debug)]
pub enum Change {
    Rename { from: PathBuf, to: PathBuf },
    Delete(PathBuf),
    AddDirectory(PathBuf),
    AddFile { at: PathBuf, source: PathBuf },
    Replace { at: PathBuf, source: PathBuf },
}
pub fn inner_name(p: &Path) -> R<String> {
    let parts = p
        .components()
        .map(|c| {
            let std::path::Component::Normal(s) = c else {
                return Err(io::Error::other("不是包内相对路径"));
            };
            files::safe_name(
                s.to_str()
                    .ok_or_else(|| io::Error::other("非法 Unicode 路径"))?,
            )
        })
        .collect::<io::Result<Vec<_>>>()?;
    if parts.is_empty() {
        return Err("包内路径为空".into());
    }
    Ok(parts.join("/"))
}
fn folded(name: &str) -> String {
    name.trim_end_matches('/').to_lowercase()
}
fn validate_tree(rows: impl Iterator<Item = (String, bool)>) -> R<()> {
    let mut found = BTreeMap::new();
    for (n, dir) in rows {
        if found.insert(folded(&n), dir).is_some() {
            return Err(format!("包内名称重复：{n}").into());
        }
    }
    for n in found.keys() {
        for (i, _) in n.match_indices('/') {
            if found.get(&n[..i]) == Some(&false) {
                return Err(format!("包内父路径是文件：{n}").into());
            }
        }
    }
    Ok(())
}
fn stream(p: &Path) -> R<File> {
    let m = p.symlink_metadata()?;
    if files::is_link(&m) || !m.is_file() {
        return Err("只接受本地常规文件".into());
    }
    Ok(File::open(p)?)
}
pub fn stage(src: &Path, inner: &Path, cancel: &AtomicBool) -> R<NamedTempFile> {
    let mut z = ZipArchive::new(stream(src)?)?;
    let n = inner_name(inner)?;
    let mut item = z.by_name(&n)?;
    if archive::path(&item)? != inner || !item.is_file() || item.size() > 512 * 1024 * 1024 {
        return Err("包内文件类型不支持或超过 512 MiB 暂存上限".into());
    }
    let ext = inner
        .extension()
        .and_then(|s| s.to_str())
        .filter(|s| s.len() < 20 && s.bytes().all(|b| b.is_ascii_alphanumeric()));
    let suffix = ext.map_or(String::new(), |e| format!(".{e}"));
    let mut tmp = tempfile::Builder::new()
        .prefix("twinfile-entry-")
        .suffix(&suffix)
        .tempfile()?;
    files::copy_stream(&mut item, &mut tmp, cancel)?;
    tmp.as_file().sync_all()?;
    Ok(tmp)
}
pub fn fingerprint(src: &Path, cancel: &AtomicBool) -> R<String> {
    drop(stream(src)?);
    Ok(files::checksum(src, cancel)?[2].clone())
}
/// A local archive, optionally containing a chain of archives inside it.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Source {
    root: PathBuf,
    chain: Vec<PathBuf>,
}
impl Source {
    pub fn local(root: PathBuf) -> Self {
        Self {
            root,
            chain: vec![],
        }
    }
    pub fn child(&self, entry: PathBuf) -> R<Self> {
        inner_name(&entry)?;
        if self.chain.len() >= 7 {
            return Err("最多浏览 8 层压缩包".into());
        }
        let mut next = self.clone();
        next.chain.push(entry);
        Ok(next)
    }
    pub fn root(&self) -> &Path {
        &self.root
    }
    pub fn leaf(&self) -> &Path {
        self.chain
            .last()
            .map_or(self.root.as_path(), PathBuf::as_path)
    }
    pub fn label(&self) -> String {
        let mut result = self.root.display().to_string();
        for entry in &self.chain {
            result.push_str(" › ");
            result.push_str(&entry.display().to_string());
        }
        result
    }
    fn stage_chain(&self, cancel: &AtomicBool) -> R<Vec<tempfile::TempPath>> {
        let mut files: Vec<tempfile::TempPath> = vec![];
        for entry in &self.chain {
            let parent = files.last().map_or(self.root.as_path(), |p| p.as_ref());
            // Windows cannot replace a NamedTempFile with its original handle open.
            files.push(stage(parent, entry, cancel)?.into_temp_path());
        }
        Ok(files)
    }
    pub fn read<T>(
        &self,
        expected: Option<&str>,
        cancel: &AtomicBool,
        f: impl FnOnce(&Path) -> R<T>,
    ) -> R<(T, String)> {
        let hash = fingerprint(&self.root, cancel)?;
        if expected.is_some_and(|x| x != hash) {
            return Err("压缩包已被其他程序修改，请刷新".into());
        }
        let files = self.stage_chain(cancel)?;
        let view = files.last().map_or(self.root.as_path(), |p| p.as_ref());
        let data = f(view)?;
        if fingerprint(&self.root, cancel)? != hash {
            return Err("读取期间外层压缩包变更，请刷新".into());
        }
        Ok((data, hash))
    }
    pub fn rewrite(&self, expected: &str, initial: Change, cancel: &AtomicBool) -> R<String> {
        // Prevent two in-app writers from passing the final revision check at once.
        static WRITE: Mutex<()> = Mutex::new(());
        let _guard = WRITE
            .lock()
            .map_err(|_| io::Error::other("归档写入锁异常"))?;
        if fingerprint(&self.root, cancel)? != expected {
            return Err("外层压缩包已更改，未覆盖；请刷新并重新打开".into());
        }
        let files = self.stage_chain(cancel)?;
        let mut change = initial;
        for (idx, file) in files.iter().enumerate().rev() {
            let path: &Path = file.as_ref();
            rewrite(path, &fingerprint(path, cancel)?, change, cancel)?;
            change = Change::Replace {
                at: self.chain[idx].clone(),
                source: path.to_owned(),
            };
        }
        // The original is replaced only once all intermediate archives succeeded.
        rewrite(&self.root, expected, change, cancel)
    }
}

fn rewrite(src: &Path, expected: &str, change: Change, cancel: &AtomicBool) -> R<String> {
    if fingerprint(src, cancel)? != expected {
        return Err("压缩包已被其他程序修改，未覆盖".into());
    }
    let target = match &change {
        Change::Rename { from, .. } => from,
        Change::Delete(p) | Change::AddDirectory(p) => p,
        Change::AddFile { at, .. } | Change::Replace { at, .. } => at,
    };
    let _ = inner_name(target)?;
    let is_rename = matches!(&change, Change::Rename { .. });
    let Some(parent) = src.parent() else {
        return Err("无法确定来源目录".into());
    };
    let mut z = ZipArchive::new(stream(src)?)?;
    let mut rows = Vec::with_capacity(z.len());
    let mut matched = false;
    for i in 0..z.len() {
        files::check_cancel(cancel)?;
        let e = z.by_index(i)?;
        let old = archive::path(&e)?;
        if e.encrypted()
            || !matches!(
                e.compression(),
                CompressionMethod::Stored | CompressionMethod::Deflated
            )
        {
            return Err("此包有加密条目或不支持重写的压缩方法，原件未改变".into());
        }
        let whole = old == *target;
        let selected = if matches!(&change, Change::Replace { .. }) {
            whole
        } else {
            old.starts_with(target)
        };
        matched |= selected;
        if selected && matches!(&change, Change::Delete(_)) {
            continue;
        }
        let next = if is_rename && selected {
            let Change::Rename { to, .. } = &change else {
                unreachable!()
            };
            let rel = old.strip_prefix(target)?;
            if rel.as_os_str().is_empty() {
                to.clone()
            } else {
                to.join(rel)
            }
        } else {
            old
        };
        let n = inner_name(&next)?;
        let replacement = if selected && let Change::Replace { source, .. } = &change {
            if e.is_dir() {
                return Err("不能用文件替换目录".into());
            }
            Some(source.as_path())
        } else {
            None
        };
        rows.push((i, n, e.is_dir(), replacement));
    }
    if matches!(
        &change,
        Change::Rename { .. } | Change::Delete(_) | Change::Replace { .. }
    ) && !matched
    {
        return Err("找不到要修改的包内条目".into());
    }
    let extra = match &change {
        Change::AddDirectory(p) => Some((inner_name(p)?, true, None)),
        Change::AddFile { at, source } => Some((inner_name(at)?, false, Some(source.as_path()))),
        _ => None,
    };
    validate_tree(
        rows.iter()
            .map(|(_, n, d, _)| (n.clone(), *d))
            .chain(extra.iter().map(|(n, d, _)| (n.clone(), *d))),
    )?;
    let mut tmp = NamedTempFile::new_in(parent)?;
    let mut out = ZipWriter::new(tmp.as_file_mut());
    out.set_raw_comment(z.comment().to_owned().into_boxed_slice());
    let options = |n: &str, m: CompressionMethod, len: u64| {
        let align = if m != CompressionMethod::Stored {
            1
        } else if n.to_ascii_lowercase().ends_with(".so") {
            16384
        } else {
            4
        };
        SimpleFileOptions::default()
            .compression_method(m)
            .large_file(len >= u32::MAX as u64)
            .with_alignment(align)
    };
    for (i, n, dir, replacement) in rows {
        files::check_cancel(cancel)?;
        let mut e = z.by_index(i)?;
        let size = replacement
            .map(|p| p.metadata().map(|m| m.len()))
            .transpose()?
            .unwrap_or(e.size());
        let mut opt = options(&n, e.compression(), size);
        if let Some(t) = e.last_modified() {
            opt = opt.last_modified_time(t);
        }
        if let Some(mode) = e.unix_mode() {
            opt = opt.unix_permissions(mode);
        }
        if dir {
            out.add_directory(n, opt)?;
        } else {
            out.start_file(n, opt)?;
            if let Some(p) = replacement {
                files::copy_stream(stream(p)?, &mut out, cancel)?;
            } else {
                files::copy_stream(&mut e, &mut out, cancel)?;
            }
        }
    }
    if let Some((n, dir, source)) = extra {
        if dir {
            out.add_directory(n, SimpleFileOptions::default())?;
        } else if let Some(p) = source {
            out.start_file(
                &n,
                options(&n, CompressionMethod::Deflated, p.metadata()?.len()),
            )?;
            files::copy_stream(stream(p)?, &mut out, cancel)?;
        }
    }
    out.finish()?.sync_all()?;
    drop(z);
    let revision = fingerprint(tmp.path(), cancel)?;
    // The caller receives the revision it published, never a later writer's revision.
    if fingerprint(src, cancel)? != expected {
        return Err("压缩包已被其他程序修改，未覆盖".into());
    }
    files::check_cancel(cancel)?;
    tmp.persist(src).map_err(|e| e.error)?;
    Ok(revision)
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, io::Write};
    fn single_file_zip(dest: &Path, name: &str, bytes: &[u8]) {
        let mut z = ZipWriter::new(File::create(dest).unwrap());
        z.start_file(name, SimpleFileOptions::default()).unwrap();
        z.write_all(bytes).unwrap();
        z.finish().unwrap();
    }
    #[test]
    fn nested_write_commits_outer_archive_once_and_rejects_stale_revision() {
        let d = tempfile::tempdir().unwrap();
        let inner = d.path().join("inner.zip");
        let mid = d.path().join("mid.zip");
        let outer = d.path().join("outer.zip");
        single_file_zip(&inner, "original.txt", b"original");
        single_file_zip(&mid, "a/inner.zip", &fs::read(inner).unwrap());
        single_file_zip(&outer, "mid.zip", &fs::read(mid).unwrap());
        let s = Source::local(outer.clone())
            .child("mid.zip".into())
            .unwrap()
            .child("a/inner.zip".into())
            .unwrap();
        let stop = AtomicBool::new(false);
        let original = fingerprint(&outer, &stop).unwrap();
        let rev = s
            .rewrite(
                &original,
                Change::Rename {
                    from: "original.txt".into(),
                    to: "updated.txt".into(),
                },
                &stop,
            )
            .unwrap();
        assert_eq!(fingerprint(&outer, &stop).unwrap(), rev);
        assert_ne!(original, rev);
        assert!(
            s.rewrite(&original, Change::Delete("updated.txt".into()), &stop)
                .is_err()
        );
        assert!(
            s.rewrite(&rev, Change::Delete("missing.txt".into()), &stop)
                .is_err()
        );
        let (file, read_rev) = s
            .read(Some(&rev), &stop, |p| {
                stage(p, Path::new("updated.txt"), &stop)
            })
            .unwrap();
        assert_eq!(fs::read(file.path()).unwrap(), b"original");
        assert_eq!(read_rev, rev);
        assert_eq!(fingerprint(&outer, &stop).unwrap(), rev);
        stop.store(true, std::sync::atomic::Ordering::Relaxed);
        assert!(
            s.rewrite(&rev, Change::Delete("updated.txt".into()), &stop)
                .is_err()
        );
        assert_eq!(fingerprint(&outer, &AtomicBool::new(false)).unwrap(), rev);
    }
    #[test]
    fn mutations_alignment_and_external_change() {
        let d = tempfile::tempdir().unwrap();
        let src = d.path().join("sample.apk");
        let local = d.path().join("local.txt");
        fs::write(&local, b"new").unwrap();
        let mut w = ZipWriter::new(File::create(&src).unwrap());
        w.set_comment("example");
        w.add_directory("dir", SimpleFileOptions::default())
            .unwrap();
        w.start_file(
            "dir/a.txt",
            SimpleFileOptions::default().compression_method(CompressionMethod::Deflated),
        )
        .unwrap();
        w.write_all(b"old").unwrap();
        w.start_file(
            "lib/arm64-v8a/libfoo.so",
            SimpleFileOptions::default().compression_method(CompressionMethod::Stored),
        )
        .unwrap();
        w.write_all(b"lib").unwrap();
        w.finish().unwrap();
        let c = AtomicBool::new(false);
        let old = fingerprint(&src, &c).unwrap();
        rewrite(
            &src,
            &old,
            Change::Rename {
                from: "dir".into(),
                to: "改名".into(),
            },
            &c,
        )
        .unwrap();
        assert!(rewrite(&src, &old, Change::Delete("改名".into()), &c).is_err());
        let tmp = stage(&src, Path::new("改名/a.txt"), &c).unwrap();
        assert_eq!(fs::read(tmp.path()).unwrap(), b"old");
        for op in [
            Change::Replace {
                at: "改名/a.txt".into(),
                source: local.clone(),
            },
            Change::AddFile {
                at: "b.txt".into(),
                source: local,
            },
            Change::AddDirectory("空目录".into()),
        ] {
            rewrite(&src, &fingerprint(&src, &c).unwrap(), op, &c).unwrap();
        }
        let tmp = stage(&src, Path::new("改名/a.txt"), &c).unwrap();
        assert_eq!(fs::read(tmp.path()).unwrap(), b"new");
        {
            let mut zip = ZipArchive::new(File::open(&src).unwrap()).unwrap();
            assert_eq!(zip.comment(), b"example");
            assert!(zip.by_name("空目录/").unwrap().is_dir());
            assert_eq!(
                zip.by_name("lib/arm64-v8a/libfoo.so").unwrap().data_start() % 16384,
                0
            );
        }
        let v = fingerprint(&src, &c).unwrap();
        assert!(
            rewrite(
                &src,
                &v,
                Change::Rename {
                    from: "b.txt".into(),
                    to: "改名/a.txt".into()
                },
                &c
            )
            .is_err()
        );
        assert_eq!(fingerprint(&src, &c).unwrap(), v);
        rewrite(&src, &v, Change::Delete("改名".into()), &c).unwrap();
        assert!(stage(&src, Path::new("改名/a.txt"), &c).is_err());
    }
}
