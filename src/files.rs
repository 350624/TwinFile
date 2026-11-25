use crate::config::Sort;
use sha2::Digest;
use std::{
    cmp::Ordering,
    fs::{self, File, Metadata},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering::Relaxed},
    time::SystemTime,
};
use tempfile::{Builder, NamedTempFile};

#[derive(Clone, Debug)]
pub struct Entry {
    pub path: PathBuf,
    pub name: String,
    pub dir: bool,
    pub size: u64,
    pub modified: Option<SystemTime>,
}

pub fn list(path: &Path, hidden: bool) -> io::Result<Vec<Entry>> {
    fs::read_dir(path)?
        .map(|item| {
            let e = item?;
            let name = e.file_name().to_string_lossy().into_owned();
            let m = e.metadata()?;
            #[cfg(windows)]
            let hidden_attr = {
                use std::os::windows::fs::MetadataExt;
                m.file_attributes() & 2 != 0
            };
            #[cfg(not(windows))]
            let hidden_attr = false;
            if !hidden && (hidden_attr || name.starts_with('.')) {
                return Ok(None);
            }
            let dir = m.is_dir() || (is_link(&m) && e.path().is_dir());
            Ok(Some(Entry {
                path: e.path(),
                name,
                dir,
                size: m.len(),
                modified: m.modified().ok(),
            }))
        })
        .filter_map(Result::transpose)
        .collect()
}

/// Explorer folder metadata is not a browsable document. Keep this rule out of
/// directory comparison and file operations, which need the actual contents.
pub fn list_for_display(path: &Path, hidden: bool) -> io::Result<Vec<Entry>> {
    let mut entries = list(path, hidden)?;
    entries.retain(|e| e.dir || !e.name.eq_ignore_ascii_case("desktop.ini"));
    Ok(entries)
}

pub fn sort(entries: &mut [Entry], key: Sort, desc: bool) {
    entries.sort_by_cached_key(|e| {
        let x = match key {
            Sort::Name => (0, None, String::new()),
            Sort::Size => (e.size, None, String::new()),
            Sort::Modified => (0, e.modified, String::new()),
            Sort::Extension => (
                0,
                None,
                e.path
                    .extension()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .to_lowercase(),
            ),
        };
        (
            !e.dir,
            if desc {
                Direction::Desc((x, e.name.to_lowercase(), e.name.clone()))
            } else {
                Direction::Asc((x, e.name.to_lowercase(), e.name.clone()))
            },
        )
    })
}
#[derive(PartialEq, Eq)]
enum Direction<T> {
    Asc(T),
    Desc(T),
}
impl<T: Ord> Ord for Direction<T> {
    fn cmp(&self, o: &Self) -> Ordering {
        match (self, o) {
            (Self::Asc(a), Self::Asc(b)) => a.cmp(b),
            (Self::Desc(a), Self::Desc(b)) => b.cmp(a),
            (Self::Asc(_), _) => Ordering::Less,
            _ => Ordering::Greater,
        }
    }
}
impl<T: Ord> PartialOrd for Direction<T> {
    fn partial_cmp(&self, o: &Self) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

pub fn safe_name(s: &str) -> io::Result<&str> {
    let stem = s
        .split('.')
        .next()
        .unwrap_or_default()
        .trim_end()
        .to_ascii_uppercase();
    let numbered = ["COM", "LPT"].iter().any(|p| {
        stem.strip_prefix(p).is_some_and(|n| {
            matches!(
                n,
                "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³"
            )
        })
    });
    if s.is_empty()
        || s.ends_with([' ', '.'])
        || s.chars()
            .any(|c| c.is_control() || "<>:\"/\\|?*".contains(c))
        || numbered
        || matches!(
            stem.as_str(),
            "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$"
        )
    {
        Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "文件名为空、包含非法字符或属于 Windows 保留名称",
        ))
    } else {
        Ok(s)
    }
}

pub(crate) fn is_link(meta: &Metadata) -> bool {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        meta.file_attributes() & 0x400 != 0
    }
    #[cfg(not(windows))]
    {
        meta.file_type().is_symlink()
    }
}

pub fn check_cancel(cancel: &AtomicBool) -> io::Result<()> {
    if cancel.load(Relaxed) {
        Err(io::Error::new(io::ErrorKind::Interrupted, "已取消"))
    } else {
        Ok(())
    }
}

