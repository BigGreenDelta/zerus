//! Fractional ranks: strings that sort between any two others, so reordering
//! never renumbers neighbours. Valid ranks use DIGITS and are compared as bytes.
pub const DIGITS: &[u8] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

fn digit(c: u8) -> usize {
    DIGITS.iter().position(|d| *d == c).unwrap_or(0)
}

pub fn valid(rank: &str) -> bool {
    !rank.is_empty() && rank.len() <= 64 && rank.bytes().all(|c| DIGITS.contains(&c))
}

/// A rank strictly between `a` and `b`; `""` sorts before everything and
/// `None` after everything. Callers ensure `a < b`.
pub fn between(a: &str, b: Option<&str>) -> String {
    let (a, b) = (a.as_bytes(), b.map(str::as_bytes));
    let base = DIGITS.len();
    let mut out = Vec::new();
    let mut i = 0;
    loop {
        let da = a.get(i).map_or(0, |c| digit(*c));
        let db = b.and_then(|b| b.get(i)).map_or(base, |c| digit(*c));
        if da == db {
            out.push(DIGITS[da]);
            i += 1;
            continue;
        }
        if db - da > 1 {
            out.push(DIGITS[(da + db) / 2]);
            break;
        }
        out.push(DIGITS[da]);
        i += 1;
        loop {
            let da = a.get(i).map_or(0, |c| digit(*c));
            if base - da > 1 {
                out.push(DIGITS[(da + base) / 2]);
                return String::from_utf8(out).expect("ranks are ASCII");
            }
            out.push(DIGITS[da]);
            i += 1;
        }
    }
    String::from_utf8(out).expect("ranks are ASCII")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_rank_is_the_middle() {
        assert_eq!(between("", None), "V");
    }

    #[test]
    fn results_sit_strictly_between_bounds() {
        for (a, b) in [("", Some("1")), ("U", Some("V")), ("z", None), ("", Some("0V")), ("V", None)] {
            let r = between(a, b);
            assert!(valid(&r), "{r}");
            assert!(a.as_bytes() < r.as_bytes(), "{a} < {r}");
            if let Some(b) = b {
                assert!(r.as_bytes() < b.as_bytes(), "{r} < {b}");
            }
        }
    }

    #[test]
    fn repeated_insertions_stay_ordered() {
        let lower = between("", None);
        let mut upper = between(&lower, None);
        for _ in 0..200 {
            let r = between(&lower, Some(&upper));
            assert!(valid(&r), "{r}");
            assert!(lower.as_bytes() < r.as_bytes() && r.as_bytes() < upper.as_bytes());
            upper = r;
        }
    }
}
