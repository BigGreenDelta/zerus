//! Short task references: `om-` plus the shortest unambiguous prefix, at least
//! five characters, of a base32 digest of the task UUID. A reference can grow
//! as tasks arrive; an older, shorter one then resolves as ambiguous.
use super::model::Refusal;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

const ALPHABET: &[u8; 32] = b"abcdefghijklmnopqrstuvwxyz234567";
pub const MIN: usize = 5;

pub fn digest(uuid: &str) -> String {
    let mut out = String::new();
    let mut buffer: u32 = 0;
    let mut bits = 0;
    for byte in Sha256::digest(uuid.as_bytes()) {
        buffer = (buffer << 8) | byte as u32;
        bits += 8;
        while bits >= 5 {
            bits -= 5;
            out.push(ALPHABET[((buffer >> bits) & 31) as usize] as char);
        }
    }
    if bits > 0 {
        out.push(ALPHABET[((buffer << (5 - bits)) & 31) as usize] as char);
    }
    out
}

fn common(a: &str, b: &str) -> usize {
    a.bytes().zip(b.bytes()).take_while(|(x, y)| x == y).count()
}

/// Prefix lengths that keep each digest distinct from all others.
fn shortest(digests: &[String]) -> Vec<usize> {
    let mut sorted: Vec<&String> = digests.iter().collect();
    sorted.sort();
    digests
        .iter()
        .map(|d| {
            let i = sorted.binary_search(&d).expect("digest is present");
            let mut need = MIN;
            if i > 0 {
                need = need.max(common(d, sorted[i - 1]) + 1);
            }
            if i + 1 < sorted.len() {
                need = need.max(common(d, sorted[i + 1]) + 1);
            }
            need.min(d.len())
        })
        .collect()
}

pub fn refs(uuids: &[String]) -> BTreeMap<String, String> {
    let digests: Vec<String> = uuids.iter().map(|u| digest(u)).collect();
    shortest(&digests)
        .into_iter()
        .zip(uuids.iter().zip(digests.iter()))
        .map(|(n, (uuid, d))| (uuid.clone(), format!("om-{}", &d[..n])))
        .collect()
}

pub fn resolve(input: &str, uuids: &[String]) -> Result<String, Refusal> {
    let input = input.trim();
    if let Some(found) = uuids.iter().find(|u| u.as_str() == input) {
        return Ok(found.clone());
    }
    let unknown = || Refusal::new("unknown_task", format!("no task {input}"));
    let prefix = input.strip_prefix("om-").filter(|p| !p.is_empty()).ok_or_else(unknown)?;
    let matches: Vec<&String> = uuids.iter().filter(|u| digest(u).starts_with(prefix)).collect();
    match matches.len() {
        0 => Err(unknown()),
        1 => Ok(matches[0].clone()),
        _ => {
            let all = refs(uuids);
            let names: Vec<&str> = matches.iter().take(10).map(|u| all[*u].as_str()).collect();
            Err(Refusal::new("ambiguous", format!("{input} matches several tasks: {}", names.join(", "))))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ids(n: usize) -> Vec<String> {
        (0..n).map(|i| format!("00000000-0000-4000-8000-{i:012}")).collect()
    }

    #[test]
    fn digests_are_stable_base32() {
        let d = digest("x");
        assert_eq!(d.len(), 52);
        assert!(d.bytes().all(|b| ALPHABET.contains(&b)));
        assert_eq!(d, digest("x"));
    }

    #[test]
    fn refs_are_unique_and_at_least_five_characters() {
        let all = ids(2000);
        let map = refs(&all);
        let values: std::collections::BTreeSet<_> = map.values().collect();
        assert_eq!(values.len(), all.len());
        assert!(map.values().all(|r| r.starts_with("om-") && r.len() >= 3 + MIN));
    }

    #[test]
    fn prefixes_grow_only_where_needed() {
        let lengths = shortest(&["aaaaab".into(), "aaaaac".into(), "zzzzzz".into()]);
        assert_eq!(lengths, vec![6, 6, 5]);
    }

    #[test]
    fn resolve_accepts_uuids_and_unambiguous_prefixes() {
        let all = ids(100);
        let map = refs(&all);
        assert_eq!(resolve(&all[3], &all).unwrap(), all[3]);
        assert_eq!(resolve(&map[&all[3]], &all).unwrap(), all[3]);
        assert_eq!(resolve(&format!(" {} ", map[&all[3]]), &all).unwrap(), all[3]);
        let first = &digest(&all[0])[..1];
        let error = resolve(&format!("om-{first}"), &all).unwrap_err();
        assert_eq!(error.code, "ambiguous");
        assert_eq!(resolve("om-zzzzzzzz9", &all).unwrap_err().code, "unknown_task");
        assert_eq!(resolve("nonsense", &all).unwrap_err().code, "unknown_task");
    }
}
