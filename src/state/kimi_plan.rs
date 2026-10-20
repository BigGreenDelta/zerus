//! Kimi's plan review in Activity. ExitPlanMode asks for approval through the
//! main agent's wire with the complete plan; the TUI prints the plan above a
//! short chooser. The card shows that plan and the chooser's own choices.
//! Delivery uses one verified choice key on the exact chooser, plus a pasted
//! Revise feedback line, and only a matching interaction.resolved event
//! acknowledges it.
use super::question_terminal::{self, Native, Transport};
use super::*;
use std::collections::BTreeMap;
use std::time::{Duration, Instant};

pub(super) const SOURCE: &str = "kimi_plan_review";
const TITLE: &str = "Ready to build with this plan?";
const REVISE: &str = "Revise";
const MAX_PLAN_CHARS: usize = 8000;

/// Pending plan reviews of the main agent, each bound to its active
/// ExitPlanMode call and removed once Kimi records a resolution.
pub(super) fn recover(record: &Value, events: &[Value]) -> Vec<Value> {
    if string(record, "agent") != "kimi" {
        return Vec::new();
    }
    let mut pending = BTreeMap::new();
    for event in events {
        let id = string(event, "id");
        match string(event, "type") {
            "interaction.request" if string(event, "kind") == "approval" => {
                if let Some(card) = card(record, event) {
                    pending.insert(id.to_owned(), card);
                }
            }
            "interaction.resolved" | "interaction.cancelled" | "interaction.dismissed" => {
                pending.remove(id);
            }
            _ => {}
        }
    }
    pending.into_values().collect()
}

fn card(record: &Value, event: &Value) -> Option<Value> {
    let id = string(event, "id");
    let tool = string(event, "toolCallId");
    let request = &event["request"];
    let display = &request["display"];
    if id.is_empty()
        || id.len() > 240
        || tool.is_empty()
        || tool.len() > 200
        || string(request, "toolName") != "ExitPlanMode"
        || string(display, "kind") != "plan_review"
        || !record["active_tools"]
            .get(tool)
            .is_some_and(|active| string(active, "name") == "ExitPlanMode")
    {
        return None;
    }
    let plan: String = string(display, "plan")
        .replace("\r\n", "\n")
        .chars()
        .filter(|ch| !ch.is_control() || *ch == '\n' || *ch == '\t')
        .collect();
    if plan.trim().is_empty() {
        return None;
    }
    let path = string(display, "path");
    let body = if plan.chars().count() > MAX_PLAN_CHARS {
        let shown: String = plan.chars().take(MAX_PLAN_CHARS).collect();
        let rest = if path.is_empty() {
            "The rest of the plan is in Terminal.".to_owned()
        } else {
            format!("The complete plan is saved in `{path}`.")
        };
        format!("{}\n\n…\n\n{rest}", shown.trim_end())
    } else {
        plan.trim().to_owned()
    };
    // Kimi lists two or three agent-proposed approaches instead of Approve,
    // then its own Reject and Revise.
    let mut options = Vec::new();
    if let Some(proposed) = display["options"].as_array().filter(|items| items.len() >= 2) {
        if proposed.len() > 3 {
            return None;
        }
        for (index, option) in proposed.iter().enumerate() {
            let label = string(option, "label");
            if label.trim().is_empty()
                || label.chars().count() > 80
                || label.chars().any(char::is_control)
                || ["approve", "reject", "reject and exit", "revise"]
                    .contains(&label.trim().to_lowercase().as_str())
                || options.iter().any(|known: &Value| string(known, "label") == label)
            {
                return None;
            }
            let description: String = string(option, "description").chars().take(4096).collect();
            options.push(json!({"id":format!("option_{}", index + 1),"label":label,"description":description}));
        }
    } else {
        options.push(json!({"id":"approve","label":"Approve","description":""}));
    }
    options.push(json!({"id":"reject","label":"Reject","description":""}));
    let mut card = json!({"question_id":id,"tool_call_id":tool,"agent_id":"main",
        "run_id":record["run_id"],"conversation_id":record["conversation_id"],
        "created_at":event["time"].as_f64().unwrap_or(0.0) / 1000.0,"source":SOURCE,
        "plan_review":true,"plan_path":path,
        "can_answer":false,"answer_transport":"","answer_unavailable_reason":"Review this plan in Terminal.",
        "questions":[{"id":"plan","question":TITLE,"header":"Plan","body":body,
            "multi_select":false,"allow_other":true,"other_label":REVISE,
            "other_description":"Send feedback and keep planning","options":options}]});
    card["question_hash"] = json!(questions::fingerprint(&card));
    Some(card)
}

