//! Test seams for the black-box suites, built only with the non-default
//! `test-hooks` feature. Packages never enable it, and release checks look
//! for the variable prefix to prove its absence.
pub const PREFIX: &str = "ZERUS_CONNECTOR_TEST_";

/// A positive number from `ZERUS_CONNECTOR_TEST_<name>`, such as a native
/// timeout in seconds or a clock scale.
pub fn number(name: &str) -> Option<f64> {
    let value: f64 = std::env::var(format!("{PREFIX}{name}"))
        .ok()?
        .parse()
        .ok()?;
    (value.is_finite() && value > 0.0).then_some(value)
}
