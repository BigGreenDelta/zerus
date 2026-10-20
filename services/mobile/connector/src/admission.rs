//! A single child admission allowance. Contention retries never respawn a child;
//! genuine path/ownership/I/O failures propagate at once.
use std::time::Duration;

pub(crate) const ALLOWANCE: Duration = Duration::from_secs(5);
const INTERVAL: Duration = Duration::from_millis(50);

/// `None` from attempt means only transient lock contention. Check the remaining
/// allowance both before and after opening: a late guard must not admit writes.
pub(crate) fn acquire<T, E>(
    mut attempt: impl FnMut() -> Result<Option<T>, E>,
    mut remaining: impl FnMut() -> Duration,
    mut wait: impl FnMut(Duration),
) -> Result<Option<T>, E> {
    loop {
        if remaining().is_zero() {
            return Ok(None);
        }
        let guard = attempt()?;
        let left = remaining();
        if left.is_zero() {
            return Ok(None);
        }
        if guard.is_some() {
            return Ok(guard);
        }
        wait(left.min(INTERVAL));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    #[test]
    fn held_then_released_admission_succeeds_without_relaunch() {
        let elapsed = Cell::new(Duration::ZERO);
        let released = Cell::new(false);
        let attempts = Cell::new(0);
        let result = acquire(
            || {
                attempts.set(attempts.get() + 1);
                Ok::<_, &str>(released.get().then_some("same child"))
            },
            || ALLOWANCE.saturating_sub(elapsed.get()),
            |delay| {
                assert_eq!(delay, INTERVAL);
                elapsed.set(elapsed.get() + delay);
                released.set(true);
            },
        );
        assert_eq!(result, Ok(Some("same child")));
        assert_eq!(attempts.get(), 2);
        assert_eq!(elapsed.get(), INTERVAL);
    }

    #[test]
    fn persistent_contention_stops_at_one_deadline() {
        let elapsed = Cell::new(Duration::ZERO);
        let attempts = Cell::new(0);
        let result = acquire(
            || {
                attempts.set(attempts.get() + 1);
                Ok::<Option<()>, &str>(None)
            },
            || ALLOWANCE.saturating_sub(elapsed.get()),
            |delay| elapsed.set(elapsed.get() + delay),
        );
        assert_eq!(result, Ok(None));
        assert_eq!(elapsed.get(), ALLOWANCE);
        assert_eq!(attempts.get(), 100);
    }

    #[test]
    fn genuine_refusal_is_not_retried() {
        let attempts = Cell::new(0);
        let result = acquire(
            || {
                attempts.set(attempts.get() + 1);
                Err::<Option<()>, _>("unsafe path")
            },
            || ALLOWANCE,
            |_| panic!("refusal must not wait"),
        );
        assert_eq!(result, Err("unsafe path"));
        assert_eq!(attempts.get(), 1);
    }

    #[test]
    fn a_guard_obtained_after_expiry_is_dropped_before_admission() {
        struct Guard<'a>(&'a Cell<bool>);
        impl Drop for Guard<'_> {
            fn drop(&mut self) {
                self.0.set(true);
            }
        }
        let elapsed = Cell::new(Duration::ZERO);
        let dropped = Cell::new(false);
        let result = acquire(
            || {
                elapsed.set(ALLOWANCE);
                Ok::<_, &str>(Some(Guard(&dropped)))
            },
            || ALLOWANCE.saturating_sub(elapsed.get()),
            |_| panic!("late acquisition must not wait"),
        );
        assert!(matches!(result, Ok(None)));
        assert!(dropped.get());
    }

    #[test]
    fn final_wait_is_capped_to_the_remaining_allowance() {
        let elapsed = Cell::new(Duration::ZERO);
        let budget = Duration::from_millis(1);
        let result = acquire(
            || Ok::<Option<()>, &str>(None),
            || budget.saturating_sub(elapsed.get()),
            |delay| {
                assert_eq!(delay, budget);
                elapsed.set(elapsed.get() + delay);
            },
        );
        assert_eq!(result, Ok(None));
        assert_eq!(elapsed.get(), budget);
    }
}
