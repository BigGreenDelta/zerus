use std::fmt;

/// Whether an operation was refused by a rule or failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorKind {
    /// A task rule or an invalid input: nothing changed, the caller can fix it.
    Refused,
    /// Storage, encoding or a damaged document.
    Failed,
}

/// A refusal or failure, with a message fit to show the person.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error {
    kind: ErrorKind,
    message: String,
}

impl Error {
    /// A refusal by a rule.
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            kind: ErrorKind::Refused,
            message: message.into(),
        }
    }

    pub fn failed(message: impl Into<String>) -> Self {
        Self {
            kind: ErrorKind::Failed,
            message: message.into(),
        }
    }

    pub fn kind(&self) -> ErrorKind {
        self.kind
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for Error {}

impl From<loro::LoroError> for Error {
    fn from(error: loro::LoroError) -> Self {
        Self::failed(format!("replica: {error}"))
    }
}

impl From<loro::LoroEncodeError> for Error {
    fn from(error: loro::LoroEncodeError) -> Self {
        Self::failed(format!("replica: {error}"))
    }
}

impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::failed(format!("replica storage: {error}"))
    }
}

impl From<serde_json::Error> for Error {
    fn from(error: serde_json::Error) -> Self {
        Self::failed(format!("replica: {error}"))
    }
}

pub type Result<T> = std::result::Result<T, Error>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn refusals_and_failures_are_told_apart() {
        assert_eq!(Error::new("a rule").kind(), ErrorKind::Refused);
        assert_eq!(Error::failed("damage").kind(), ErrorKind::Failed);
        let io: Error = std::io::Error::other("disk").into();
        assert_eq!(io.kind(), ErrorKind::Failed);
        assert_eq!(io.to_string(), "replica storage: disk");
    }
}
