//! Fractional ranks: strings that sort between two others, so reordering
//! rarely renumbers neighbours. Valid ranks use DIGITS, are at most 64
//! characters long and are compared as bytes.
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

/// A short rank strictly after `a`: the first digit below the largest one is
/// incremented and the rest dropped; a rank of only largest digits grows by
/// one. Successive results grow by one character every 31 steps, so a column
/// appended to 1,984 times from the empty rank still has valid ranks.
pub fn after(a: &str) -> String {
    if a.is_empty() {
        return "V".into();
    }
    let top = DIGITS.len() - 1;
    let bytes = a.as_bytes();
    match bytes.iter().position(|c| digit(*c) < top) {
        Some(i) => {
            let mut out = bytes[..i].to_vec();
            out.push(DIGITS[digit(bytes[i]) + 1]);
            String::from_utf8(out).expect("ranks are ASCII")
        }
        None => format!("{a}V"),
    }
}

/// `n` evenly spaced valid ranks in increasing order, for renumbering a column.
pub fn spread(n: usize) -> Vec<String> {
    let base = DIGITS.len() as u128;
    let slots = n as u128 + 1;
    let (mut width, mut span) = (1, base);
    while span <= slots {
        width += 1;
        span *= base;
    }
    (1..slots)
        .map(|i| {
            let mut value = i * span / slots;
            let mut out = vec![b'0'; width];
            for place in out.iter_mut().rev() {
                *place = DIGITS[(value % base) as usize];
                value /= base;
            }
            while out.last() == Some(&b'0') {
                out.pop();
            }
            String::from_utf8(out).expect("ranks are ASCII")
        })
        .collect()
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
    fn after_increments_the_first_digit_below_the_largest() {
        assert_eq!(after(""), "V");
        assert_eq!(after("V"), "W");
        assert_eq!(after("Vk"), "W");
        assert_eq!(after("zV"), "zW");
        assert_eq!(after("zz"), "zzV");
    }

    #[test]
    fn successive_afters_stay_ordered_and_grow_slowly() {
        let mut previous = String::new();
        for step in 1..=5000 {
            let next = after(&previous);
            assert!(previous.as_bytes() < next.as_bytes(), "{previous} < {next}");
            // One character per 31 steps: the first 1,984 ranks fit in 64 characters.
            assert_eq!(valid(&next), step <= 64 * 31, "step {step}: {next}");
            previous = next;
        }
    }

    #[test]
    fn spread_ranks_are_valid_increasing_and_short() {
        for n in [0, 1, 2, 61, 62, 63, 1000, 5000] {
            let ranks = spread(n);
            assert_eq!(ranks.len(), n);
            assert!(ranks.iter().all(|r| valid(r) && !r.ends_with('0')), "{n}");
            assert!(ranks.windows(2).all(|w| w[0].as_bytes() < w[1].as_bytes()), "{n}");
        }
        assert_eq!(spread(1), vec!["V"]);
        assert!(spread(5000).iter().all(|r| r.len() <= 3));
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
