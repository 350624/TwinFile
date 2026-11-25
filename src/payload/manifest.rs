use super::*;

#[derive(Debug)]
pub struct Partition {
    pub name: String,
    pub size: u64,
    pub(super) hash: Option<[u8; 32]>,
    pub(super) ops: Vec<Op>,
}
#[derive(Default, Debug)]
pub(super) struct Op {
    pub kind: u64,
    pub off: u64,
    pub len: u64,
    pub hash: Option<[u8; 32]>,
    pub ext: Vec<(u64, u64)>,
}
struct Fields<'a>(&'a [u8]);
enum V<'a> {
    N(u64),
    B(&'a [u8]),
    Skip,
}
fn var(b: &mut &[u8]) -> R<u64> {
    let mut n = 0;
    for i in 0..10 {
        let (&c, rest) = b.split_first().ok_or_else(|| err("元数据意外结束"))?;
        *b = rest;
        guard(i < 9 || c <= 1, "无效 varint")?;
        n |= ((c & 127) as u64) << (7 * i);
        if c & 128 == 0 {
            return Ok(n);
        }
    }
    Err(err("无效 varint"))
}
fn take<'a>(b: &mut &'a [u8], n: u64) -> R<&'a [u8]> {
    guard(n <= b.len() as u64, "元数据字段越界")?;
    let (a, rest) = b.split_at(n as usize);
    *b = rest;
    Ok(a)
}
impl<'a> Fields<'a> {
    fn next(&mut self) -> R<Option<(u64, V<'a>)>> {
        if self.0.is_empty() {
            return Ok(None);
        }
        let k = var(&mut self.0)?;
        guard(k >> 3 != 0 && k >> 3 < 1 << 29, "无效 protobuf key")?;
        let v = match k & 7 {
            0 => V::N(var(&mut self.0)?),
            1 => {
                take(&mut self.0, 8)?;
                V::Skip
            }
            2 => {
                let l = var(&mut self.0)?;
                V::B(take(&mut self.0, l)?)
            }
            5 => {
                take(&mut self.0, 4)?;
                V::Skip
            }
            _ => return Err(err("无效 protobuf wire type")),
        };
        Ok(Some((k >> 3, v)))
    }
}
fn digest(b: &[u8]) -> R<Option<[u8; 32]>> {
    if b.is_empty() {
        return Ok(None);
    }
    Ok(Some(b.try_into().map_err(|_| err("SHA256 长度无效"))?))
}
pub(super) fn parse(b: &[u8], data_len: u64, cancel: &AtomicBool) -> R<(u64, Vec<Partition>)> {
    let mut f = Fields(b);
    let mut block = 4096;
    let mut parts = vec![];
    let mut names = HashSet::new();
    let (mut ops, mut exts) = (0, 0);
    while let Some(field) = f.next()? {
        check(cancel)?;
        match field {
            (3, V::N(v)) => block = v,
            (13, V::B(v)) => {
                guard(parts.len() < 1024, "分区过多")?;
                let mut p = Partition {
                    name: String::new(),
                    size: 0,
                    ops: vec![],
                    hash: None,
                };
                let mut ff = Fields(v);
                while let Some(field) = ff.next()? {
                    check(cancel)?;
                    match field {
                        (1, V::B(v)) => {
                            guard(v.len() <= 200, "分区名称过长")?;
                            p.name = std::str::from_utf8(v).map_err(err)?.to_owned()
                        }
                        (7, V::B(v)) => {
                            let mut info = Fields(v);
                            while let Some(k) = info.next()? {
                                match k {
                                    (1, V::N(v)) => p.size = v,
                                    (2, V::B(v)) => p.hash = digest(v)?,
                                    _ => {}
                                }
                            }
                        }
                        (8, V::B(v)) => {
                            ops += 1;
                            guard(ops <= 1_000_000, "操作数量超过限制")?;
                            let mut op = Op {
                                kind: u64::MAX,
                                ..Default::default()
                            };
                            let mut fields = Fields(v);
                            while let Some(field) = fields.next()? {
                                match field {
                                    (1, V::N(v)) => op.kind = v,
                                    (2, V::N(v)) => op.off = v,
                                    (3, V::N(v)) => op.len = v,
                                    (8, V::B(v)) => op.hash = digest(v)?,
                                    (6, V::B(v)) => {
                                        exts += 1;
                                        guard(exts <= 2_000_000, "extent 过多")?;
                                        let mut ef = Fields(v);
                                        let (mut start, mut count) = (0, 0);
                                        while let Some(k) = ef.next()? {
                                            match k {
                                                (1, V::N(v)) => start = v,
                                                (2, V::N(v)) => count = v,
                                                _ => {}
                                            }
                                        }
                                        op.ext.push((start, count));
                                    }
                                    _ => {}
                                }
                            }
                            p.ops.push(op)
                        }
                        _ => {}
                    }
                }
                crate::files::safe_name(&p.name)?;
                guard(names.insert(p.name.to_lowercase()), "分区名称重复")?;
                guard(
                    p.size > 0 && p.size <= 256 << 30 && !p.ops.is_empty(),
                    "分区大小或操作列表无效",
                )?;
                parts.push(p);
            }
            _ => {}
        }
    }
    guard(
        block.is_power_of_two() && block <= 1 << 20 && !parts.is_empty(),
        "block_size 或分区列表无效",
    )?;
    for p in &parts {
        let mut spans = vec![];
        for op in &p.ops {
            check(cancel)?;
            if !matches!(op.kind, 0 | 1 | 6 | 7 | 8 | 14) {
                return Err(err(format!(
                    "分区 {} 使用不支持的操作 {}（不支持依赖旧分区的增量 OTA）",
                    p.name, op.kind
                )));
            }
            guard(
                op.off <= data_len && op.len <= data_len - op.off && !op.ext.is_empty(),
                "操作数据越界或缺少 extent",
            )?;
            let mut sum = 0;
            for &(a, n) in &op.ext {
                guard(
                    n > 0 && a <= p.size / block && n <= p.size / block - a,
                    "extent 越界",
                )?;
                guard(n * block <= p.size - sum, "extent 长度无效")?;
                sum += n * block;
                spans.push((a, a + n));
            }
            guard(
                match op.kind {
                    0 => op.len == sum,
                    6 | 7 => op.len == 0,
                    _ => op.len > 0,
                },
                "操作长度与 extent 不匹配",
            )?;
        }
        spans.sort_unstable();
        guard(
            spans.windows(2).all(|w| w[0].1 <= w[1].0),
            "分区包含重叠的写入区域",
        )?;
    }
    Ok((block, parts))
}