/// The chooser's labels in native order: the card's options, then Revise.
fn labels(card: &Value) -> Result<Vec<String>> {
    let options = card["questions"][0]["options"]
        .as_array()
        .ok_or("missing plan choices")?;
    let mut labels: Vec<String> = options.iter().map(|o| string(o, "label").to_owned()).collect();
    labels.push(REVISE.into());
    if !(3..=5).contains(&labels.len()) {
        return Err("unsupported plan choices; answer in Terminal".into());
    }
    Ok(labels)
}

#[derive(Debug, PartialEq)]
enum Panel {
    /// The chooser waits for a choice.
    Choosing,
    /// Revise is selected and its feedback line shows this visible text.
    Feedback(String),
}

fn rule(row: &str) -> bool {
    row.chars().count() >= 20 && row.chars().all(|ch| ch == '─')
}

/// A chooser row shows one label; Kimi truncates a row wider than the
/// terminal with an ellipsis.
fn shows(visible: &str, label: &str) -> bool {
    visible == label
        || visible
            .strip_suffix('…')
            .is_some_and(|start| !start.is_empty() && label.starts_with(start))
}

/// The complete chooser at the bottom of the screen: rule, title, blank row,
/// every expected choice in order with exactly one highlighted, blank row,
/// key hint and closing rule.
fn parse(screen: &str, labels: &[String]) -> Result<Panel> {
    let plain = question_terminal::plain(screen)?;
    let rows: Vec<&str> = plain.lines().map(str::trim).collect();
    let heading = format!("▶ {TITLE}");
    let title = rows
        .iter()
        .rposition(|row| *row == heading)
        .ok_or("Kimi's plan review is not visible; open Terminal")?;
    let incomplete = || "Kimi's plan review is not completely visible; enlarge Terminal or answer there";
    if title == 0 || !rule(rows[title - 1]) || rows.get(title + 1) != Some(&"") {
        return Err(incomplete().into());
    }
    let first = title + 2;
    let differ = "Kimi's plan choices differ from this request; open Terminal";
    // Kimi truncates each choice to one row, so every numbered row is a choice.
    let numbered = rows[first..]
        .iter()
        .take_while(|row| {
            let row = row.strip_prefix("▶ ").unwrap_or(row);
            row.split_once(". ")
                .is_some_and(|(number, _)| !number.is_empty() && number.chars().all(|ch| ch.is_ascii_digit()))
        })
        .count();
    let footer = first + numbered + 1;
    if rows.len() <= footer + 1 {
        return Err(incomplete().into());
    }
    if numbered != labels.len() {
        return Err(differ.into());
    }
    if !rows[footer - 1].is_empty() || !rule(rows[footer + 1]) {
        return Err(incomplete().into());
    }
    let feedback = rows[footer].starts_with("Type feedback");
    if !feedback && !rows[footer].starts_with("↑/↓ select · ") {
        return Err(incomplete().into());
    }
    let mut selected = None;
    let mut typed = String::new();
    for (index, label) in labels.iter().enumerate() {
        let row = rows[first + index];
        let number = format!("{}. ", index + 1);
        let (marked, rest) = match row.strip_prefix("▶ ") {
            Some(rest) => (true, rest),
            None => (false, row),
        };
        let visible = rest
            .strip_prefix(&number)
            .ok_or(differ)?;
        if marked && selected.replace(index).is_some() {
            return Err("Kimi's plan review has an ambiguous selection; open Terminal".into());
        }
        if marked && feedback && index + 1 == labels.len() {
            let text = visible
                .strip_prefix(REVISE)
                .ok_or(differ)?;
            if !text.is_empty() && !text.starts_with("  ") {
                return Err(differ.into());
            }
            typed = text.trim_start().to_owned();
        } else if !shows(visible, label) {
            return Err(differ.into());
        }
    }
    match (selected, feedback) {
        (Some(_), false) => Ok(Panel::Choosing),
        (Some(index), true) if index + 1 == labels.len() => Ok(Panel::Feedback(typed)),
        _ => Err("Kimi's plan review has an unexpected selection; open Terminal".into()),
    }
}

