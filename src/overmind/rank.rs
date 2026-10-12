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

// Ranks made by `after` are keys: a head digit from `V` to `y` gives the width
// (1 to 30) of the base-62 number that follows, so wider numbers sort later
// and a column grows by one character per 62-fold more appends.
const FIRST_HEAD: usize = 31;
const LAST_HEAD: usize = 60;

/// A short rank strictly after `a`: the next key above `a`. No rank made here,
/// by `between` or by `spread` ends in `0`, so `between` always has room.
pub fn after(a: &str) -> String {
    let bytes = a.as_bytes();
    let head = bytes.first().map_or(0, |c| digit(*c));
    if head < FIRST_HEAD {
        return "V".into();
    }
    if head > LAST_HEAD {
        // Below a long run of `z`, a whole key may not fit; `between` adds one character.
        let next = format!("z{}", after(&a[1..]));
        return if next.len() <= 64 { next } else { between(a, None) };
    }
    let top = DIGITS.len() - 1;
    let width = head - FIRST_HEAD + 1;
    // The number written after the head, padded or cut to its width, plus one.
    let mut number: Vec<usize> = (1..=width).map(|i| bytes.get(i).map_or(0, |c| digit(*c))).collect();
    let mut place = width;
    loop {
        if place == 0 {
            if head == LAST_HEAD {
                return "z".into();
            }
            // Every digit was the largest: the next width starts.
            let mut out = vec![DIGITS[head + 1]];
            out.resize(width + 1, b'0');
            out.push(b'1');
            return String::from_utf8(out).expect("ranks are ASCII");
        }
        place -= 1;
        if number[place] < top {
            number[place] += 1;
            break;
        }
        number[place] = 0;
    }
    if number[width - 1] == 0 {
        number[width - 1] = 1;
    }
    let mut out = vec![DIGITS[head]];
    out.extend(number.iter().map(|d| DIGITS[*d]));
    String::from_utf8(out).expect("ranks are ASCII")
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
    fn after_counts_up_in_keys_of_growing_width() {
        assert_eq!(after(""), "V");
        assert_eq!(after("F"), "V");
        assert_eq!(after("V"), "V1");
        assert_eq!(after("Vk"), "Vl");
        assert_eq!(after("Vk7"), "Vl");
        assert_eq!(after("Vz"), "W01");
        assert_eq!(after("W0z"), "W11");
        assert_eq!(after("zA3"), "zV");
        assert_eq!(after("z"), "zV");
        assert_eq!(after(&format!("y{}", "z".repeat(30))), "z");
        for a in [format!("{}W", "z".repeat(62)), format!("{}y", "z".repeat(34)), "z".repeat(63)] {
            let next = after(&a);
            assert!(valid(&next) && a < next, "{a} -> {next}");
        }
    }

    #[test]
    fn successive_afters_stay_ordered_and_short() {
        let mut previous = String::new();
        for _ in 0..200_000 {
            let next = after(&previous);
            assert!(previous.as_bytes() < next.as_bytes(), "{previous} < {next}");
            assert!(valid(&next) && !next.ends_with('0'), "{next}");
            previous = next;
        }
        assert!(previous.len() <= 4, "{previous}");
    }

    #[test]
    fn after_any_rank_sorts_later_and_leaves_room_before_it() {
        let mut seed = 11u64;
        for _ in 0..20_000 {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            // Up to 63 characters, often below a run of `z` such as old ranks left.
            let len = 1 + (seed % 63) as usize;
            let run = if seed % 3 == 0 { (seed >> 8) as usize % len } else { 0 };
            let a: String = (0..len)
                .map(|i| if i < run { 'z' } else { DIGITS[((seed >> (i % 8 * 8)) as usize + i * 7) % DIGITS.len()] as char })
                .collect();
            let next = after(&a);
            assert!(valid(&next) && !next.ends_with('0'), "{a} -> {next}");
            assert!(a.as_bytes() < next.as_bytes(), "{a} < {next}");
            let middle = between(&a, Some(&next));
            assert!(a.as_bytes() < middle.as_bytes() && middle.as_bytes() < next.as_bytes(), "{a} < {middle} < {next}");
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
