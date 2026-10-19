//! The task operations that need their project: where a task goes in its
//! list and whether its prompt could be sent. hgs and the mobile apps use
//! these, so the rules hold the same everywhere.
use crate::render::{task_prompt, Tools};
use crate::schema::{Actor, Stage, Target};
use crate::store::{Position, ProjectStore};
use crate::task::{check_new_stage, NewTask, TaskDoc};
use crate::Result;

impl ProjectStore {
    /// Refuses a task whose prompt could not be sent: the rendered prompt
    /// follows hgs's input limits. `project_name` names the project in it.
    pub fn check_prompt(&self, task: &TaskDoc, project_name: &str) -> Result<()> {
        let settings = self.project()?.settings();
        task_prompt(
            &settings.task_template,
            &task.view()?,
            project_name,
            Tools::Cli,
        )
        .map(|_| ())
    }

    /// A new task at `position` of its stage, To do or Backlog; unsaved.
    #[allow(clippy::too_many_arguments)]
    pub fn new_task(
        &self,
        text: &str,
        stage: Stage,
        target: Target,
        author: Actor,
        position: &Position,
        project_name: &str,
        now: i64,
    ) -> Result<TaskDoc> {
        // Before the rank: placing it in another stage could renumber that stage.
        check_new_stage(stage)?;
        let rank = self.rank_for(stage, position, None)?;
        let id = self.unused_short_id()?;
        let task = TaskDoc::create(NewTask {
            project: self.project_id(),
            id: &id,
            text,
            stage,
            target,
            author,
            rank: &rank,
            now,
        })?;
        self.check_prompt(&task, project_name)?;
        Ok(task)
    }

    /// Replaces the prompt of a task in To do or Backlog.
    pub fn edit_task(
        &self,
        task: &mut TaskDoc,
        text: &str,
        actor: &Actor,
        project_name: &str,
        now: i64,
    ) -> Result<()> {
        task.edit_text(text, actor, now)?;
        self.check_prompt(task, project_name)
    }

    /// Moves a task to stage `to`; a task that changes list goes to the top
    /// of its new list.
    pub fn change_stage(
        &self,
        task: &mut TaskDoc,
        to: Stage,
        actor: &Actor,
        now: i64,
    ) -> Result<()> {
        let from = task.stage()?;
        task.set_stage(to, actor, now)?;
        if from != to && matches!(to, Stage::Todo | Stage::Backlog) {
            let rank = self.rank_for(to, &Position::Top, Some(task.uuid()))?;
            task.set_rank(&rank)?;
        }
        Ok(())
    }

    /// Puts a task of To do or Backlog at `position` of its list.
    pub fn reorder(&self, task: &mut TaskDoc, position: &Position) -> Result<()> {
        let rank = self.rank_for(task.stage()?, position, Some(task.uuid()))?;
        task.set_rank(&rank)
    }
}

#[cfg(test)]
mod tests {
    use crate::{Actor, ErrorKind, Position, ProjectStore, Stage, Target, MAX_TEXT};

    fn open() -> (tempfile::TempDir, ProjectStore) {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        (root, store)
    }

    fn file(store: &ProjectStore, text: &str, stage: Stage, position: Position) -> String {
        let mut task = store
            .new_task(
                text,
                stage,
                Target::Any,
                Actor::Person,
                &position,
                "orchard",
                1,
            )
            .unwrap();
        store.save(&mut task).unwrap();
        task.uuid().to_owned()
    }

    fn texts(store: &ProjectStore, stage: Stage) -> Vec<String> {
        store
            .ordered(stage)
            .unwrap()
            .iter()
            .map(|t| t.view().unwrap().text)
            .collect()
    }

    #[test]
    fn a_new_task_goes_where_asked_and_only_to_todo_or_backlog() {
        let (_root, store) = open();
        file(&store, "Second", Stage::Todo, Position::End);
        file(&store, "First", Stage::Todo, Position::Top);
        file(&store, "Later", Stage::Backlog, Position::End);
        assert_eq!(texts(&store, Stage::Todo), ["First", "Second"]);
        assert_eq!(texts(&store, Stage::Backlog), ["Later"]);
        for stage in [Stage::Active, Stage::Review, Stage::Done] {
            let error = store
                .new_task(
                    "Nope",
                    stage,
                    Target::Any,
                    Actor::Person,
                    &Position::End,
                    "orchard",
                    1,
                )
                .err()
                .unwrap();
            assert_eq!(error.kind(), ErrorKind::Refused);
            assert_eq!(error.to_string(), "a new task goes to Backlog or To do");
        }
    }

    #[test]
    fn a_prompt_that_could_not_be_sent_is_refused() {
        let (_root, store) = open();
        let text = "x".repeat(MAX_TEXT);
        let error = store
            .new_task(
                &text,
                Stage::Todo,
                Target::Any,
                Actor::Person,
                &Position::End,
                "orchard",
                1,
            )
            .err()
            .unwrap();
        assert_eq!(error.kind(), ErrorKind::Refused);
        let uuid = file(&store, "Short", Stage::Todo, Position::End);
        let mut task = store.load(&uuid).unwrap();
        assert!(store
            .edit_task(&mut task, &text, &Actor::Person, "orchard", 2)
            .is_err());
        store
            .edit_task(&mut task, "Still short", &Actor::Person, "orchard", 2)
            .unwrap();
        assert_eq!(task.view().unwrap().text, "Still short");
    }

    #[test]
    fn a_task_that_changes_list_goes_to_its_top() {
        let (_root, store) = open();
        file(&store, "Old backlog", Stage::Backlog, Position::End);
        let uuid = file(&store, "Moved", Stage::Todo, Position::End);
        let mut task = store.load(&uuid).unwrap();
        store
            .change_stage(&mut task, Stage::Backlog, &Actor::Person, 2)
            .unwrap();
        store.save(&mut task).unwrap();
        assert_eq!(texts(&store, Stage::Backlog), ["Moved", "Old backlog"]);
    }

    #[test]
    fn reorder_moves_a_task_within_its_list() {
        let (_root, store) = open();
        file(&store, "One", Stage::Todo, Position::End);
        file(&store, "Two", Stage::Todo, Position::End);
        let uuid = file(&store, "Three", Stage::Todo, Position::End);
        let mut task = store.load(&uuid).unwrap();
        store.reorder(&mut task, &Position::Index(0)).unwrap();
        store.save(&mut task).unwrap();
        assert_eq!(texts(&store, Stage::Todo), ["Three", "One", "Two"]);
    }
}