/// The chosen chooser index and Revise feedback, if any.
fn chosen(card: &Value, answers: &Value) -> Result<(usize, String)> {
    let labels = labels(card)?;
    let answer = answers
        .as_array()
        .filter(|items| items.len() == 1)
        .map(|items| &items[0])
        .ok_or("answer the plan review")?;
    let text = string(answer, "text").trim().to_owned();
    let ids = answer["selected_option_ids"].as_array().cloned().unwrap_or_default();
    if !text.is_empty() {
        if !ids.is_empty() {
            return Err("choose one plan action".into());
        }
        return Ok((labels.len() - 1, text));
    }
    let [id] = ids.as_slice() else {
        return Err("choose one plan action".into());
    };
    let index = card["questions"][0]["options"]
        .as_array()
        .and_then(|options| options.iter().position(|o| o["id"] == *id))
        .ok_or("unknown plan action")?;
    Ok((index, String::new()))
}

/// Whether Kimi recorded exactly the submitted decision.
pub(super) fn response_matches(card: &Value, answers: &Value, event: &Value) -> bool {
    let (Ok((index, feedback)), Ok(labels)) = (chosen(card, answers), labels(card)) else {
        return false;
    };
    let response = &event["response"];
    let revise = index + 1 == labels.len();
    let reject = index + 2 == labels.len();
    string(event, "type") == "interaction.resolved"
        && string(response, "decision") == if revise || reject { "rejected" } else { "approved" }
        && string(response, "selectedLabel") == labels[index]
        && string(response, "feedback") == feedback
        && response.get("scope").is_none()
}

/// The response as the journal's question-answer summary expects it.
pub(super) fn journal_response(card: &Value, event: &Value) -> Value {
    let response = &event["response"];
    let label = string(response, "selectedLabel");
    let feedback = string(response, "feedback");
    let answer = if feedback.is_empty() { label.to_owned() } else { format!("{label}: {feedback}") };
    json!({"time":event["time"],"response":{"answers":{string(&card["questions"][0], "question"):answer}}})
}

pub(super) fn available(record: &Value, card: &Value) -> Result<()> {
    if string(record, "agent") != "kimi" || string(card, "source") != SOURCE {
        return Err("Not a Kimi plan review".into());
    }
    let native = Native::new(record, card);
    let current = native.identity()?;
    if questions::wire_response(&current, string(card, "question_id"))?.is_some() {
        return Err("This plan review was already answered; nothing was sent".into());
    }
    match parse(&Native::capture(&current)?, &labels(card)?)? {
        Panel::Choosing => Ok(()),
        Panel::Feedback(_) => Err("Finish the Revise feedback in Terminal".into()),
    }
}

pub(super) fn answer(record: &Value, card: &Value, answers: &Value) -> Result<()> {
    available(record, card)?;
    let mut native = Native::new(record, card);
    drive(&mut native, card, answers).map_err(|error| {
        if native.touched() {
            format!("delivery uncertain: {error}; inspect Terminal and do not resend automatically")
        } else {
            error
        }
    })
}

