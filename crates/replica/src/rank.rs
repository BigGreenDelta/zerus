//! Fractional ranks: the To do order is a key on each task, so moving one task
//! rewrites one field of one document. Digits are 0-9A-Za-z in ASCII order; a
//! rank never ends in `0`, so there is always room before and after it.
use crate::{Error, Result};

const DIGITS: &[u8; 62] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

fn digit(c: u8) -> Option<usize> {
    DIGITS.iter().position(|&d| d == c)
}

pub fn is_rank(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value.bytes().all(|c| digit(c).is_some())
        && !value.ends_with('0')
}

/// A rank strictly between `low` and `high`; `None` leaves that side open.
pub fn between(low: Option<&str>, high: Option<&str>) -> Result<String> {
    for rank in [low, high].into_iter().flatten() {
        if !is_rank(rank) {
            return Err(Error::new(format!("invalid rank {rank:?}")));
        }
    }
    if let (Some(l), Some(h)) = (low, high) {
        if l >= h {
            return Err(Error::new("rank bounds are out of order"));
        }
    }
    let low = low.unwrap_or("").as_bytes();
    let mut high = high.map(str::as_bytes);
    let mut out = Vec::new();
    let mut i = 0;
    loop {
        let l = low.get(i).map_or(0, |&c| digit(c).expect("checked"));
        let h = high.map_or(62, |h| h.get(i).map_or(62, |&c| digit(c).expect("checked")));
        if l == h {
            out.push(DIGITS[l]);
            i += 1;
            continue;
        }
        let mid = (l + h) / 2;
        if mid > l {
            out.push(DIGITS[mid]);
            break;
        }
        // Adjacent digits: keep the lower one; above it nothing bounds us.
        out.push(DIGITS[l]);
        i += 1;
        high = None;
    }
    let rank = String::from_utf8(out).expect("ASCII digits");
    if rank.len() > 64 {
        return Err(Error::new("ranks are exhausted here; reorder the list"));
    }
    Ok(rank)
}

/// `count` short ranks in increasing order, evenly spaced with room between
/// them, for renumbering a list whose ranks have grown long.
pub fn spread(count: usize) -> Vec<String> {
    let slots = count as u128 + 1;
    // The shortest width that leaves at least 62 free values in every gap.
    let mut width = 1;
    while 62u128.pow(width) < slots * 62 {
        width += 1;
    }
    let space = 62u128.pow(width);
    (1..slots)
        .map(|i| {
            let mut value = i * space / slots;
            let mut digits = vec![b'0'; width as usize];
            for slot in digits.iter_mut().rev() {
                *slot = DIGITS[(value % 62) as usize];
                value /= 62;
            }
            // Dropping trailing zeros keeps the order and the no-trailing-0 rule.
            while digits.last() == Some(&b'0') {
                digits.pop();
            }
            String::from_utf8(digits).expect("ASCII digits")
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn between_orders_strictly() {
        let first = between(None, None).unwrap();
        let after = between(Some(&first), None).unwrap();
        let before = between(None, Some(&first)).unwrap();
        let middle = between(Some(&first), Some(&after)).unwrap();
        assert!(before < first && first < middle && middle < after);
        for rank in [&first, &after, &before, &middle] {
            assert!(is_rank(rank), "{rank}");
        }
    }

    #[test]
    fn repeated_inserts_stay_ordered() {
        // Always inserting right after the same rank, and right before it.
        let mut low = between(None, None).unwrap();
        let high = between(Some(&low), None).unwrap();
        for _ in 0..40 {
            let next = between(Some(&low), Some(&high)).unwrap();
            assert!(low < next && next < high);
            low = next;
        }
        let mut high = between(None, None).unwrap();
        for _ in 0..40 {
            let next = between(None, Some(&high)).unwrap();
            assert!(next < high && is_rank(&next));
            high = next;
        }
    }

    #[test]
    fn adjacent_digits_and_bad_bounds() {
        let r = between(Some("V"), Some("W")).unwrap();
        assert!("V" < r.as_str() && r.as_str() < "W");
        assert!(between(Some("W"), Some("V")).is_err());
        assert!(between(Some("V0"), None).is_err());
        assert!(between(Some("a-b"), None).is_err());
    }

    #[test]
    fn spread_gives_short_increasing_ranks() {
        for n in [0, 1, 2, 61, 62, 1000, 100_000] {
            let ranks = spread(n);
            assert_eq!(ranks.len(), n);
            assert!(ranks.iter().all(|r| is_rank(r) && r.len() <= 5), "{n}");
            assert!(ranks.windows(2).all(|w| w[0] < w[1]), "{n}");
        }
    }
}