#[cfg(windows)]
pub fn rename_new(src: &Path, dst: &Path) -> io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    let wide = |p: &Path| {
        p.as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect::<Vec<_>>()
    };
    let a = wide(src);
    let b = wide(dst);
    if a[..a.len() - 1].contains(&0) || b[..b.len() - 1].contains(&0) {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "路径中有 NUL"));
    }
    if unsafe { windows_sys::Win32::Storage::FileSystem::MoveFileExW(a.as_ptr(), b.as_ptr(), 0) }
        == 0
    {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
#[cfg(not(windows))]
pub fn rename_new(src: &Path, dst: &Path) -> io::Result<()> {
    if dst.symlink_metadata().is_ok() {
        return Err(io::ErrorKind::AlreadyExists.into());
    }
    fs::rename(src, dst)
}

pub(crate) fn copy_stream<R: Read, W: Write>(
    src: R,
    dst: W,
    cancel: &AtomicBool,
) -> io::Result<()> {
    copy_stream_metered(src, dst, cancel, &|_| {})
}
fn copy_stream_metered<R: Read, W: Write>(
    mut src: R,
    mut dst: W,
    cancel: &AtomicBool,
    written: &dyn Fn(u64),
) -> io::Result<()> {
    let mut buf = vec![0; 256 * 1024];
    loop {
        check_cancel(cancel)?;
        let n = src.read(&mut buf)?;
        if n == 0 {
            return Ok(());
        }
        dst.write_all(&buf[..n])?;
        written(n as u64);
    }
}

fn copy_tree(src: &Path, dst: &Path, cancel: &AtomicBool, written: &dyn Fn(u64)) -> io::Result<()> {
    check_cancel(cancel)?;
    let metadata = src.symlink_metadata()?;
    if is_link(&metadata) {
        return Err(io::Error::other(format!(
            "不跟随链接或重解析点：{}",
            src.display()
        )));
    }
    if metadata.is_dir() {
        fs::create_dir(dst)?;
        for e in fs::read_dir(src)? {
            let e = e?;
            copy_tree(&e.path(), &dst.join(e.file_name()), cancel, written)?;
        }
    } else if metadata.is_file() {
        let out = File::create_new(dst)?;
        copy_stream_metered(File::open(src)?, &out, cancel, written)?;
        out.sync_all()?;
    } else {
        return Err(io::Error::other("不支持的文件类型"));
    }
    Ok(())
}

pub fn transfer(
    src: &Path,
    directory: &Path,
    move_file: bool,
    cancel: &AtomicBool,
) -> io::Result<PathBuf> {
    transfer_metered(src, directory, move_file, cancel, &|| Ok(()), &|_| {})
}
/// `before_copy` runs only for byte copies, never for a same-volume rename.
pub fn transfer_metered(
    src: &Path,
    directory: &Path,
    move_file: bool,
    cancel: &AtomicBool,
    before_copy: &dyn Fn() -> io::Result<()>,
    written: &dyn Fn(u64),
) -> io::Result<PathBuf> {
    check_cancel(cancel)?;
    let dst = directory.join(
        src.file_name()
            .ok_or_else(|| io::Error::other("不能传输根目录"))?,
    );
    let src_real = src.canonicalize()?;
    let dir_real = directory.canonicalize()?;
    if dir_real.starts_with(&src_real) || dst.symlink_metadata().is_ok() {
        return Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "目标已存在或在来源内部",
        ));
    }
    let meta = src.symlink_metadata()?;
    if is_link(&meta) {
        return Err(io::Error::other("不传输链接或重解析点"));
    }
    if move_file {
        match rename_new(&src_real, &dir_real.join(src.file_name().unwrap())) {
            Ok(()) => return Ok(dst),
            Err(e) if e.kind() == io::ErrorKind::CrossesDevices => {}
            Err(e) => return Err(e),
        }
    }
    before_copy()?;
    if meta.is_dir() {
        let stage = Builder::new().prefix(".twinfile-").tempdir_in(&dir_real)?;
        let start = stage.path().join("data");
        copy_tree(src, &start, cancel, written)?;
        check_cancel(cancel)?;
        rename_new(&start, &dir_real.join(dst.file_name().unwrap()))?;
    } else if meta.is_file() {
        let mut stage = NamedTempFile::new_in(&dir_real)?;
        copy_stream_metered(File::open(src)?, &mut stage, cancel, written)?;
        stage.as_file().sync_all()?;
        check_cancel(cancel)?;
        stage.persist_noclobber(&dst).map_err(|e| e.error)?;
    } else {
        return Err(io::Error::other("不支持的文件类型"));
    }
    if move_file {
        let removed = if meta.is_dir() {
            fs::remove_dir_all(src)
        } else {
            fs::remove_file(src)
        };
        removed.map_err(|e| {
            io::Error::other(format!(
                "已复制到 {}，但未能完整删除原件：{e}",
                dst.display()
            ))
        })?;
    }
    Ok(dst)
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct Totals {
    pub bytes: u64,
    pub files: u64,
    pub directories: u64,
    pub links_skipped: u64,
    pub errors: u64,
}

