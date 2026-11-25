use serde::{Deserialize, Serialize};
use std::{
    fs::{self, File, OpenOptions},
    io::{self, Write},
    path::{Path, PathBuf},
};
use tempfile::NamedTempFile;

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
pub enum Theme {
    #[default]
    #[serde(alias = "Mt")]
    TwinFile,
    // Old configurations should retain all workspace and appearance preferences.
    #[serde(alias = "Cream", alias = "Bata", alias = "MacOs", alias = "Fluent")]
    Miuix,
    Soft,
    Keynote,
}

impl Theme {
    pub const ALL: [Self; 4] = [Self::TwinFile, Self::Miuix, Self::Soft, Self::Keynote];
    pub fn label(self) -> &'static str {
        match self {
            Self::TwinFile => "TwinFile",
            Self::Miuix => "miuix",
            Self::Soft => "柔和",
            Self::Keynote => "发布会",
        }
    }
}

#[derive(Clone, Copy, Default, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub enum Sort {
    #[default]
    Name,
    Size,
    Modified,
    Extension,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Location {
    Local(PathBuf),
    Computer,
}
impl Location {
    // Older Rust builds still accept the JSON and ignore this non-absolute marker.
    const COMPUTER_MARKER: &str = ":twinfile-computer:";
    pub fn local(&self) -> Option<&Path> {
        match self {
            Self::Local(path) => Some(path),
            Self::Computer => None,
        }
    }
    pub fn display(&self) -> String {
        self.local()
            .map_or_else(|| "此电脑".into(), |p| p.display().to_string())
    }
}
impl Serialize for Location {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Local(path) => path.serialize(serializer),
            Self::Computer => serializer.serialize_str(Self::COMPUTER_MARKER),
        }
    }
}
impl<'de> Deserialize<'de> for Location {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum Stored {
            Path(PathBuf),
            Preview { computer: bool },
        }
        match Stored::deserialize(deserializer)? {
            Stored::Path(p) if p.as_os_str() == Self::COMPUTER_MARKER => Ok(Self::Computer),
            Stored::Path(p) => Ok(Self::Local(p)),
            Stored::Preview { computer: true } => Ok(Self::Computer),
            Stored::Preview { computer: false } => {
                Err(serde::de::Error::custom("invalid computer tab"))
            }
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(default)]
pub struct PaneSettings {
    pub tabs: Vec<Location>,
    pub selected: usize,
    pub sort: Sort,
    pub descending: bool,
}

impl Default for PaneSettings {
    fn default() -> Self {
        Self {
            tabs: vec![],
            selected: 0,
            sort: Sort::Name,
            descending: false,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(default)]
pub struct Settings {
    pub schema: u32,
    pub theme: Theme,
    pub dark: bool,
    pub reduce_motion: bool,
    pub accent: usize,
    pub split: f32,
    pub hidden: bool,
    pub check_updates: bool,
    pub active: usize,
    pub panes: [PaneSettings; 2],
    pub favorites: Vec<PathBuf>,
    pub recent: Vec<PathBuf>,
    pub window_size: Option<[f32; 2]>,
    pub download_dir: Option<PathBuf>,
    pub download_connections: usize,
    pub payload_dir: Option<PathBuf>,
    pub payload_workers: usize,
    pub payload_url: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            schema: 1,
            theme: Theme::default(),
            dark: false,
            reduce_motion: false,
            accent: 0,
            split: 0.5,
            hidden: false,
            check_updates: true,
            active: 0,
            panes: Default::default(),
            favorites: vec![],
            recent: vec![],
            window_size: None,
            download_dir: None,
            download_connections: 16,
            payload_dir: None,
            payload_workers: 4,
            payload_url: None,
        }
    }
}

pub struct Store {
    pub path: PathBuf,
    _lock: File,
    can_save: bool,
}

impl Store {
    pub fn open(dir: &Path) -> io::Result<(Self, Settings, Option<String>)> {
        fs::create_dir_all(dir)?;
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(dir.join("settings.lock"))?;
        lock.try_lock()
            .map_err(|e| io::Error::other(format!("配置目录已有写入者或无法加锁：{e}")))?;
        let path = dir.join("settings.json");
        let decoded = match fs::read(&path) {
            Ok(bytes) => serde_json::from_slice::<Settings>(&bytes)
                .map_err(|e| e.to_string())
                .and_then(|s| {
                    if s.schema == 1 {
                        Ok(s)
                    } else {
                        Err(format!("不支持设置版本 {}", s.schema))
                    }
                }),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(Settings::default()),
            Err(e) => Err(e.to_string()),
        };
        let (settings, error) = match decoded {
            Ok(s) => (s, None),
            Err(e) => (
                Settings::default(),
                Some(format!("无法读取设置，已保留原文件并暂停保存：{e}")),
            ),
        };
        Ok((
            Self {
                path,
                _lock: lock,
                can_save: error.is_none(),
            },
            settings,
            error,
        ))
    }

    pub fn save(&self, s: &Settings) -> io::Result<()> {
        if !self.can_save {
            return Err(io::Error::other("设置文件无法读取，原文件未覆盖"));
        }
        let data = serde_json::to_vec_pretty(s).map_err(io::Error::other)?;
        atomic_replace(&self.path, &data)
    }
}

/// Shared by the workspace and `--downloader`; their workspace locks stay independent.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct DownloadPrefs {
    pub dir: Option<PathBuf>,
    pub connections: usize,
}
impl DownloadPrefs {
    pub fn from_settings(s: &Settings) -> Self {
        Self {
            dir: s.download_dir.clone(),
            connections: s.download_connections.clamp(1, 512),
        }
    }
    pub fn read(root: &Path, fallback: &Self) -> io::Result<Self> {
        match fs::read(root.join("download-prefs.json")) {
            Ok(bytes) => {
                let mut prefs: Self = serde_json::from_slice(&bytes).map_err(io::Error::other)?;
                prefs.connections = prefs.connections.clamp(1, 512);
                Ok(prefs)
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(fallback.clone()),
            Err(e) => Err(e),
        }
    }
    // Merge only what this window edited; an older open window must not restore a stale directory.
    pub fn save_changes(root: &Path, old: &Self, new: &Self) -> io::Result<Self> {
        fs::create_dir_all(root)?;
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(root.join("download-prefs.lock"))?;
        lock.lock()?;
        let mut prefs = Self::read(root, old)?;
        if old.dir != new.dir {
            prefs.dir.clone_from(&new.dir);
        }
        if old.connections != new.connections {
            prefs.connections = new.connections.clamp(1, 512);
        }
        atomic_replace(
            &root.join("download-prefs.json"),
            &serde_json::to_vec_pretty(&prefs).map_err(io::Error::other)?,
        )?;
        Ok(prefs)
    }
    pub fn apply(self, s: &mut Settings) {
        s.download_connections = self.connections;
        s.download_dir = self.dir;
    }
}

pub fn config_dir() -> io::Result<PathBuf> {
    std::env::var_os("APPDATA")
        .filter(|s| !s.is_empty())
        .map(|s| PathBuf::from(s).join("TwinFileRust"))
        .ok_or_else(|| io::Error::other("找不到 APPDATA"))
}

pub fn atomic_replace(path: &Path, data: &[u8]) -> io::Result<()> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let mut staging = NamedTempFile::new_in(parent)?;
    staging.write_all(data)?;
    staging.as_file().sync_all()?;
    staging.persist(path).map_err(|e| e.error)?;
    #[cfg(unix)]
    File::open(parent)?.sync_all()?;
    Ok(())
}
