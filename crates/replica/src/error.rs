use std::fmt;

/// A refusal or failure, with a message fit to show the person.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error(String);

impl Error {
    pub fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

impl From<loro::LoroError> for Error {
    fn from(error: loro::LoroError) -> Self {
        Self(format!("replica: {error}"))
    }
}

impl From<loro::LoroEncodeError> for Error {
    fn from(error: loro::LoroEncodeError) -> Self {
        Self(format!("replica: {error}"))
    }
}

impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self(format!("replica storage: {error}"))
    }
}

impl From<serde_json::Error> for Error {
    fn from(error: serde_json::Error) -> Self {
        Self(format!("replica: {error}"))
    }
}

pub type Result<T> = std::result::Result<T, Error>;
