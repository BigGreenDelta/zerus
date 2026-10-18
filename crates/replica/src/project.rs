//! A project's task settings as one small Loro document.
use crate::schema::{check_controls, Actor, SessionKey, SCHEMA};
use crate::{Error, Result};
use loro::{ExportMode, LoroDoc, LoroValue, VersionVector};
use serde::{Deserialize, Serialize};

pub const DEFAULT_TASK_TEMPLATE: &str = "[Zerus task {ref}]\nWritten by {author}.\n{prompt}\n{history}\nWhen you finish, or find that it cannot be done, report with\n{tools}.task_report: one to three sentences on what you changed, how you\nchecked it, and what is left undone or why it cannot be done.";
pub const DEFAULT_CANCEL_TEMPLATE: &str = "[Zerus task {ref} cancelled]\nThe person cancelled this task. Stop working on it. Do not continue and do\nnot revert anything; list what you changed and wait for the person.";
const MAX_TEMPLATE: usize = 16 * 1024;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Settings {
    pub task_template: String,
    pub cancel_template: String,
    /// Agents may start and cancel tasks in other sessions.
    pub agents_other: bool,
    /// Tasks agents may start per hour in the project; 0 is no limit.
    pub agent_limit: i64,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            task_template: DEFAULT_TASK_TEMPLATE.into(),
            cancel_template: DEFAULT_CANCEL_TEMPLATE.into(),
            agents_other: true,
            agent_limit: 0,
        }
    }
}

impl Settings {
    /// Refuses an agent's start or cancel that the project's settings forbid.
    /// `starts_last_hour` counts the tasks agents started in the past hour.
    pub fn check_agent(
        &self,
        actor: &Actor,
        session: &SessionKey,
        starting: bool,
        starts_last_hour: usize,
    ) -> Result<()> {
        let Actor::Agent(own) = actor else {
            return Ok(());
        };
        if !self.agents_other && own != session {
            return Err(Error::new(
                "this project lets agents start and cancel tasks only in their own session",
            ));
        }
        if starting && self.agent_limit > 0 && starts_last_hour as i64 >= self.agent_limit {
            return Err(Error::new(format!(
                "this project allows {} tasks started by agents per hour; try again later",
                self.agent_limit
            )));
        }
        Ok(())
    }
}

pub struct ProjectDoc {
    doc: LoroDoc,
}

impl Default for ProjectDoc {
    fn default() -> Self {
        Self::new()
    }
}

impl ProjectDoc {
    pub fn new() -> Self {
        Self {
            doc: LoroDoc::new(),
        }
    }

    pub fn from_bytes(bytes: &[u8]) -> Result<Self> {
        let doc = LoroDoc::new();
        doc.import(bytes)?;
        Ok(Self { doc })
    }

    pub fn import(&mut self, bytes: &[u8]) -> Result<()> {
        self.doc.import(bytes)?;
        Ok(())
    }

    pub fn snapshot(&self) -> Result<Vec<u8>> {
        Ok(self.doc.export(ExportMode::Snapshot)?)
    }

    pub fn version(&self) -> Vec<u8> {
        self.doc.oplog_vv().encode()
    }

    pub fn export_since(&self, version: &[u8]) -> Result<Vec<u8>> {
        Ok(self
            .doc
            .export(ExportMode::updates(&VersionVector::decode(version)?))?)
    }

    pub fn settings(&self) -> Settings {
        let map = self.doc.get_map("settings");
        let mut settings = Settings::default();
        let value = |key: &str| map.get(key).and_then(|v| v.into_value().ok());
        if let Some(LoroValue::String(s)) = value("task_template") {
            settings.task_template = s.to_string();
        }
        if let Some(LoroValue::String(s)) = value("cancel_template") {
            settings.cancel_template = s.to_string();
        }
        if let Some(LoroValue::Bool(b)) = value("agents_other") {
            settings.agents_other = b;
        }
        if let Some(LoroValue::I64(n)) = value("agent_limit") {
            settings.agent_limit = n;
        }
        settings
    }

    pub fn set_settings(&mut self, settings: &Settings) -> Result<()> {
        for (name, template) in [
            ("task prompt", &settings.task_template),
            ("cancel message", &settings.cancel_template),
        ] {
            if !template.starts_with("[Zerus task") {
                return Err(Error::new(format!(
                    "the {name} template must start with its [Zerus task …] line"
                )));
            }
            if template.len() > MAX_TEMPLATE {
                return Err(Error::new(format!(
                    "the {name} template is longer than 16 KiB"
                )));
            }
            check_controls(template, &format!("{name} template"))?;
        }
        if settings.agent_limit < 0 {
            return Err(Error::new("the agent limit cannot be negative"));
        }
        let map = self.doc.get_map("settings");
        map.insert("schema", SCHEMA)?;
        map.insert("task_template", settings.task_template.as_str())?;
        map.insert("cancel_template", settings.cancel_template.as_str())?;
        map.insert("agents_other", settings.agents_other)?;
        map.insert("agent_limit", settings.agent_limit)?;
        self.doc.commit();
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn templates_refuse_terminal_control_characters() {
        let mut project = ProjectDoc::new();
        let settings = Settings {
            task_template: format!("{DEFAULT_TASK_TEMPLATE}\x1b[31m"),
            ..Settings::default()
        };
        let error = project.set_settings(&settings).unwrap_err();
        assert!(error.to_string().contains("control characters"), "{error}");
    }

    #[test]
    fn defaults_then_validated_changes() {
        let mut project = ProjectDoc::new();
        assert_eq!(project.settings(), Settings::default());
        let mut changed = Settings {
            agents_other: false,
            agent_limit: 5,
            ..Settings::default()
        };
        project.set_settings(&changed).unwrap();
        let reloaded = ProjectDoc::from_bytes(&project.snapshot().unwrap()).unwrap();
        assert_eq!(reloaded.settings(), changed);
        changed.task_template = "Do {prompt}".into();
        assert!(project.set_settings(&changed).is_err());
        changed.task_template = DEFAULT_TASK_TEMPLATE.into();
        changed.agent_limit = -1;
        assert!(project.set_settings(&changed).is_err());
    }

    #[test]
    fn agent_rules() {
        let own = SessionKey {
            machine: "m".into(),
            session: "codex/api".into(),
        };
        let other = SessionKey {
            machine: "m".into(),
            session: "codex/web".into(),
        };
        let agent = Actor::Agent(own.clone());
        let mut settings = Settings::default();
        assert!(settings.check_agent(&agent, &other, true, 100).is_ok());
        settings.agents_other = false;
        assert!(settings.check_agent(&agent, &other, true, 0).is_err());
        assert!(settings.check_agent(&agent, &own, true, 0).is_ok());
        assert!(settings
            .check_agent(&Actor::Person, &other, true, 0)
            .is_ok());
        settings.agent_limit = 2;
        assert!(settings.check_agent(&agent, &own, true, 1).is_ok());
        assert!(settings.check_agent(&agent, &own, true, 2).is_err());
        assert!(settings.check_agent(&agent, &own, false, 2).is_ok());
    }
}