/// Totals use logical bytes, include hidden descendants, and never traverse links.
pub fn aggregate(paths: &[PathBuf], cancel: &AtomicBool) -> io::Result<Totals> {
    let mut roots = paths.to_vec();
    roots.sort_by(|a, b| {
        a.components()
            .count()
            .cmp(&b.components().count())
            .then_with(|| a.cmp(b))
    });
    roots.dedup();
    let mut effective: Vec<PathBuf> = vec![];
    let mut totals = Totals::default();
    for root in roots {
        check_cancel(cancel)?;
        if effective.iter().any(|p| root.starts_with(p)) {
            continue;
        }
        let mut pending = vec![root.clone()];
        if root
            .symlink_metadata()
            .is_ok_and(|m| m.is_dir() && !is_link(&m))
        {
            effective.push(root.clone());
        }
        while let Some(path) = pending.pop() {
            check_cancel(cancel)?;
            let Ok(meta) = path.symlink_metadata() else {
                totals.errors += 1;
                continue;
            };
            if is_link(&meta) {
                totals.links_skipped += 1;
            } else if meta.is_dir() {
                if path != root {
                    totals.directories += 1;
                }
                match fs::read_dir(&path) {
                    Ok(children) => {
                        for entry in children {
                            check_cancel(cancel)?;
                            match entry {
                                Ok(e) => pending.push(e.path()),
                                Err(_) => totals.errors += 1,
                            }
                        }
                    }
                    Err(_) => totals.errors += 1,
                }
            } else if meta.is_file() {
                totals.files += 1;
                totals.bytes = totals
                    .bytes
                    .checked_add(meta.len())
                    .ok_or_else(|| io::Error::other("总字节数超出范围"))?;
            } else {
                totals.errors += 1;
            }
        }
    }
    Ok(totals)
}

pub fn checksum(path: &Path, cancel: &AtomicBool) -> io::Result<[String; 3]> {
    let (mut md, mut s1, mut s2) = (md5::Md5::new(), sha1::Sha1::new(), sha2::Sha256::new());
    let mut f = File::open(path)?;
    let mut buf = vec![0; 256 * 1024];
    loop {
        check_cancel(cancel)?;
        let n = f.read(&mut buf)?;
        if n == 0 {
            break;
        }
        md.update(&buf[..n]);
        s1.update(&buf[..n]);
        s2.update(&buf[..n]);
    }
    Ok([
        format!("{:x}", md.finalize()),
        format!("{:x}", s1.finalize()),
        format!("{:x}", s2.finalize()),
    ])
}

#[derive(Clone, Debug)]
pub struct Volume {
    pub root: PathBuf,
    pub label: String,
    pub kind: VolumeKind,
    pub capacity: Option<(u64, u64)>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VolumeKind {
    Fixed,
    Removable,
    Optical,
    RamDisk,
}

pub fn scan_volumes() -> std::sync::mpsc::Receiver<Vec<Volume>> {
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let _ = tx.send(volumes());
    });
    rx
}

#[cfg(windows)]
fn volumes() -> Vec<Volume> {
    use windows_sys::Win32::Storage::FileSystem::{
        GetDiskFreeSpaceExW, GetDriveTypeW, GetLogicalDrives, GetVolumeInformationW,
    };
    let mask = unsafe { GetLogicalDrives() };
    (0..26)
        .filter(|&i| mask & (1 << i) != 0)
        .filter_map(|i| {
            let c = b'A' + i as u8;
            let path = [c as u16, 58, 92, 0];
            let kind = match unsafe { GetDriveTypeW(path.as_ptr()) } {
                2 => VolumeKind::Removable,
                3 => VolumeKind::Fixed,
                5 => VolumeKind::Optical,
                6 => VolumeKind::RamDisk,
                _ => return None,
            };
            let mut label = [0u16; 261];
            let mut available = 0;
            let mut total = 0;
            let (named, sized) = unsafe {
                let named = GetVolumeInformationW(
                    path.as_ptr(),
                    label.as_mut_ptr(),
                    label.len() as u32,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    0,
                );
                let sized = GetDiskFreeSpaceExW(
                    path.as_ptr(),
                    &mut available,
                    &mut total,
                    std::ptr::null_mut(),
                );
                (named != 0, sized != 0)
            };
            let label = if named {
                String::from_utf16_lossy(
                    &label[..label.iter().position(|&w| w == 0).unwrap_or(label.len())],
                )
            } else {
                String::new()
            };
            Some(Volume {
                root: PathBuf::from(format!("{}:\\", c as char)),
                label,
                kind,
                capacity: sized.then_some((available, total)),
            })
        })
        .collect()
}

#[cfg(not(windows))]
fn volumes() -> Vec<Volume> {
    vec![Volume {
        root: "/".into(),
        label: String::new(),
        kind: VolumeKind::Fixed,
        capacity: None,
    }]
}