fn wait<T: Transport>(io: &mut T, labels: &[String], expected: impl Fn(&Panel) -> bool) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if expected(&parse(&io.screen()?, labels)?) {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err("Kimi's plan review did not reach the expected state".into());
        }
        io.pause();
    }
}

fn drive<T: Transport>(io: &mut T, card: &Value, answers: &Value) -> Result<()> {
    let labels = labels(card)?;
    let (index, feedback) = chosen(card, answers)?;
    if parse(&io.screen()?, &labels)? != Panel::Choosing {
        return Err("Finish the Revise feedback in Terminal".into());
    }
    // The chooser's own number key selects one choice in a single action;
    // Revise opens its feedback line instead of submitting.
    io.key(&(index + 1).to_string())?;
    if !feedback.is_empty() {
        wait(io, &labels, |panel| *panel == Panel::Feedback(String::new()))?;
        io.paste(&feedback)?;
        // A line wider than the terminal scrolls; its visible end must match.
        wait(io, &labels, |panel| {
            matches!(panel, Panel::Feedback(text) if !text.is_empty() && feedback.ends_with(text.trim_start_matches('…')))
        })?;
        io.key("Enter")?;
    }
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        if let Some(response) = io.resolved()? {
            let event = json!({"type":"interaction.resolved","response":response});
            if !response_matches(card, answers, &event) {
                return Err("Kimi recorded a different plan decision".into());
            }
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err("Kimi has not confirmed the plan decision".into());
        }
        io.pause();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PLAN: &str = "# Rotate fixture keys\n\n1. Add the rotation script.\n2. Verify it.";

    fn record() -> Value {
        json!({"agent":"kimi","run_id":"run","conversation_id":"session_test",
            "active_tools":{"tool_plan":{"name":"ExitPlanMode"}}})
    }
    fn request(options: Value) -> Value {
        let mut display = json!({"kind":"plan_review","plan":PLAN,"path":"/home/user/.kimi-code/plans/example.md"});
        if !options.is_null() {
            display["options"] = options;
        }
        json!({"type":"interaction.request","agentId":"main","id":"approval_plan","kind":"approval",
            "toolCallId":"tool_plan","request":{"id":"approval_plan","toolCallId":"tool_plan",
            "toolName":"ExitPlanMode","action":"Presenting plan and exiting plan mode","display":display},"time":2000})
    }
    fn screen(rows: &[&str], footer: &str) -> String {
        let rule = "─".repeat(60);
        let mut lines = vec!["  │ 2. Verify it.  │".to_owned(), format!(" {rule}"), format!("   ▶ {TITLE}"), String::new()];
        lines.extend(rows.iter().map(|row| format!("   {row}")));
        lines.extend([String::new(), format!("   {footer}"), format!(" {rule}"), " Ask When Needed plan  K3".into()]);
        lines.join("\n")
    }
    const HINT: &str = "↑/↓ select · 1/2/3 choose · ↵ confirm";
    const TYPING: &str = "Type feedback · ↵ submit.";

    #[test]
    fn plan_review_shows_the_plan_and_native_choices() {
        let cards = recover(&record(), &[request(Value::Null)]);
        let [card] = cards.as_slice() else { panic!("{cards:?}") };
        assert_eq!((card["source"].clone(), card["question_id"].clone(), card["created_at"].clone()),
            (json!(SOURCE), json!("approval_plan"), json!(2.0)));
        let item = &card["questions"][0];
        assert_eq!((item["question"].clone(), item["body"].clone(), item["other_label"].clone()),
            (json!(TITLE), json!(PLAN), json!("Revise")));
        assert_eq!(labels(card).unwrap(), ["Approve", "Reject", "Revise"]);
        // Proposed approaches replace Approve.
        let options = json!([{"label":"Script","description":"Rotate with a script"},{"label":"Manual","description":""}]);
        let cards = recover(&record(), &[request(options)]);
        assert_eq!(labels(&cards[0]).unwrap(), ["Script", "Manual", "Reject", "Revise"]);
        assert_eq!(cards[0]["questions"][0]["options"][0]["description"], "Rotate with a script");
    }

    #[test]
    fn resolved_inactive_or_other_approvals_have_no_card() {
        let resolved = json!({"type":"interaction.resolved","agentId":"main","id":"approval_plan"});
        assert!(recover(&record(), &[request(Value::Null), resolved]).is_empty());
        let mut stopped = record();
        stopped["active_tools"] = json!({});
        assert!(recover(&stopped, &[request(Value::Null)]).is_empty());
        let mut bash = request(Value::Null);
        bash["request"]["toolName"] = json!("Bash");
        bash["request"]["display"] = json!({"kind":"command","command":"ls"});
        assert!(recover(&record(), &[bash]).is_empty());
        let reserved = json!([{"label":"Approve"},{"label":"Other"}]);
        assert!(recover(&record(), &[request(reserved)]).is_empty());
        let mut claude = record();
        claude["agent"] = json!("claude");
        assert!(recover(&claude, &[request(Value::Null)]).is_empty());
    }

    #[test]
    fn long_plans_are_clipped_with_their_saved_path() {
        let mut long = request(Value::Null);
        long["request"]["display"]["plan"] = json!("x".repeat(MAX_PLAN_CHARS + 10));
        let body = string(&recover(&record(), &[long])[0]["questions"][0], "body").to_owned();
        assert!(body.ends_with("The complete plan is saved in `/home/user/.kimi-code/plans/example.md`."));
        assert!(body.chars().count() < MAX_PLAN_CHARS + 100);
    }

    #[test]
    fn chooser_must_match_the_request_completely() {
        let labels: Vec<String> = ["Approve", "Reject", "Revise"].map(String::from).into();
        let choosing = screen(&["▶ 1. Approve", "  2. Reject", "  3. Revise"], HINT);
        assert_eq!(parse(&choosing, &labels).unwrap(), Panel::Choosing);
        let typing = screen(&["  1. Approve", "  2. Reject", "▶ 3. Revise  needs a rollback step"], TYPING);
        assert_eq!(parse(&typing, &labels).unwrap(), Panel::Feedback("needs a rollback step".into()));
        let empty = screen(&["  1. Approve", "  2. Reject", "▶ 3. Revise"], TYPING);
        assert_eq!(parse(&empty, &labels).unwrap(), Panel::Feedback(String::new()));
        for wrong in [
            screen(&["▶ 1. Approve", "  2. Reject"], HINT),
            screen(&["▶ 1. Approve", "  2. Reject", "  3. Revise", "  4. Extra"], HINT),
            screen(&["▶ 1. Ship it", "  2. Reject", "  3. Revise"], HINT),
            screen(&["▶ 1. Approve", "▶ 2. Reject", "  3. Revise"], HINT),
            screen(&["  1. Approve", "  2. Reject", "  3. Revise"], HINT),
            screen(&["▶ 1. Approve", "  2. Reject", "  3. Revise"], "Esc to cancel"),
            screen(&["▶ 1. Approve", "  2. Reject", "  3. Revise"], TYPING),
            choosing.replace(TITLE, "Run this command?"),
            choosing.lines().take(7).collect::<Vec<_>>().join("\n"),
        ] {
            assert!(parse(&wrong, &labels).is_err(), "{wrong}");
        }
        let long: Vec<String> = ["Use the scripted rotation path", "Manual", "Reject", "Revise"].map(String::from).into();
        let clipped = screen(&["▶ 1. Use the scripted rot…", "  2. Manual", "  3. Reject", "  4. Revise"], HINT);
        assert_eq!(parse(&clipped, &long).unwrap(), Panel::Choosing);
    }

    #[test]
    fn acknowledgement_must_carry_the_exact_decision() {
        let card = &recover(&record(), &[request(Value::Null)])[0];
        let approve = json!([{"question_id":"plan","selected_option_ids":["approve"],"text":""}]);
        let reject = json!([{"question_id":"plan","selected_option_ids":["reject"],"text":""}]);
        let revise = json!([{"question_id":"plan","selected_option_ids":[],"text":"Add a rollback step"}]);
        let resolved = |response: Value| json!({"type":"interaction.resolved","response":response});
        assert!(response_matches(card, &approve, &resolved(json!({"decision":"approved","selectedLabel":"Approve"}))));
        assert!(!response_matches(card, &approve, &resolved(json!({"decision":"approved","selectedLabel":"Approve","scope":"session"}))));
        assert!(!response_matches(card, &approve, &resolved(json!({"decision":"rejected"}))));
        assert!(response_matches(card, &reject, &resolved(json!({"decision":"rejected","selectedLabel":"Reject"}))));
        assert!(response_matches(card, &revise,
            &resolved(json!({"decision":"rejected","selectedLabel":"Revise","feedback":"Add a rollback step"}))));
        assert!(!response_matches(card, &revise,
            &resolved(json!({"decision":"rejected","selectedLabel":"Revise","feedback":"Add a rollback"}))));
        let logged = journal_response(card, &json!({"time":5000,"response":{"selectedLabel":"Revise","feedback":"Add a rollback step"}}));
        assert_eq!(logged["response"]["answers"][TITLE], "Revise: Add a rollback step");
    }

    struct Fixture {
        screens: Vec<String>,
        keys: Vec<String>,
        pastes: Vec<String>,
        response: Option<Value>,
    }
    impl Transport for Fixture {
        fn screen(&mut self) -> Result<String> {
            Ok(self.screens[0].clone())
        }
        fn key(&mut self, key: &str) -> Result<()> {
            self.keys.push(key.into());
            if self.screens.len() > 1 {
                self.screens.remove(0);
            }
            Ok(())
        }
        fn paste(&mut self, text: &str) -> Result<()> {
            self.pastes.push(text.into());
            if self.screens.len() > 1 {
                self.screens.remove(0);
            }
            Ok(())
        }
        fn resolved(&mut self) -> Result<Option<Value>> {
            Ok(self.response.clone().filter(|_| self.screens.len() == 1))
        }
        fn pause(&mut self) {}
    }

    #[test]
    fn choices_use_one_number_key_and_revise_pastes_verified_feedback() {
        let card = &recover(&record(), &[request(Value::Null)])[0];
        let choosing = screen(&["▶ 1. Approve", "  2. Reject", "  3. Revise"], HINT);
        let mut io = Fixture { screens: vec![choosing.clone(), String::new()], keys: vec![], pastes: vec![],
            response: Some(json!({"decision":"approved","selectedLabel":"Approve"})) };
        drive(&mut io, card, &json!([{"question_id":"plan","selected_option_ids":["approve"],"text":""}])).unwrap();
        assert_eq!(io.keys, ["1"]);
        let feedback = "Add a rollback step";
        let mut io = Fixture {
            screens: vec![
                choosing.clone(),
                screen(&["  1. Approve", "  2. Reject", "▶ 3. Revise"], TYPING),
                screen(&["  1. Approve", "  2. Reject", "▶ 3. Revise  Add a rollback step"], TYPING),
                String::new(),
            ],
            keys: vec![], pastes: vec![],
            response: Some(json!({"decision":"rejected","selectedLabel":"Revise","feedback":feedback})),
        };
        drive(&mut io, card, &json!([{"question_id":"plan","selected_option_ids":[],"text":feedback}])).unwrap();
        assert_eq!((io.keys, io.pastes), (vec!["3".to_owned(), "Enter".into()], vec![feedback.to_owned()]));
        // A different recorded decision is reported, never accepted.
        let mut io = Fixture { screens: vec![choosing, String::new()], keys: vec![], pastes: vec![],
            response: Some(json!({"decision":"rejected","selectedLabel":"Reject"})) };
        assert!(drive(&mut io, card, &json!([{"question_id":"plan","selected_option_ids":["approve"],"text":""}])).is_err());
    }
}
