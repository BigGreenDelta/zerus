//! Base64 as Python's `base64` module reads and writes it: the standard
//! alphabet, `b64encode` with padding, and `b64decode(text, validate=True)`.
use base64::alphabet;
use base64::engine::general_purpose::{GeneralPurpose, GeneralPurposeConfig, STANDARD};
use base64::engine::DecodePaddingMode;
use base64::Engine;

/// binascii's strict mode as CPython 3.12 and later apply it: characters
/// outside the alphabet, missing, excess, leading or discontinuous padding
/// and anything after it are refused, but the unused low bits of the last
/// character are ignored, so `QR==` reads as `A`, as `QQ==` does. (3.11 also
/// reads padding after a complete group, `QUJD====`; `hgs` never writes it.)
const STRICT: GeneralPurpose = GeneralPurpose::new(
    &alphabet::STANDARD,
    GeneralPurposeConfig::new()
        .with_decode_allow_trailing_bits(true)
        .with_decode_padding_mode(DecodePaddingMode::RequireCanonical),
);

/// The plain ValueError of `_bytes_from_decode_data` for a non-ASCII
/// string, which handlers of `binascii.Error` alone do not catch.
pub const NOT_ASCII: &str = "string argument should contain only ASCII characters";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DecodeError {
    /// `binascii.Error`.
    Invalid,
    /// A string with a non-ASCII character ([`NOT_ASCII`]).
    NotAscii,
}

/// `base64.b64decode(text, validate=True)`.
pub fn decode(text: &str) -> Result<Vec<u8>, DecodeError> {
    if !text.is_ascii() {
        return Err(DecodeError::NotAscii);
    }
    STRICT.decode(text).map_err(|_| DecodeError::Invalid)
}

/// `base64.b64encode(data).decode("ascii")`.
pub fn encode(data: &[u8]) -> String {
    STANDARD.encode(data)
}

/// `base64.b64encode(base64.b64decode(text, validate=True)).decode() ==
/// text`: the bytes of a canonical encoding, or `None` for any other valid
/// one.
pub fn canonical(text: &str) -> Result<Option<Vec<u8>>, DecodeError> {
    let data = decode(text)?;
    Ok((encode(&data) == text).then_some(data))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strict_decoding_follows_binascii() {
        // Observed with CPython 3.12 and 3.14's base64.b64decode(text, validate=True).
        for (text, data) in [
            ("", &b""[..]),
            ("QQ==", b"A"),
            ("QR==", b"A"),
            ("QUI=", b"AB"),
            ("QUJ=", b"AB"),
            ("QUJD", b"ABC"),
        ] {
            assert_eq!(decode(text).as_deref(), Ok(data), "{text}");
        }
        for text in [
            "QQ", "QQ=", "QQ===", "=QQ=", "Q===", "QQ==QQ==", "Q Q==", "QUJDRA", "QUJD====",
            "====", "QQ==\n", "QUJ==", "QU-_",
        ] {
            assert_eq!(decode(text), Err(DecodeError::Invalid), "{text}");
        }
        assert_eq!(decode("\u{e9}"), Err(DecodeError::NotAscii));
        assert_eq!(decode("QQ==\u{e9}"), Err(DecodeError::NotAscii));
    }

    #[test]
    fn canonical_encodings_round_trip() {
        assert_eq!(encode(b"\0\xff"), "AP8=");
        assert_eq!(canonical("AP8="), Ok(Some(b"\0\xff".to_vec())));
        assert_eq!(canonical("AP9="), Ok(None));
        assert_eq!(canonical("AP8"), Err(DecodeError::Invalid));
    }
}
