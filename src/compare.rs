use crate::files;
use std::{
    collections::{BTreeMap, BTreeSet},
    io,
    path::{Path, PathBuf},
    sync::atomic::AtomicBool,
    time::SystemTime,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Rule {
    Size,
    Modified,
    Sha256,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    LeftOnly,
    RightOnly,
    Same,
    Different,
    TypeMismatch,
}
impl State {
    pub const fn name(self) -> &'static str {
        match self {
            Self::LeftOnly => "仅左侧",
            Self::RightOnly => "仅右侧",
            Self::Same => "相同",
            Self::Different => "不同",
            Self::TypeMismatch => "类型不同",
        }
    }
}
#[derive(Debug)]
pub struct Row {
    pub relative: PathBuf,
    pub state: State,
    pub directory: bool,
}
#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    File,
    Directory,
    Special,
}
struct Meta {
    kind: Kind,
    size: u64,
    mtime: Option<SystemTime>,
}
fn collect(base: &Path, hidden: bool, c: &AtomicBool) -> io::Result<BTreeMap<PathBuf, Meta>> {
    let mut pending = vec![base.to_owned()];
    let mut m = BTreeMap::new();
    while let Some(p) = pending.pop() {
        files::check_cancel(c)?;
        for e in files::list(&p, hidden)? {
            if m.len() >= 200_000 {
                return Err(io::Error::other("目录比较超过 20 万项"));
            }
            let meta = e.path.symlink_metadata()?;
            let kind = if files::is_link(&meta) {
                Kind::Special
            } else if e.dir {
                pending.push(e.path.clone());
                Kind::Directory
            } else if meta.is_file() {
                Kind::File
            } else {
                Kind::Special
            };
            let k = e
                .path
                .strip_prefix(base)
                .map_err(io::Error::other)?
                .to_owned();
            m.insert(
                k,
                Meta {
                    kind,
                    size: e.size,
                    mtime: e.modified,
                },
            );
        }
    }
    Ok(m)
}
pub fn directory(
    left: &Path,
    right: &Path,
    hidden: bool,
    rule: Rule,
    c: &AtomicBool,
) -> io::Result<Vec<Row>> {
    let (a, b) = (collect(left, hidden, c)?, collect(right, hidden, c)?);
    let keys = a.keys().chain(b.keys()).collect::<BTreeSet<_>>();
    let mut rows = Vec::with_capacity(keys.len());
    for k in keys {
        files::check_cancel(c)?;
        let (state, directory) = match (a.get(k), b.get(k)) {
            (Some(x), None) => (State::LeftOnly, x.kind == Kind::Directory),
            (None, Some(y)) => (State::RightOnly, y.kind == Kind::Directory),
            (Some(x), Some(y)) if x.kind != y.kind => (State::TypeMismatch, false),
            (Some(x), Some(_)) if x.kind == Kind::Directory => (State::Same, true),
            (Some(x), Some(y)) => {
                let same = x.kind == Kind::File
                    && match rule {
                        Rule::Size => x.size == y.size,
                        Rule::Modified => {
                            x.size == y.size && x.mtime.is_some() && x.mtime == y.mtime
                        }
                        Rule::Sha256 => {
                            x.size == y.size
                                && files::checksum(&left.join(k), c)?[2]
                                    == files::checksum(&right.join(k), c)?[2]
                        }
                    };
                ((if same { State::Same } else { State::Different }), false)
            }
            _ => unreachable!(),
        };
        rows.push(Row {
            relative: k.clone(),
            state,
            directory,
        });
    }
    Ok(rows)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Line {
    Equal(usize, usize),
    Remove(usize),
    Add(usize),
    Replace(usize, usize),
}
pub fn lines(left: &str, right: &str, ignore_ws: bool, c: &AtomicBool) -> io::Result<Vec<Line>> {
    let (a, b) = (
        left.split('\n').collect::<Vec<_>>(),
        right.split('\n').collect::<Vec<_>>(),
    );
    let n = a.len() + 1;
    let m = b.len() + 1;
    let cells = n
        .checked_mul(m)
        .filter(|n| *n <= 4_000_000)
        .ok_or_else(|| io::Error::other("文本比较超过 400 万对齐单元"))?;
    let normal = |s: &str| {
        s.chars()
            .filter(|ch| !ch.is_whitespace())
            .collect::<String>()
    };
    let keys = ignore_ws.then(|| {
        (
            a.iter().map(|s| normal(s)).collect::<Vec<_>>(),
            b.iter().map(|s| normal(s)).collect::<Vec<_>>(),
        )
    });
    let equal = |i: usize, j: usize| keys.as_ref().map_or(a[i] == b[j], |(a, b)| a[i] == b[j]);
    let mut dp = vec![0u32; cells];
    for i in (0..a.len()).rev() {
        files::check_cancel(c)?;
        for j in (0..b.len()).rev() {
            dp[i * m + j] = if equal(i, j) {
                1 + dp[(i + 1) * m + j + 1]
            } else {
                dp[(i + 1) * m + j].max(dp[i * m + j + 1])
            };
        }
    }
    let (mut i, mut j) = (0, 0);
    let mut r = Vec::new();
    let (mut removed, mut added) = (Vec::new(), Vec::new());
    let flush = |r: &mut Vec<Line>, del: &mut Vec<usize>, add: &mut Vec<usize>| {
        let size = del.len().max(add.len());
        for k in 0..size {
            r.push(match (del.get(k), add.get(k)) {
                (Some(&i), Some(&j)) => Line::Replace(i, j),
                (Some(&i), None) => Line::Remove(i),
                (None, Some(&j)) => Line::Add(j),
                _ => unreachable!(),
            });
        }
        del.clear();
        add.clear();
    };
    while i < a.len() || j < b.len() {
        files::check_cancel(c)?;
        if i < a.len() && j < b.len() && equal(i, j) {
            flush(&mut r, &mut removed, &mut added);
            r.push(Line::Equal(i, j));
            i += 1;
            j += 1;
        } else if i < a.len() && (j == b.len() || dp[(i + 1) * m + j] >= dp[i * m + j + 1]) {
            removed.push(i);
            i += 1;
        } else {
            added.push(j);
            j += 1;
        }
    }
    flush(&mut r, &mut removed, &mut added);
    Ok(r)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn diff_pairs_replacements_and_whitespace() {
        let c = AtomicBool::new(false);
        assert_eq!(
            lines("a\nb\nc", "a\nz\nc\nd", false, &c).unwrap(),
            [
                Line::Equal(0, 0),
                Line::Replace(1, 1),
                Line::Equal(2, 2),
                Line::Add(3)
            ]
        );
        assert_eq!(
            lines("a    b", "a b", true, &c).unwrap(),
            [Line::Equal(0, 0)]
        );
    }
    #[test]
    fn local_directory_empty_types_and_hash() {
        let left = tempfile::tempdir().unwrap();
        let right = tempfile::tempdir().unwrap();
        let l = left.path();
        let r = right.path();
        std::fs::create_dir(l.join("empty")).unwrap();
        std::fs::create_dir(l.join("type")).unwrap();
        std::fs::write(l.join("name"), b"a").unwrap();
        std::fs::write(r.join("name"), b"b").unwrap();
        std::fs::write(r.join("type"), b"x").unwrap();
        let rows = directory(l, r, true, Rule::Sha256, &AtomicBool::new(false)).unwrap();
        assert_eq!(
            rows.iter().map(|r| r.state).collect::<Vec<_>>(),
            [State::LeftOnly, State::Different, State::TypeMismatch]
        );
    }
}
