use super::*;
struct Out {
    local: PathBuf,
    remote: String,
    size: u64,
    time: i64,
    dir: bool,
    first: u64,
    end: u64,
}
fn verify_source(e: &Out, file: &File) -> R<()> {
    let meta = file.metadata()?;
    let stamp = meta
        .modified()
        .ok()
        .and_then(|v| v.duration_since(UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_millis() as i64);
    require(
        meta.len() == e.size && stamp == e.time,
        "发送时来源文件已变化",
    )
}
fn basename(p: &Path) -> R<&str> {
    p.file_name()
        .and_then(|s| s.to_str())
        .ok_or_else(|| bad("文件名不是有效的 Unicode"))
}
fn vacant(p: &Path) -> R<()> {
    match fs::symlink_metadata(p) {
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) => Err(bad(format!("目标已存在，不覆盖：{}", p.display()))),
        Err(e) => Err(e),
    }
}
fn no_link(p: &Path) -> R<fs::Metadata> {
    let m = fs::symlink_metadata(p)?;
    require(!files::is_link(&m), "HFX 不跟随链接或重解析点")?;
    Ok(m)
}
fn parent_dirs(stage: &Path, rel: &str) -> R<()> {
    let mut d = stage.to_owned();
    if let Some((parent, _)) = rel.rsplit_once('/') {
        for n in parent.split('/') {
            d.push(n);
            match fs::create_dir(&d) {
                Ok(()) => {}
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {
                    require(no_link(&d)?.is_dir(), "HFX 路径与文件冲突")?
                }
                Err(e) => return Err(e),
            }
        }
    }
    Ok(())
}
fn guarded<T>(sig: &Signals<'_>, f: impl FnOnce() -> R<T>) -> R<T> {
    let r = f();
    if r.is_err() {
        sig.abort.store(true, Relaxed)
    }
    r
}
fn gather(
    paths: &[PathBuf],
    dest: &str,
    existing: &[Entry],
    stop: &AtomicBool,
) -> R<(Vec<Out>, u64)> {
    require(!paths.is_empty(), "未选择要发送的项目")?;
    let mut todo = Vec::new();
    for p in paths {
        let n = basename(p)?;
        require(
            !existing.iter().any(|e| e.name == n),
            "手机目标已存在，不覆盖",
        )?;
        todo.push((p.clone(), join(dest, n)))
    }
    let mut files = Vec::new();
    let mut total = 0u64;
    let mut blocks = 0;
    let mut names = HashSet::new();
    let mut bytes = 0;
    while let Some((p, remote)) = todo.pop() {
        files::check_cancel(stop)?;
        bytes += remote.len();
        require(
            files.len() < MAX_FILES
                && bytes < MAX_META
                && remote.len() <= 65535
                && names.insert(remote.clone()),
            "文件数、路径长度超限或目标重名",
        )?;
        let m = no_link(&p)?;
        let dir = m.is_dir();
        require(dir || m.is_file(), "HFX 只支持普通文件和目录")?;
        let time = m
            .modified()
            .ok()
            .and_then(|v| v.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |d| d.as_millis() as i64);
        let size = if dir { 0 } else { m.len() };
        require(size <= MAX_FILE, "单文件超过 HFX 限制")?;
        total = total.checked_add(size).ok_or_else(|| bad("总大小溢出"))?;
        let first = blocks;
        blocks += size.div_ceil(BLOCK).max(1);
        if dir {
            for item in fs::read_dir(&p)? {
                let child = item?.path();
                let n = basename(&child)?;
                let child_remote = join(&remote, n);
                require(todo.len() + files.len() < MAX_FILES, "HFX 项目数超限")?;
                todo.push((child, child_remote));
            }
        }
        files.push(Out {
            local: p,
            remote,
            size,
            time,
            dir,
            first,
            end: blocks,
        });
    }
    Ok((files, total))
}
#[derive(Default)]
struct InState {
    files: HashMap<i32, InFile>,
    names: HashSet<String>,
    total: u64,
    bytes: usize,
    blocks_seen: u64,
}
struct InFile {
    rel: String,
    size: u64,
    mtime: i64,
    dir: bool,
    blocks: HashSet<i32>,
}
fn remote_rel(raw: &str, base: &str, roots: &HashSet<String>) -> R<String> {
    let norm = raw.replace('\\', "/");
    let rel = norm.strip_prefix(base).unwrap_or(&norm);
    require(
        rel.len() <= 32766
            && rel.split('/').all(|s| files::safe_name(s).is_ok())
            && roots.contains(rel.split('/').next().unwrap_or("")),
        "手机文件越出本次选择或名称无效",
    )?;
    Ok(rel.to_owned())
}
#[cfg(windows)]
fn free_space(p: &Path) -> R<u64> {
    use std::os::windows::ffi::OsStrExt;
    let w: Vec<u16> = p.as_os_str().encode_wide().chain([0]).collect();
    let mut free = 0;
    let ok = unsafe {
        windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW(
            w.as_ptr(),
            &mut free,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(free)
    }
}
#[cfg(not(windows))]
fn free_space(_: &Path) -> R<u64> {
    Ok(u64::MAX)
}
impl Session {
    pub(super) fn send_files(
        &self,
        local: &[PathBuf],
        remote: &str,
        stop: &AtomicBool,
        progress: &(dyn Fn(u64, u64) + Sync),
    ) -> R<()> {
        let existing = self.read_dir(remote, stop)?;
        let (entries, total) = gather(local, remote, &existing, stop)?;
        progress(0, total);
        let sig = Signals::new(stop);
        let mut ctrl = Wire::new(&self.control, &sig);
        ctrl.send(&10i16.to_be_bytes())?;
        let next = AtomicU64::new(0);
        let done = Mutex::new(0u64);
        std::thread::scope(|scope| -> R<()> {
            let phone = scope.spawn(|| {
                guarded(&sig, || {
                    let mut w = Wire::new(&self.control, &sig);
                    if !w.flag()? {
                        return Err(bad(format!("手机写入失败：{}", w.string()?)));
                    }
                    Ok(())
                })
            });
            let mut tasks = vec![];
            for lane in &self.lanes {
                tasks.push(scope.spawn(|| {
                    guarded(&sig, || {
                        let mut w = Wire::new(lane, &sig);
                        let mut opened: Option<(usize, File)> = None;
                        let mut frame = Vec::with_capacity(BLOCK as usize + 1024);
                        loop {
                            sig.check()?;
                            let seq = next.fetch_add(1, Relaxed);
                            let index = entries.partition_point(|e| e.end <= seq);
                            let Some(e) = entries.get(index) else { break };
                            let block = (seq - e.first) as u32;
                            frame.clear();
                            frame.extend_from_slice(
                                &(if e.dir { 1i16 } else { 0i16 }).to_be_bytes(),
                            );
                            frame.extend_from_slice(&(index as i32).to_be_bytes());
                            txt(&mut frame, &e.remote)?;
                            frame.extend_from_slice(&e.time.to_be_bytes());
                            let mut n = 0;
                            if !e.dir {
                                frame.extend_from_slice(&e.size.to_be_bytes());
                                frame.extend_from_slice(&block.to_be_bytes());
                                let offset = block as u64 * BLOCK;
                                n = (e.size - offset).min(BLOCK);
                                frame.extend_from_slice(&(n as i32).to_be_bytes());
                                if opened.as_ref().is_none_or(|(i, _)| *i != index) {
                                    if let Some((last, file)) = opened.take() {
                                        verify_source(&entries[last], &file)?;
                                    }
                                    no_link(&e.local)?;
                                    let f = File::open(&e.local)?;
                                    verify_source(e, &f)?;
                                    opened = Some((index, f));
                                }
                                let file = &mut opened.as_mut().unwrap().1;
                                file.seek(SeekFrom::Start(offset))?;
                                let len = frame.len();
                                frame.resize(len + n as usize, 0);
                                file.read_exact(&mut frame[len..])?;
                            }
                            w.send(&frame)?;
                            if !e.dir {
                                let mut d = done.lock().unwrap();
                                *d += n;
                                progress(*d, total)
                            }
                        }
                        if let Some((last, file)) = opened {
                            verify_source(&entries[last], &file)?;
                        }
                        w.send(&3i16.to_be_bytes())?;
                        Ok(())
                    })
                }));
            }
            let mut err = None;
            for t in tasks {
                if let Err(e) = t.join().map_err(|_| bad("传输线程退出"))? {
                    err.get_or_insert(e);
                }
            }
            let phone = phone.join().map_err(|_| bad("控制线程退出"))?;
            if let Err(e) = phone {
                if e.to_string().starts_with("手机写入失败") || err.is_none() {
                    return Err(e);
                }
            }
            if let Some(e) = err {
                return Err(e);
            }
            Ok(())
        })?;
        ctrl.send(&[1])?;
        progress(total, total);
        Ok(())
    }
    pub(super) fn receive_files(
        &self,
        remote: &[String],
        src: &str,
        local: &Path,
        stop: &AtomicBool,
        progress: &(dyn Fn(u64, u64) + Sync),
    ) -> R<()> {
        directory(src)?;
        require(
            !remote.is_empty() && remote.len() <= MAX_FILES,
            "未选择要接收的项目或数量超限",
        )?;
        let src_prefix = format!("{}/", src.trim_end_matches('/'));
        let mut roots = HashSet::new();
        let mut folded = HashSet::new();
        for path in remote {
            let n = path
                .strip_prefix(&src_prefix)
                .ok_or_else(|| bad("仅可接收同一手机目录中的项目"))?;
            files::safe_name(n)?;
            require(
                folded.insert(n.to_lowercase()),
                "手机项目映射到同一 Windows 名称",
            )?;
            roots.insert(n.to_owned());
            vacant(&local.join(n))?;
        }
        let local = std::path::absolute(local)?;
        require(local.is_dir(), "请选择现有目标目录")?;
        let staging = tempfile::Builder::new()
            .prefix(".twinfile-hfx-")
            .tempdir_in(&local)?;
        let stage = staging.path();
        let base = format!(
            "{}/",
            stage
                .to_str()
                .ok_or_else(|| bad("暂存路径编码无效"))?
                .replace('\\', "/")
        );
        let available = free_space(stage)?;
        let mut req = 11i16.to_be_bytes().to_vec();
        req.extend_from_slice(&(remote.len() as i32).to_be_bytes());
        for p in remote {
            txt(&mut req, p)?;
            require(req.len() < MAX_META, "所选路径超限")?;
        }
        txt(&mut req, &base.replace('/', "\\"))?;
        req.extend_from_slice(&1i32.to_be_bytes());
        txt(&mut req, &src_prefix)?;
        let sig = Signals::new(stop);
        let mut ctrl = Wire::new(&self.control, &sig);
        ctrl.send(&req)?;
        let meta = Mutex::new(InState::default());
        let received = Mutex::new(0u64);
        std::thread::scope(|scope| -> R<()> {
            let mut tasks = vec![];
            for lane in &self.lanes {
                tasks.push(scope.spawn(|| {
                    guarded(&sig, || {
                        let mut w = Wire::new(lane, &sig);
                        w.first = true;
                        let mut buffer = vec![0u8; BLOCK as usize];
                        let mut opened: Option<(i32, File)> = None;
                        loop {
                            let typ = i16::from_be_bytes(w.read()?);
                            if typ == 3 {
                                break;
                            }
                            let dir = typ == 1;
                            require(dir || typ == 0, "手机发来未知传输帧")?;
                            let id = w.i32()?;
                            require(id >= 0, "文件编号无效")?;
                            let raw = w.string()?;
                            let rel = remote_rel(&raw, &base, &roots)?;
                            let mtime = w.i64()?;
                            let (size, block, n) = if dir {
                                (0, -1, 0)
                            } else {
                                let size = w.i64()?;
                                let b = w.i32()?;
                                let n = w.i32()?;
                                require(
                                    (0..=MAX_FILE as i64).contains(&size) && b >= 0 && n >= 0,
                                    "传输块头无效",
                                )?;
                                let s = size as u64;
                                let pos = b as u64 * BLOCK;
                                require(
                                    pos <= s
                                        && (s == 0 || pos < s)
                                        && n as u64 == (s - pos).min(BLOCK),
                                    "传输块偏移或长度无效",
                                )?;
                                w.get(&mut buffer[..n as usize])?;
                                (s, b, n as usize)
                            };
                            let path = stage.join(&rel);
                            {
                                let mut m = meta.lock().unwrap();
                                if !m.files.contains_key(&id) {
                                    m.bytes += rel.len();
                                    require(
                                        m.bytes <= MAX_META
                                            && m.files.len() < MAX_FILES
                                            && m.names.insert(rel.to_lowercase()),
                                        "文件重复或路径信息超限",
                                    )?;
                                    m.total = m
                                        .total
                                        .checked_add(size)
                                        .filter(|v| *v <= available)
                                        .ok_or_else(|| bad("空间不足"))?;
                                    parent_dirs(stage, &rel)?;
                                    if dir {
                                        match fs::create_dir(&path) {
                                            Ok(()) => {}
                                            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {
                                                require(no_link(&path)?.is_dir(), "目录冲突")?
                                            }
                                            Err(e) => return Err(e),
                                        }
                                    } else {
                                        let f = OpenOptions::new()
                                            .write(true)
                                            .create_new(true)
                                            .open(&path)?;
                                        f.set_len(size)?;
                                        opened = Some((id, f));
                                    }
                                    m.files.insert(
                                        id,
                                        InFile {
                                            rel: rel.clone(),
                                            size,
                                            mtime,
                                            dir,
                                            blocks: HashSet::new(),
                                        },
                                    );
                                }
                                m.blocks_seen += 1;
                                require(m.blocks_seen <= 8_000_000, "块记录超过上限")?;
                                let f = m.files.get_mut(&id).unwrap();
                                require(
                                    f.rel == rel
                                        && f.size == size
                                        && f.dir == dir
                                        && f.mtime == mtime
                                        && f.blocks.insert(block),
                                    "重复块或文件元数据不一致",
                                )?;
                            }
                            if !dir {
                                if opened.as_ref().is_none_or(|(i, _)| *i != id) {
                                    opened =
                                        Some((id, OpenOptions::new().write(true).open(&path)?));
                                }
                                let file = &mut opened.as_mut().unwrap().1;
                                file.seek(SeekFrom::Start(block as u64 * BLOCK))?;
                                file.write_all(&buffer[..n])?;
                                let mut d = received.lock().unwrap();
                                *d += n as u64;
                                progress(*d, 0);
                            }
                        }
                        Ok(())
                    })
                }));
            }
            let mut err = None;
            for t in tasks {
                if let Err(e) = t.join().map_err(|_| bad("传输线程退出"))? {
                    if err.is_none() || e.to_string() != "其它 HFX 通道已中止" {
                        err = Some(e)
                    }
                }
            }
            if let Some(e) = err { Err(e) } else { Ok(()) }
        })?;
        let m = meta.into_inner().unwrap();
        for f in m.files.values() {
            files::check_cancel(stop)?;
            if !f.dir {
                require(
                    f.blocks.len() as u64 == f.size.div_ceil(BLOCK).max(1),
                    "手机回传缺少文件块",
                )?;
                if let Ok(file) = OpenOptions::new().write(true).open(stage.join(&f.rel)) {
                    if f.mtime >= 0 {
                        if let Some(t) = SystemTime::UNIX_EPOCH
                            .checked_add(Duration::from_millis(f.mtime as u64))
                        {
                            let _ = file.set_modified(t);
                        }
                    }
                }
            }
        }
        for n in &roots {
            require(stage.join(n).exists(), "手机未返回所选项目")?;
        }
        ctrl.send(&[1])?;
        if !ctrl.flag()? {
            return Err(bad(format!("手机读取失败：{}", ctrl.string()?)));
        }
        files::check_cancel(stop)?;
        let mut moved: Vec<String> = vec![];
        for n in &roots {
            if let Err(e) = files::check_cancel(stop)
                .and_then(|_| files::rename_new(&stage.join(n), &local.join(n)))
            {
                let mut left = vec![];
                for item in moved.iter().rev() {
                    if files::rename_new(&local.join(item), &stage.join(item)).is_err() {
                        left.push(item.clone())
                    }
                }
                if !left.is_empty() {
                    return Err(bad(format!(
                        "提交失败：{e}；以下新项目仍在目标目录：{}",
                        left.join(", ")
                    )));
                }
                return Err(e);
            }
            moved.push(n.clone())
        }
        progress(m.total, m.total);
        Ok(())
    }
}
