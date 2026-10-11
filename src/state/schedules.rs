//! Turns an agent scheduled for itself. Kimi keeps them in a durable cron
//! journal on its main wire; hgs folds it read-only, as Kimi does.
use super::*;
use chrono::{Datelike, Local, NaiveDateTime, TimeZone, Timelike};

// Keep what the next run needs: expression, recurrence, creation and last fire.
pub(super) fn reduce_kimi(jobs: &mut Value, event: &Value) {
    if !jobs.is_object() {
        *jobs = json!({});
    }
    let jobs = jobs.as_object_mut().unwrap();
    match string(event, "type") {
        "cron.add" => {
            let task = &event["task"];
            if string(task, "id").is_empty() || !task["cron"].is_string()
                || !task["prompt"].is_string() || !task["createdAt"].is_number()
            {
                return;
            }
            jobs.insert(string(task, "id").to_owned(), json!({"cron":task["cron"],
                "recurring":task["recurring"] != false,"created_at":task["createdAt"]}));
        }
        "cron.delete" => {
            for id in event["ids"].as_array().into_iter().flatten().filter_map(Value::as_str) {
                jobs.remove(id);
            }
        }
        "cron.cursor" => {
            if let (Some(job), Some(at)) = (jobs.get_mut(string(event, "id")), event["lastFiredAt"].as_f64()) {
                job["last_fired_at"] = json!(at);
            }
        }
        _ => {}
    }
}

/// `{count, recurring, next_at}` for folded jobs. The next run depends only on
/// the journal, so an unchanged session keeps an unchanged snapshot.
pub(super) fn summary(jobs: &Value) -> Value {
    let jobs: Vec<&Value> = jobs.as_object().map(|jobs| jobs.values().collect()).unwrap_or_default();
    let next = jobs.iter().filter_map(|job| {
        let base = job["created_at"].as_f64()?.max(job["last_fired_at"].as_f64().unwrap_or(0.0));
        next_run(job["cron"].as_str()?, base, &Local)
    }).reduce(f64::min);
    json!({"count":jobs.len(),"recurring":jobs.iter().filter(|job| job["recurring"] == true).count(),"next_at":next})
}

/// The first minute matching a five-field cron expression strictly after
/// `after_ms`, in seconds. Kimi's jitter is not modeled.
fn next_run<Tz: TimeZone>(expression: &str, after_ms: f64, zone: &Tz) -> Option<f64> {
    let spec = Spec::parse(expression)?;
    let after = zone.timestamp_millis_opt(after_ms as i64).single()?;
    let local = after.naive_local();
    let start = local.date().and_hms_opt(local.hour(), local.minute(), 0)? + chrono::Duration::minutes(1);
    for day in 0..=5 * 366 {
        let date = start.date() + chrono::Duration::days(day);
        if !spec.months[date.month() as usize] || !spec.day(date.day(), date.weekday().num_days_from_sunday()) {
            continue;
        }
        let first = if day == 0 { (start.hour(), start.minute()) } else { (0, 0) };
        for hour in first.0..24 {
            if !spec.hours[hour as usize] {
                continue;
            }
            for minute in if hour == first.0 { first.1 } else { 0 }..60 {
                if !spec.minutes[minute as usize] {
                    continue;
                }
                // A minute skipped by a daylight-saving change does not occur.
                let candidate: NaiveDateTime = date.and_hms_opt(hour, minute, 0)?;
                if let Some(time) = zone.from_local_datetime(&candidate).earliest() {
                    if time > after {
                        return Some(time.timestamp() as f64);
                    }
                }
            }
        }
    }
    None
}

struct Spec {
    minutes: Vec<bool>,
    hours: Vec<bool>,
    days: Vec<bool>,
    months: Vec<bool>,
    weekdays: Vec<bool>,
    any_day: bool,
    any_weekday: bool,
}

impl Spec {
    fn parse(expression: &str) -> Option<Spec> {
        const MONTHS: [&str; 12] = ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"];
        const DAYS: [&str; 7] = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"];
        let fields: Vec<&str> = expression.split_whitespace().collect();
        let [minute, hour, day, month, weekday] = fields.as_slice() else {
            return None;
        };
        let mut weekdays = field(weekday, 0, 7, &DAYS, 0)?;
        weekdays[0] |= weekdays[7];
        weekdays.truncate(7);
        Some(Spec {
            minutes: field(minute, 0, 59, &[], 0)?,
            hours: field(hour, 0, 23, &[], 0)?,
            days: field(day, 1, 31, &[], 1)?,
            months: field(month, 1, 12, &MONTHS, 1)?,
            weekdays,
            any_day: day.starts_with('*'),
            any_weekday: weekday.starts_with('*'),
        })
    }

    // Like cron: when both day fields are restricted, either may match.
    fn day(&self, day: u32, weekday: u32) -> bool {
        let (day, weekday) = (self.days[day as usize], self.weekdays[weekday as usize]);
        if self.any_day || self.any_weekday { day && weekday } else { day || weekday }
    }
}

