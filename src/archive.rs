use std::{
    fs::{self, File},
    io::{self, Read},
    path::{Path, PathBuf},
    sync::atomic::AtomicBool,
};
use tempfile::{Builder, NamedTempFile};
use zip::{ZipArchive, ZipWriter, write::SimpleFileOptions};
type R<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;
#[derive(Clone, Debug)]
pub struct Item {
    pub path: PathBuf,
    pub size: u64,
    pub dir: bool,
}
fn open(src: &Path) -> R<ZipArchive<File>> {
    Ok(ZipArchive::new(File::open(src)?)?)
}
pub(crate) fn path<R: Read>(entry: &zip::read::ZipFile<'_, R>) -> RResult<PathBuf> {
    let p = entry
        .enclosed_name()
        .ok_or_else(|| io::Error::other("归档中含不安全路径"))?;
    if p.as_os_str().is_empty()
        || p.components().any(|c| match c {
            std::path::Component::Normal(s) => s
                .to_str()
                .is_none_or(|s| crate::files::safe_name(s).is_err()),
            _ => true,
        })
    {
        return Err(io::Error::other("归档中含不安全名称"));
    }
    if entry.is_symlink() {
        return Err(io::Error::other("归档中含符号链接"));
    }
    Ok(p)
}
type RResult<T> = io::Result<T>;
pub fn list(src: &Path) -> R<Vec<Item>> {
    let mut z = open(src)?;
    (0..z.len())
        .map(|i| {
            let e = z.by_index(i)?;
            Ok(Item {
                path: path(&e)?,
                size: e.size(),
                dir: e.is_dir(),
            })
        })
        .collect()
}
pub fn extract(src: &Path, destination: &Path, cancel: &AtomicBool) -> R<()> {
    let parent = destination.parent().ok_or("缺少解压目标目录")?;
    let dir = Builder::new()
        .prefix(".twinfile-unzip-")
        .tempdir_in(parent)?;
    let root = dir.path().join("data");
    fs::create_dir(&root)?;
    let mut z = open(src)?;
    for i in 0..z.len() {
        crate::files::check_cancel(cancel)?;
        let mut e = z.by_index(i)?;
        let out = root.join(path(&e)?);
        if e.is_dir() {
            fs::create_dir_all(out)?;
        } else {
            fs::create_dir_all(out.parent().ok_or("无效路径")?)?;
            let mut f = File::create_new(out)?;
            crate::files::copy_stream(&mut e, &mut f, cancel)?;
            f.sync_all()?;
        }
    }
    crate::files::check_cancel(cancel)?;
    crate::files::rename_new(&root, destination)?;
    Ok(())
}
pub fn create(paths: &[PathBuf], dst: &Path, cancel: &AtomicBool) -> R<()> {
    fn add(w: &mut ZipWriter<&mut File>, p: &Path, name: String, c: &AtomicBool) -> R<()> {
        crate::files::check_cancel(c)?;
        let m = p.symlink_metadata()?;
        if crate::files::is_link(&m) {
            return Err("不能压缩链接或重解析点".into());
        }
        let opt = SimpleFileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated)
            .large_file(true);
        if m.is_dir() {
            w.add_directory(format!("{name}/"), opt)?;
            for e in p.read_dir()? {
                let e = e?;
                let n = e.file_name();
                let n = n.to_str().ok_or("无效名称")?;
                crate::files::safe_name(n)?;
                add(w, &e.path(), format!("{name}/{n}"), c)?;
            }
        } else if m.is_file() {
            w.start_file(name, opt)?;
            crate::files::copy_stream(File::open(p)?, w, c)?;
        } else {
            return Err("不支持的文件类型".into());
        }
        Ok(())
    }
    let parent = dst.parent().ok_or("缺少目标目录")?;
    let mut tmp = NamedTempFile::new_in(parent)?;
    let mut w = ZipWriter::new(tmp.as_file_mut());
    for p in paths {
        if parent.canonicalize()?.starts_with(p.canonicalize()?) {
            return Err("不能将待压缩目录写入其内部".into());
        }
        let name = p.file_name().and_then(|n| n.to_str()).ok_or("缺少文件名")?;
        crate::files::safe_name(name)?;
        add(&mut w, p, name.to_owned(), cancel)?;
    }
    w.finish()?.sync_all()?;
    crate::files::check_cancel(cancel)?;
    tmp.persist_noclobber(dst).map_err(|e| e.error)?;
    Ok(())
}
