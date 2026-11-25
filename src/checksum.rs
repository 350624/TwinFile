use std::{
    fs::File,
    io::{self, Read, Write},
    path::Path,
};

const LIMIT: usize = 2 * 1024 * 1024;
pub const TAGS: [&str; 3] = ["MD5", "SHA1", "SHA256"];

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Format {
    Gnu,
    Bsd,
}
pub fn import(path: &Path) -> io::Result<String> {
    let mut content = Vec::new();
    File::open(path)?
        .take((LIMIT + 1) as u64)
        .read_to_end(&mut content)?;
    if content.len() > LIMIT {
        return Err(io::Error::other("校验清单不能超过 2 MiB"));
    }
    let text = String::from_utf8(content).map_err(io::Error::other)?;
    Ok(text.trim_start_matches('\u{feff}').to_owned())
}
fn algorithm(value: &str) -> Option<usize> {
    if !value.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    [32, 40, 64].iter().position(|&n| n == value.len())
}
fn matches_name(listed: &str, name: &str) -> bool {
    let entry = listed.rsplit(['/', '\\']).next().unwrap_or("");
    if cfg!(windows) {
        entry.to_lowercase() == name.to_lowercase()
    } else {
        entry == name
    }
}
pub fn expected(text: &str, name: &str) -> io::Result<[Option<String>; 3]> {
    if text.len() > LIMIT {
        return Err(io::Error::other("校验清单不能超过 2 MiB"));
    }
    let mut result = [None, None, None];
    for line in text
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
    {
        let (v, declared) = if let Some((tag, tail)) = line.split_once(" (")
            && let Some((target, digest)) = tail.rsplit_once(") = ")
            && let Some(index) = TAGS.iter().position(|t| t.eq_ignore_ascii_case(tag))
        {
            if !matches_name(target, name) {
                continue;
            }
            (digest, Some(index))
        } else if let Some((digest, tail)) = line.split_once([' ', '\t']) {
            let target = tail
                .trim_start()
                .strip_prefix('*')
                .unwrap_or(tail.trim_start());
            if !matches_name(target, name) {
                continue;
            }
            (digest, None)
        } else {
            (line, None)
        };
        let Some(index) = algorithm(v).filter(|i| declared.is_none_or(|d| d == *i)) else {
            return Err(io::Error::other("校验值无效，或算法与摘要长度不符"));
        };
        let lower = v.to_ascii_lowercase();
        if result[index].as_ref().is_some_and(|prior| prior != &lower) {
            return Err(io::Error::other("同一算法存在相互冲突的校验值"));
        }
        result[index] = Some(lower);
    }
    if result.iter().all(Option::is_none) {
        return Err(io::Error::other("清单中未找到当前文件"));
    }
    Ok(result)
}
pub fn export(path: &Path, name: &str, values: &[String; 3], format: Format) -> io::Result<()> {
    if name.is_empty() || name.contains(['\r', '\n', '\\', '/']) {
        return Err(io::Error::other("文件名不适合写入校验清单"));
    }
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let mut tmp = tempfile::NamedTempFile::new_in(parent)?;
    for (tag, value) in TAGS.iter().zip(values) {
        match format {
            Format::Gnu => writeln!(tmp, "{value} *{name}")?,
            Format::Bsd => writeln!(tmp, "{tag} ({name}) = {value}")?,
        };
    }
    tmp.as_file().sync_all()?;
    tmp.persist_noclobber(path).map_err(|e| e.error)?;
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn checksum_manifests_round_trip_and_ignore_other_files() {
        let d = tempfile::tempdir().unwrap();
        let values = ["a".repeat(32), "b".repeat(40), "c".repeat(64)];
        for format in [Format::Gnu, Format::Bsd] {
            let dest = d
                .path()
                .join(if format == Format::Gnu { "gnu" } else { "bsd" });
            export(&dest, "带 空格.txt", &values, format).unwrap();
            let text = import(&dest).unwrap();
            assert_eq!(
                expected(&text, "带 空格.txt").unwrap(),
                values.clone().map(Some)
            );
            assert!(expected(&text, "other.txt").is_err());
            assert!(export(&dest, "带 空格.txt", &values, format).is_err());
        }
        assert!(expected(&format!("{}\n{}", values[0], "f".repeat(32)), "n").is_err());
        assert!(expected(&format!("SHA1 (n) = {}", values[0]), "n").is_err());
        assert_eq!(
            expected(&format!("{}  ./n\n{} *misc", values[1], values[2]), "n").unwrap(),
            [None, Some(values[1].clone()), None]
        );
    }
}
