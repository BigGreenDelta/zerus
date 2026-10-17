//! Shared native/relay video admission. No codec runs on either service.
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
type Result<T> = std::result::Result<T, String>;

pub(crate) const MAX_BYTES: usize = 10 * 1024 * 1024;
pub(crate) const MAX_REQUEST_BYTES: usize = 19 * 1024 * 1024;
pub(crate) const MAX_DURATION_MS: u64 = 60_000;

pub(crate) struct Video {
    pub bytes: Vec<u8>,
    pub metadata: Value,
}

/// Check the bounded container without invoking codecs on the conversation host.
fn mp4(bytes: &[u8]) -> bool {
    let mut offset = 0usize;
    let mut boxes = 0;
    let (mut ftyp, mut moov, mut mdat) = (false, false, false);
    while offset < bytes.len() {
        boxes += 1;
        if boxes > 4096 || bytes.len() - offset < 8 {
            return false;
        }
        let declared = u32::from_be_bytes(bytes[offset..offset + 4].try_into().unwrap());
        let (size, header) = if declared == 1 {
            if bytes.len() - offset < 16 {
                return false;
            }
            let size = u64::from_be_bytes(bytes[offset + 8..offset + 16].try_into().unwrap());
            let Ok(size) = usize::try_from(size) else {
                return false;
            };
            (size, 16)
        } else if declared == 0 {
            (bytes.len() - offset, 8)
        } else {
            (declared as usize, 8)
        };
        if size < header || size > bytes.len() - offset {
            return false;
        }
        match &bytes[offset + 4..offset + 8] {
            b"ftyp" => {
                if offset != 0 || !(16..=4096).contains(&size) {
                    return false;
                }
                ftyp = true;
            }
            b"moov" => moov = true,
            b"mdat" => mdat |= size > header,
            _ => (),
        }
        offset += size;
    }
    ftyp && moov && mdat
}

pub(crate) fn parse(value: &Value, id: &str, pcm_bytes: usize) -> Result<Video> {
    let object = value.as_object().ok_or("Invalid video recording")?;
    let required = ["format", "data_base64", "duration_ms", "width", "height"];
    if required.iter().any(|field| !object.contains_key(*field))
        || object
            .keys()
            .any(|field| !required.contains(&field.as_str()) && field != "poster_base64")
        || value["format"] != "mp4"
    {
        return Err("Invalid video recording fields".into());
    }
    let duration = value["duration_ms"]
        .as_u64()
        .filter(|n| (100..=MAX_DURATION_MS).contains(n))
        .ok_or("Video recording exceeds 60 seconds")?;
    if !(3_200..=1_920_000).contains(&pcm_bytes)
        || duration.abs_diff((pcm_bytes * 1000 / 32000) as u64) > 1000
    {
        return Err("Video and speech audio durations do not match".into());
    }
    for field in ["width", "height"] {
        if value[field]
            .as_u64()
            .is_none_or(|n| !(16..=1920).contains(&n))
        {
            return Err("Invalid video recording dimensions".into());
        }
    }
    let encoded = value["data_base64"]
        .as_str()
        .filter(|s| s.len() <= MAX_BYTES.div_ceil(3) * 4)
        .ok_or("Video recording exceeds 10 MiB")?;
    let bytes = STANDARD
        .decode(encoded)
        .map_err(|_| "Invalid video recording encoding")?;
    if bytes.len() > MAX_BYTES || !mp4(&bytes) {
        return Err("Invalid or oversized MP4 recording".into());
    }
    let mut metadata = json!({"request_id":id,"index":1,"name":"video.mp4","mime":"video/mp4",
        "bytes":bytes.len(),"duration_ms":duration,"width":value["width"],"height":value["height"]});
    if let Some(poster) = value.get("poster_base64") {
        let encoded = poster
            .as_str()
            .filter(|s| s.len() <= 10_924)
            .ok_or("Video poster is too large")?;
        let jpeg = STANDARD
            .decode(encoded)
            .map_err(|_| "Invalid video poster")?;
        if jpeg.len() > 8192 || !jpeg.starts_with(&[0xff, 0xd8]) || !jpeg.ends_with(&[0xff, 0xd9]) {
            return Err("Invalid video poster".into());
        }
        metadata["poster_base64"] = json!(encoded);
    }
    Ok(Video { bytes, metadata })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    pub(crate) fn request() -> Value {
        let mut bytes = Vec::new();
        for (kind, body) in [
            (b"ftyp", b"isom0000".as_slice()),
            (b"moov", b"".as_slice()),
            (b"mdat", b"data".as_slice()),
        ] {
            bytes.extend_from_slice(&((body.len() + 8) as u32).to_be_bytes());
            bytes.extend_from_slice(kind);
            bytes.extend_from_slice(body);
        }
        json!({"format":"mp4","data_base64":STANDARD.encode(bytes),"duration_ms":1000,"width":480,"height":480})
    }
    #[test]
    fn only_bounded_complete_containers_and_matching_audio_are_accepted() {
        let valid = request();
        let accepted = parse(&valid, "message", 32_000).unwrap();
        assert_eq!(accepted.metadata["index"], 1);
        assert_eq!(accepted.metadata["mime"], "video/mp4");
        assert!(parse(&valid, "message", 96_000).is_err());
        for (field, value) in [
            ("duration_ms", json!(60_001)),
            ("width", json!(0)),
            ("height", json!(2_000)),
            ("data_base64", json!(STANDARD.encode(b"not an mp4"))),
            ("poster_base64", json!("not jpeg")),
            ("path", json!("/workspace/video.mp4")),
        ] {
            let mut invalid = valid.clone();
            invalid[field] = value;
            assert!(parse(&invalid, "message", 32_000).is_err(), "{field}");
        }
        let mut incomplete = accepted.bytes;
        incomplete.pop();
        assert!(!mp4(&incomplete));
    }
}