// Lists of `*`, `n`, `a-b`, each optionally `/step`; names count from `offset`.
fn field(text: &str, min: u32, max: u32, names: &[&str], offset: u32) -> Option<Vec<bool>> {
    let value = |part: &str| -> Option<u32> {
        let number = part.parse::<u32>().ok().or_else(|| {
            names.iter().position(|name| name.eq_ignore_ascii_case(part)).map(|index| index as u32 + offset)
        })?;
        (min..=max).contains(&number).then_some(number)
    };
    let mut allowed = vec![false; max as usize + 1];
    for part in text.split(',') {
        let (range, step) = match part.split_once('/') {
            Some((range, step)) => (range, step.parse::<u32>().ok().filter(|step| *step > 0)?),
            None => (part, 1),
        };
        let (first, last) = if range == "*" {
            (min, max)
        } else if let Some((first, last)) = range.split_once('-') {
            (value(first)?, value(last)?)
        } else {
            let first = value(range)?;
            (first, if part.contains('/') { max } else { first })
        };
        if first > last {
            return None;
        }
        for number in (first..=last).step_by(step as usize) {
            allowed[number as usize] = true;
        }
    }
    Some(allowed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::Utc;

    fn at(text: &str) -> f64 {
        chrono::DateTime::parse_from_rfc3339(text).unwrap().timestamp_millis() as f64
    }
    fn next(expression: &str, after: &str) -> Option<String> {
        next_run(expression, at(after), &Utc).map(|seconds| {
            chrono::DateTime::from_timestamp(seconds as i64, 0).unwrap().format("%Y-%m-%d %H:%M %a").to_string()
        })
    }

    #[test]
    fn next_runs_follow_cron_fields_after_the_last_fire() {
        assert_eq!(next("*/15 * * * *", "2026-10-11T21:45:00Z").as_deref(), Some("2026-10-11 22:00 Sun"));
        assert_eq!(next("*/15 * * * *", "2026-10-11T21:44:59.900Z").as_deref(), Some("2026-10-11 21:45 Sun"));
        assert_eq!(next("0 9 * * mon-fri", "2026-10-09T09:00:00Z").as_deref(), Some("2026-10-12 09:00 Mon"));
        assert_eq!(next("30 2 1 * *", "2026-10-11T00:00:00Z").as_deref(), Some("2026-11-01 02:30 Sun"));
        assert_eq!(next("0 0 * * 7", "2026-10-11T00:00:00Z").as_deref(), Some("2026-10-18 00:00 Sun"));
        // Both day fields restricted: either matches, as in cron.
        assert_eq!(next("0 12 13 * FRI", "2026-10-10T00:00:00Z").as_deref(), Some("2026-10-13 12:00 Tue"));
        assert_eq!(next("5,10-12/2 * * DEC *", "2026-10-11T00:00:00Z").as_deref(), Some("2026-12-01 00:05 Tue"));
        for invalid in ["", "* * * *", "61 * * * *", "*/0 * * * *", "5-1 * * * *", "0 0 31 2 *", "@hourly"] {
            assert_eq!(next(invalid, "2026-10-11T00:00:00Z"), None, "{invalid}");
        }
    }

    #[test]
    fn next_runs_use_the_local_clock_and_skip_missing_minutes() {
        let york: chrono_tz::Tz = "America/New_York".parse().unwrap();
        // 02:30 does not exist on 2027-03-14; the next 02:30 is a day later.
        let after = at("2027-03-14T05:00:00Z");
        let next = next_run("30 2 * * *", after, &york).unwrap();
        assert_eq!(chrono::DateTime::from_timestamp(next as i64, 0).unwrap().with_timezone(&york).to_rfc3339(),
            "2027-03-15T02:30:00-04:00");
    }

    #[test]
    fn summary_counts_pending_jobs_and_the_earliest_next_run() {
        let mut jobs = Value::Null;
        let add = |id: &str, cron: &str, recurring: Value, created: f64| {
            let mut task = json!({"id":id,"cron":cron,"prompt":"Check","createdAt":created});
            if !recurring.is_null() { task["recurring"] = recurring; }
            json!({"type":"cron.add","task":task})
        };
        reduce_kimi(&mut jobs, &add("watch", "*/15 * * * *", Value::Null, at("2026-10-11T19:10:00Z")));
        reduce_kimi(&mut jobs, &add("once", "0 0 1 1 *", json!(false), at("2026-10-11T19:10:00Z")));
        reduce_kimi(&mut jobs, &json!({"type":"cron.add","task":{"id":"invalid","cron":"* * * * *"}}));
        reduce_kimi(&mut jobs, &json!({"type":"cron.cursor","id":"watch","lastFiredAt":at("2026-10-11T21:45:00Z")}));
        reduce_kimi(&mut jobs, &json!({"type":"cron.cursor","id":"missing","lastFiredAt":1.0}));
        let listed = summary(&jobs);
        assert_eq!((listed["count"].clone(), listed["recurring"].clone()), (json!(2), json!(1)));
        let local = |text: &str| next_run("*/15 * * * *", at(text), &Local).unwrap();
        assert_eq!(listed["next_at"], json!(local("2026-10-11T21:45:00Z")));
        reduce_kimi(&mut jobs, &json!({"type":"cron.delete","ids":["watch","once"]}));
        assert_eq!(summary(&jobs), json!({"count":0,"recurring":0,"next_at":null}));
    }
}
