//! Workflows are data. The engine understands stage roles, never stage names.
use super::model::{ActorKind, Refusal};
use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Role {
    Backlog,
    Ready,
    Active,
    Review,
    NeedsHuman,
    Done,
    Parked,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Action {
    Create,
    Move,
    Claim,
    Unclaim,
    Advance,
    CallHuman,
    Answer,
    Return,
    Decompose,
    Approve,
    NeedsWork,
    Done,
    Reopen,
}

impl Action {
    pub const ALL: [Action; 13] = [
        Action::Create, Action::Move, Action::Claim, Action::Unclaim, Action::Advance, Action::CallHuman,
        Action::Answer, Action::Return, Action::Decompose, Action::Approve, Action::NeedsWork, Action::Done,
        Action::Reopen,
    ];
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Evidence {
    /// A commit that exists in the session's repository; free text outside Git.
    GitShaOrText,
    Text,
    None,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Stage {
    pub name: String,
    pub role: Role,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rule {
    pub action: Action,
    #[serde(default)]
    pub from: Vec<String>,
    pub to: String,
    pub actors: Vec<ActorKind>,
    #[serde(default)]
    pub requires: Vec<String>,
    /// The acting agent's session must be the task's assignee.
    #[serde(default)]
    pub own: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Workflow {
    pub version: u32,
    pub stages: Vec<Stage>,
    pub rules: Vec<Rule>,
    pub evidence: Evidence,
    pub session_wip: u32,
}

/// Target of `answer`: the active stage the task left for Your Call.
pub const RESUME: &str = "@resume";
pub const FIELDS: &[&str] = &[
    "spec", "worklog", "evidence", "root_cause", "question", "answer", "reason", "report", "breaks",
    "decomposed_from",
];

fn rule(action: Action, from: &[&str], to: &str, actors: &[ActorKind], requires: &[&str], own: bool) -> Rule {
    Rule {
        action,
        from: from.iter().map(|s| s.to_string()).collect(),
        to: to.into(),
        actors: actors.to_vec(),
        requires: requires.iter().map(|s| s.to_string()).collect(),
        own,
    }
}

fn blank(value: Option<&String>) -> Option<&'static str> {
    match value {
        None => Some("missing"),
        Some(v) if v.trim().is_empty() => Some("empty"),
        Some(_) => None,
    }
}

impl Workflow {
    /// The vikunja-mcp process: Backlog → Queue → Design → Build → Review → Done.
    pub fn default_v1() -> Self {
        use Action as A;
        use ActorKind::*;
        let stage = |name: &str, role| Stage { name: name.into(), role };
        let working = ["Backlog", "Queue", "Design", "Build", "Review", "Your Call"];
        Self {
            version: 1,
            stages: vec![
                stage("Backlog", Role::Backlog),
                stage("Queue", Role::Ready),
                stage("Design", Role::Active),
                stage("Build", Role::Active),
                stage("Review", Role::Review),
                stage("Your Call", Role::NeedsHuman),
                stage("Done", Role::Done),
                stage("Icebox", Role::Parked),
            ],
            rules: vec![
                rule(A::Create, &[], "Backlog", &[Human, Manager], &[], false),
                rule(A::Create, &[], "Backlog", &[Agent], &["breaks"], false),
                rule(A::Create, &[], "Queue", &[Human, Manager], &[], false),
                rule(A::Create, &[], "Queue", &[Agent], &["decomposed_from"], false),
                rule(A::Move, &["Backlog"], "Queue", &[Human], &[], false),
                rule(A::Move, &["Queue"], "Backlog", &[Human], &[], false),
                rule(A::Claim, &["Queue"], "Design", &[Human, Autopilot], &[], false),
                rule(A::Advance, &["Design"], "Build", &[Agent], &["spec"], true),
                rule(A::Advance, &["Build"], "Review", &[Agent], &["worklog", "evidence"], true),
                rule(A::CallHuman, &["Design", "Build"], "Your Call", &[Agent], &["question"], true),
                rule(A::Answer, &["Your Call"], RESUME, &[Human], &["answer"], false),
                rule(A::Return, &["Backlog", "Queue", "Design", "Build", "Your Call"], "Backlog", &[Agent], &["reason"], true),
                rule(A::Decompose, &["Design", "Build"], "Backlog", &[Agent], &[], true),
                rule(A::Approve, &["Review"], "Review", &[Agent, Human], &["report"], false),
                rule(A::NeedsWork, &["Review"], "Build", &[Agent, Human], &["report"], false),
                rule(A::Done, &["Review"], "Done", &[Human], &[], false),
                rule(A::Unclaim, &["Design", "Build"], "Queue", &[Human, Autopilot], &[], false),
                rule(A::Move, &working, "Icebox", &[Human], &[], false),
                rule(A::Move, &["Icebox"], "Backlog", &[Human], &[], false),
                rule(A::Move, &["Icebox"], "Queue", &[Human], &[], false),
                rule(A::Reopen, &["Done"], "Queue", &[Human], &[], false),
            ],
            evidence: Evidence::GitShaOrText,
            session_wip: 1,
        }
    }

    pub fn stage(&self, name: &str) -> Option<&Stage> {
        self.stages.iter().find(|s| s.name == name)
    }

    pub fn role(&self, name: &str) -> Option<Role> {
        self.stage(name).map(|s| s.role)
    }

    pub fn first(&self, role: Role) -> Option<&Stage> {
        self.stages.iter().find(|s| s.role == role)
    }

    pub fn validate(&self) -> Result<()> {
        ensure!(self.version == 1, "workflow version must be 1");
        ensure!(!self.stages.is_empty() && self.stages.len() <= 32, "a workflow has 1 to 32 stages");
        let mut names = BTreeSet::new();
        for stage in &self.stages {
            let length = stage.name.chars().count();
            ensure!(
                (1..=40).contains(&length) && !stage.name.chars().any(char::is_control) && !stage.name.starts_with('@'),
                "stage names are 1 to 40 printable characters and do not start with @"
            );
            ensure!(
                stage.name.trim() == stage.name,
                "stage names have no leading or trailing whitespace"
            );
            ensure!(names.insert(stage.name.as_str()), "duplicate stage {}", stage.name);
        }
        let count = |role: Role| self.stages.iter().filter(|s| s.role == role).count();
        ensure!(count(Role::Done) == 1, "a workflow needs exactly one done stage");
        for role in [Role::Backlog, Role::Ready, Role::Active, Role::Review, Role::NeedsHuman] {
            ensure!(count(role) >= 1, "a workflow needs a {role:?} stage");
        }
        ensure!(self.session_wip >= 1, "session_wip must be at least 1");
        ensure!(self.rules.len() <= 200, "a workflow has at most 200 rules");
        for rule in &self.rules {
            ensure!(!rule.actors.is_empty(), "every rule names its actors");
            ensure!(
                !rule.actors.contains(&ActorKind::Unknown),
                "a rule names an unknown actor kind"
            );
            if rule.to == RESUME {
                ensure!(rule.action == Action::Answer, "only answer rules may target {RESUME}");
                for name in &rule.from {
                    ensure!(
                        self.role(name) == Some(Role::NeedsHuman),
                        "answer rules may only leave the human stage, not {name}"
                    );
                }
            }
            ensure!((rule.action == Action::Create) == rule.from.is_empty(), "only create rules have no source stage");
            for name in &rule.from {
                ensure!(names.contains(name.as_str()), "a rule refers to unknown stage {name}");
            }
            let resume = rule.action == Action::Answer && rule.to == RESUME;
            ensure!(resume || names.contains(rule.to.as_str()), "a rule refers to unknown stage {}", rule.to);
            for field in &rule.requires {
                ensure!(FIELDS.contains(&field.as_str()), "unknown required field {field}");
            }
            if self.role(&rule.to) == Some(Role::Done) {
                ensure!(
                    rule.actors.iter().all(|a| *a == ActorKind::Human),
                    "only people may move tasks to the done stage"
                );
            }
        }
        let mut reached: BTreeSet<&str> =
            self.rules.iter().filter(|r| r.action == Action::Create).map(|r| r.to.as_str()).collect();
        loop {
            let before = reached.len();
            for rule in &self.rules {
                if rule.to != RESUME && rule.from.iter().any(|f| reached.contains(f.as_str())) {
                    reached.insert(rule.to.as_str());
                }
            }
            if reached.len() == before {
                break;
            }
        }
        for stage in &self.stages {
            ensure!(reached.contains(stage.name.as_str()), "stage {} cannot be reached", stage.name);
        }
        Ok(())
    }

    /// The rule allowing `action` from `from` (None when creating) to `to`.
    /// A rule that matches but lacks a field refuses with that field.
    pub fn allow(
        &self,
        action: Action,
        from: Option<&str>,
        to: &str,
        actor: ActorKind,
        fields: &BTreeMap<String, String>,
    ) -> Result<&Rule, Refusal> {
        if actor == ActorKind::Unknown {
            return Err(Refusal::new(
                "transition_not_allowed",
                format!("{to} is not allowed for an actor kind this build does not know"),
            ));
        }
        let candidates: Vec<&Rule> = self
            .rules
            .iter()
            .filter(|r| {
                r.action == action
                    && r.to == to
                    && r.actors.contains(&actor)
                    && match from {
                        None => r.from.is_empty(),
                        Some(stage) => r.from.iter().any(|f| f == stage),
                    }
            })
            .collect();
        let mut first_missing = None;
        for rule in &candidates {
            let missing = rule.requires.iter().find_map(|f| blank(fields.get(f)).map(|state| (f.clone(), state)));
            match missing {
                None => return Ok(rule),
                Some(found) => {
                    if first_missing.is_none() {
                        first_missing = Some(found);
                    }
                }
            }
        }
        if let Some((field, state)) = first_missing {
            return Err(Refusal::field(
                "missing_field",
                &field,
                state,
                format!("this action needs `{field}`, which arrived {state}"),
            ));
        }
        Err(Refusal::new(
            "transition_not_allowed",
            format!(
                "{} from {} to {to} is not allowed for {}",
                serde_json::to_value(action).unwrap().as_str().unwrap_or("action"),
                from.unwrap_or("nothing"),
                serde_json::to_value(actor).unwrap().as_str().unwrap_or("this actor")
            ),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ActorKind::*;

    fn fields(pairs: &[(&str, &str)]) -> BTreeMap<String, String> {
        pairs.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
    }

    #[test]
    fn default_workflow_is_valid() {
        Workflow::default_v1().validate().unwrap();
    }

    #[test]
    fn advance_needs_a_spec_and_names_how_it_arrived() {
        let wf = Workflow::default_v1();
        let missing = wf.allow(Action::Advance, Some("Design"), "Build", Agent, &fields(&[])).unwrap_err();
        assert_eq!((missing.code.as_str(), missing.field.as_str(), missing.state.as_str()), ("missing_field", "spec", "missing"));
        let empty = wf.allow(Action::Advance, Some("Design"), "Build", Agent, &fields(&[("spec", "  ")])).unwrap_err();
        assert_eq!(empty.state, "empty");
        assert!(wf.allow(Action::Advance, Some("Design"), "Build", Agent, &fields(&[("spec", "plan")])).is_ok());
    }

    #[test]
    fn creation_rules_depend_on_the_actor() {
        let wf = Workflow::default_v1();
        assert!(wf.allow(Action::Create, None, "Queue", Human, &fields(&[])).is_ok());
        assert_eq!(wf.allow(Action::Create, None, "Queue", Agent, &fields(&[])).unwrap_err().field, "decomposed_from");
        assert_eq!(wf.allow(Action::Create, None, "Done", Human, &fields(&[])).unwrap_err().code, "transition_not_allowed");
        assert_eq!(wf.allow(Action::Create, None, "Backlog", Agent, &fields(&[])).unwrap_err().field, "breaks");
        assert!(wf.allow(Action::Create, None, "Backlog", Agent, &fields(&[("breaks", "x")])).is_ok());
        assert!(wf.allow(Action::Create, None, "Queue", Agent, &fields(&[("decomposed_from", "u")])).is_ok());
    }

    #[test]
    fn agents_never_reach_done() {
        let wf = Workflow::default_v1();
        for action in Action::ALL {
            assert!(wf.allow(action, Some("Review"), "Done", Agent, &fields(&[("report", "r")])).is_err());
        }
    }

    #[test]
    fn invalid_workflows_are_refused() {
        let mut duplicate = Workflow::default_v1();
        duplicate.stages.push(Stage { name: "Queue".into(), role: Role::Ready });
        assert!(duplicate.validate().is_err());
        let mut no_done = Workflow::default_v1();
        no_done.stages.retain(|s| s.role != Role::Done);
        no_done.rules.retain(|r| r.to != "Done" && !r.from.contains(&"Done".to_string()));
        assert!(no_done.validate().is_err());
        let mut unreachable = Workflow::default_v1();
        unreachable.stages.push(Stage { name: "Limbo".into(), role: Role::Parked });
        assert!(unreachable.validate().is_err());
        let mut agent_done = Workflow::default_v1();
        agent_done.rules.push(Rule { action: Action::Move, from: vec!["Build".into()], to: "Done".into(), actors: vec![Agent], requires: vec![], own: false });
        assert!(agent_done.validate().is_err());
        let mut bad_field = Workflow::default_v1();
        bad_field.rules[0].requires.push("password".into());
        assert!(bad_field.validate().is_err());
    }

    #[test]
    fn unknown_actor_kinds_never_satisfy_a_rule() {
        let rule: Rule = serde_json::from_str(
            r#"{"action":"move","from":["Backlog"],"to":"Queue","actors":["bogus"]}"#,
        )
        .unwrap();
        assert_eq!(rule.actors, vec![Unknown]);
        let mut wf = Workflow::default_v1();
        wf.rules.push(rule);
        assert!(wf.validate().is_err());
        let refusal = Workflow::default_v1()
            .allow(Action::Move, Some("Backlog"), "Queue", Unknown, &fields(&[]))
            .unwrap_err();
        assert_eq!(refusal.code, "transition_not_allowed");
    }

    #[test]
    fn resume_answers_only_come_from_the_human_stage() {
        let mut wf = Workflow::default_v1();
        wf.rules.push(Rule { action: Action::Answer, from: vec!["Done".into()], to: RESUME.into(), actors: vec![Human], requires: vec!["answer".into()], own: false });
        assert!(wf.validate().is_err());
        assert!(Workflow::default_v1().validate().is_ok());
    }

    #[test]
    fn humans_may_finish_a_reviewed_task() {
        let wf = Workflow::default_v1();
        assert!(wf.allow(Action::Done, Some("Review"), "Done", Human, &fields(&[])).is_ok());
    }

    #[test]
    fn stage_names_and_session_wip_are_checked() {
        let mut blank = Workflow::default_v1();
        blank.stages.push(Stage { name: "   ".into(), role: Role::Parked });
        assert!(blank.validate().is_err());
        let mut padded = Workflow::default_v1();
        padded.stages.push(Stage { name: " Idle".into(), role: Role::Parked });
        padded.rules.push(Rule { action: Action::Move, from: vec!["Backlog".into()], to: " Idle".into(), actors: vec![Human], requires: vec![], own: false });
        assert!(padded.validate().is_err());
        let mut no_wip = Workflow::default_v1();
        no_wip.session_wip = 0;
        assert!(no_wip.validate().is_err());
    }
}
