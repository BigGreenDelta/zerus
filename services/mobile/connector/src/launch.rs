//! Session launch, folder creation and worktree creation (`launch.py`,
//! `worktrees.py` and `connector.py:911-1052`): Python's validators, fresh
//! native preflights, one fixed native command per request and only its
//! exact receipt. A preflight failure never reaches the mutation, so its
//! receipt is failed; once the mutation runs, any doubt is uncertain and the
//! journal's no-replay identity keeps it from running again. Nothing here
//! retries a mutation or launches a session on its own.
use crate::accounts::{launch_catalog, PROVIDERS};
use crate::execute::{absolute, py, worktree_catalog, Failure};
use crate::native::{Call, Native};
use crate::projects::{is_archived, launch_projects};
use crate::pyvalue::{bounded, is_str, is_true, py_eq, py_isspace, py_iter, py_strip, truthy};
use crate::pyvalue::{PyError, PyResult};
use crate::runtime::Connector;
use serde_json::{json, Map, Value};
use std::cell::Cell;
use std::time::Duration;
use zerus_relay::protocol::opencode_effort_id;

/// How long a launch is looked for in the listing by its launch UUID.
pub const VERIFY_SECONDS: f64 = 5.0;
/// Native budgets: catalogs and folders, the launch, the project catalog,
/// the listing during verification and the worktree supervisor.
pub const CATALOG_TIMEOUT: f64 = 5.0;
pub const LAUNCH_TIMEOUT: f64 = 15.0;
pub const SWARM_TIMEOUT: f64 = 3.0;
pub const LISTING_TIMEOUT: f64 = 3.0;
pub const WORKTREE_CREATE_TIMEOUT: f64 = 35.0;

const FIELDS: &str = "invalid scoped catalog/launch fields";
const WORKTREE_FIELDS: &str = "invalid scoped worktree fields";
const NOT_CANONICAL: &str = "native directory path is not canonical";
pub const UNVERIFIED: &str =
    "native launch could not be verified by exact launch UUID; check computers before creating another";
pub const ACCOUNT_UNAVAILABLE: &str = "selected native provider/account is unavailable";
pub const MODEL_WITHDRAWN: &str = "selected native model is no longer offered by this account";
pub const DIRECTORY_UNAVAILABLE: &str = "native directory is unavailable";
pub const FOLDER_CHANGED: &str = "selected local project folder changed";
pub const REPOSITORY_CHANGED: &str = "repository changed; refresh worktrees before creation";
const NOT_RELATED: &str =
    "choose a current project folder, a verified related worktree, or explicitly add the folder";

fn invalid(message: &str) -> PyError {
    PyError::value(message)
}

/// `pathlib.PurePosixPath`: a root of "", "/" or "//" (exactly two leading
/// slashes stay two) and the components left once empty and "." ones are
/// dropped. ".." stays a component.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PurePath {
    root: &'static str,
    parts: Vec<String>,
}

impl PurePath {
    pub fn new(path: &str) -> PurePath {
        let (root, rest) = match path.strip_prefix("//") {
            Some(rest) if !rest.starts_with('/') => ("//", rest),
            Some(_) => ("/", &path[1..]),
            None => match path.strip_prefix('/') {
                Some(rest) => ("/", rest),
                None => ("", path),
            },
        };
        let parts = rest
            .split('/')
            .filter(|part| !part.is_empty() && *part != ".")
            .map(str::to_owned)
            .collect();
        PurePath { root, parts }
    }

    /// `path.name`.
    pub fn name(&self) -> &str {
        self.parts.last().map_or("", String::as_str)
    }

    /// `path.parent`.
    pub fn parent(&self) -> PurePath {
        let mut parent = self.clone();
        parent.parts.pop();
        parent
    }

    /// `path / name` for one relative component.
    pub fn join(&self, name: &str) -> PurePath {
        let mut joined = self.clone();
        joined.parts.extend(PurePath::new(name).parts);
        joined
    }

    pub fn has_parent_reference(&self) -> bool {
        self.parts.iter().any(|part| part == "..")
    }
}

impl std::fmt::Display for PurePath {
    /// `str(path)`: "." for an empty relative path.
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.root.is_empty() && self.parts.is_empty() {
            return f.write_str(".");
        }
        write!(f, "{}{}", self.root, self.parts.join("/"))
    }
}

/// `re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_-]*', value)`.
fn account_identifier(value: &str) -> bool {
    let mut bytes = value.bytes();
    bytes
        .next()
        .is_some_and(|b| b.is_ascii_alphanumeric() || b == b'_')
        && bytes.all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// `re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}', provider)`.
fn model_provider(provider: &str) -> bool {
    let mut bytes = provider.bytes();
    provider.len() <= 80
        && bytes
            .next()
            .is_some_and(|b| b.is_ascii_alphanumeric() || b == b'_')
        && bytes.all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}

/// An identifier Python bounds by code points and by UTF-8 bytes, without
/// surrounding whitespace.
fn identifier(value: Option<&Value>, maximum: usize, message: &str) -> PyResult<String> {
    let value = bounded(value, maximum, false)?;
    if py_strip(&value) != value || value.len() > maximum {
        return Err(invalid(message));
    }
    Ok(value)
}

/// `launch.directory_destination(value)`: a literal absolute child path.
pub fn directory_destination(value: Option<&Value>) -> PyResult<String> {
    let value = bounded(value, 4096, false)?;
    let name = value.rsplit('/').next().unwrap_or_default();
    if !value.starts_with('/') || value.len() > 4096 || matches!(name, "" | "." | "..") {
        return Err(invalid("new directory must be an absolute child path"));
    }
    Ok(value)
}

/// `launch.canonical_directory(value)`.
pub fn canonical_directory(value: Option<&Value>) -> PyResult<String> {
    let value = bounded(value, 4096, false)?;
    let path = PurePath::new(&value);
    if !value.starts_with('/')
        || value.len() > 4096
        || path.to_string() != value
        || value.starts_with("//")
        || path.has_parent_reference()
    {
        return Err(invalid(NOT_CANONICAL));
    }
    Ok(value)
}

/// `launch.directory_created(raw, payload, request_id, parent)`: only the
/// exact native receipt may select the new folder on the phone.
pub fn directory_created(
    raw: &Value,
    requested: &str,
    request_id: &str,
    parent: &str,
) -> PyResult<Value> {
    let name = requested.rsplit('/').next().unwrap_or_default();
    let expected = PurePath::new(parent).join(name).to_string();
    let confirmed = raw.as_object().is_some_and(|raw| {
        is_str(raw.get("status"), "created")
            && is_str(raw.get("request_id"), request_id)
            && is_str(raw.get("requested_path"), requested)
            && is_str(raw.get("parent"), parent)
            && is_str(raw.get("path"), &expected)
    });
    if !confirmed {
        return Err(invalid(
            "native directory creation was not confirmed by exact identity",
        ));
    }
    Ok(json!({
        "status": "created", "request_id": request_id, "requested_path": requested,
        "path": canonical_directory(raw.get("path"))?,
        "parent": canonical_directory(raw.get("parent"))?,
    }))
}

/// `worktrees.created(raw, payload, request_id, destination)`.
pub fn worktree_created(
    raw: &Value,
    payload: &Value,
    request_id: &str,
    destination: &str,
) -> PyResult<Value> {
    let confirmed = raw.as_object().is_some_and(|raw| {
        is_str(raw.get("request_id"), request_id)
            && is_str(raw.get("status"), "created")
            && raw
                .get("common_dir")
                .is_some_and(|value| py_eq(value, &payload["common_dir"]))
            && raw
                .get("branch")
                .is_some_and(|value| py_eq(value, &payload["branch"]))
            && is_str(raw.get("path"), destination)
    });
    if !confirmed {
        return Err(invalid(
            "native worktree creation was not confirmed by exact identity",
        ));
    }
    Ok(json!({
        "request_id": request_id, "status": "created", "path": absolute(raw.get("path"))?,
        "common_dir": absolute(raw.get("common_dir"))?,
        "branch": bounded(raw.get("branch"), 256, false)?,
    }))
}

/// `worktrees.contains(catalog, path)`: a current catalog lists the path as
/// an available checkout.
pub fn worktree_contains(catalog: &Map<String, Value>, path: &str) -> bool {
    if !is_str(catalog.get("state"), "ok")
        || catalog.get("stale").is_some_and(truthy)
        || !catalog.get("common_dir").is_some_and(truthy)
    {
        return false;
    }
    let selected = PurePath::new(path);
    catalog
        .get("worktrees")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .any(|row| {
            is_true(row.get("available"))
                && !is_str(row.get("kind"), "bare")
                && row
                    .get("path")
                    .and_then(Value::as_str)
                    .is_some_and(|candidate| PurePath::new(candidate) == selected)
        })
}

/// The first folder of the first project with this identity, as Python's
/// `next(...)` finds it.
fn folder<'a>(catalog: &'a Value, project: &Value, id: &Value) -> Option<&'a Value> {
    let project = catalog["projects"]
        .as_array()?
        .iter()
        .find(|row| py_eq(&row["id"], project))?;
    project["folders"]
        .as_array()?
        .iter()
        .find(|row| py_eq(&row["id"], id))
}

fn folder_mut<'a>(catalog: &'a mut Value, project: &Value, id: &Value) -> Option<&'a mut Value> {
    let project = catalog["projects"]
        .as_array_mut()?
        .iter_mut()
        .find(|row| py_eq(&row["id"], project))?;
    project["folders"]
        .as_array_mut()?
        .iter_mut()
        .find(|row| py_eq(&row["id"], id))
}

/// `launch.project_choice(catalog, payload, directory, worktree_catalog)`.
pub fn project_choice(
    catalog: &Value,
    payload: &Value,
    directory: &str,
    worktrees: Option<&Map<String, Value>>,
) -> PyResult<()> {
    if !py_eq(
        catalog.get("swarm_id").unwrap_or(&Value::Null),
        &payload["swarm_id"],
    ) {
        return Err(invalid("project catalog identity changed"));
    }
    let project = catalog["projects"]
        .as_array()
        .and_then(|rows| {
            rows.iter()
                .find(|row| py_eq(&row["id"], &payload["project_id"]))
        })
        .ok_or_else(|| invalid("selected native project is unavailable"))?;
    let folders = project["folders"].as_array().map_or(&[][..], Vec::as_slice);
    match payload.get("project_folder_id") {
        Some(folder) if !folder.is_null() => {
            let saved = folders
                .iter()
                .any(|row| py_eq(&row["id"], folder) && is_str(row.get("path"), directory));
            if !saved {
                return Err(invalid(FOLDER_CHANGED));
            }
        }
        _ if !truthy(&payload["add_folder"]) => {
            let anchor_id = payload.get("worktree_folder_id").unwrap_or(&Value::Null);
            let anchor = folders.iter().find(|row| py_eq(&row["id"], anchor_id));
            let related = match (anchor, worktrees) {
                (Some(anchor), Some(catalog)) => {
                    py_eq(
                        catalog.get("common_dir").unwrap_or(&Value::Null),
                        payload.get("worktree_common_dir").unwrap_or(&Value::Null),
                    ) && worktree_contains(catalog, directory)
                        && anchor["path"]
                            .as_str()
                            .is_some_and(|path| worktree_contains(catalog, path))
                }
                _ => false,
            };
            if !related {
                return Err(invalid(NOT_RELATED));
            }
        }
        _ => {}
    }
    Ok(())
}

/// An argument taken from native output. Python's subprocess refuses a
/// value that is not text with a TypeError, and text holding NUL with
/// ValueError("embedded null byte").
fn native_arg(value: &Value) -> PyResult<String> {
    match value {
        Value::String(text) if text.contains('\0') => Err(invalid("embedded null byte")),
        Value::String(text) => Ok(text.clone()),
        _ => Err(PyError::Type),
    }
}

/// `launch.validate` for `launch` and `directory_create`, and
/// `worktrees.validate` for `worktree_create`.
pub fn validate(operation: &str, payload: Option<&Value>, request_id: &str) -> PyResult<()> {
    if operation == "worktree_create" {
        return validate_worktree_create(payload, request_id);
    }
    let fields: &[&str] = match operation {
        "directory_create" => &["request_id", "path"],
        _ => &["request_id", "agent", "directory", "tag"],
    };
    // `effort` is additive and only OpenCode takes it; any other agent keeps
    // the unknown-field refusal.
    let opencode = payload
        .and_then(|payload| payload.get("agent"))
        .is_some_and(|agent| agent == "opencode");
    let extras: &[&str] = if operation == "launch" && opencode {
        &[
            "account_id",
            "model",
            "effort",
            "swarm_id",
            "project_id",
            "project_folder_id",
            "add_folder",
            "worktree_folder_id",
            "worktree_common_dir",
        ]
    } else if operation == "launch" {
        &[
            "account_id",
            "model",
            "swarm_id",
            "project_id",
            "project_folder_id",
            "add_folder",
            "worktree_folder_id",
            "worktree_common_dir",
        ]
    } else {
        &[]
    };
    let payload = payload
        .and_then(Value::as_object)
        .filter(|payload| {
            fields.iter().all(|field| payload.contains_key(*field))
                && payload
                    .keys()
                    .all(|key| fields.contains(&key.as_str()) || extras.contains(&key.as_str()))
                && is_str(payload.get("request_id"), request_id)
        })
        .ok_or_else(|| invalid(FIELDS))?;
    if operation == "directory_create" {
        // The request UUID was checked canonical before the claim.
        directory_destination(payload.get("path"))?;
        return Ok(());
    }
    validate_launch(payload)
}

fn validate_launch(payload: &Map<String, Value>) -> PyResult<()> {
    let has = |key: &str| payload.contains_key(key);
    let agent = payload["agent"]
        .as_str()
        .filter(|agent| PROVIDERS.contains(agent))
        .ok_or_else(|| invalid("unsupported native provider"))?;
    let directory = bounded(payload.get("directory"), 4096, false)?;
    if !directory.starts_with('/') {
        return Err(invalid("launch directory must be absolute"));
    }
    let tag = bounded(payload.get("tag"), 120, false)?;
    if tag.len() > 120 || py_strip(&tag) != tag || tag.contains(['/', '\\', ':', '.']) {
        return Err(invalid("invalid native session tag"));
    }
    if let Some(account) = payload.get("account_id") {
        if !account_identifier(&bounded(Some(account), 80, false)?) {
            return Err(invalid("invalid native account identifier"));
        }
    }
    if let Some(model) = payload.get("model") {
        let model = bounded(Some(model), 256, false)?;
        let (provider, identity) = model.split_once('/').unwrap_or((&model, ""));
        if agent != "opencode"
            || !is_str(payload.get("account_id"), "native-opencode")
            || model.len() > 256
            || !model.contains('/')
            || identity.is_empty()
            || !model_provider(provider)
            || model.chars().any(py_isspace)
        {
            return Err(invalid(
                "model selection requires the native OpenCode account and provider/model",
            ));
        }
    }
    if let Some(effort) = payload.get("effort") {
        // The native id shape; the fresh projection decides membership.
        if !payload.contains_key("model") || !effort.as_str().is_some_and(opencode_effort_id) {
            return Err(invalid("effort requires a model and a valid effort"));
        }
    }
    if has("worktree_folder_id") || has("worktree_common_dir") {
        let required = [
            "swarm_id",
            "project_id",
            "add_folder",
            "worktree_folder_id",
            "worktree_common_dir",
        ];
        if !required.iter().all(|key| has(key))
            || payload["add_folder"] != Value::Bool(false)
            || has("project_folder_id")
        {
            return Err(invalid(
                "worktree placement requires a distinct current project anchor",
            ));
        }
        identifier(
            payload.get("worktree_folder_id"),
            128,
            "invalid worktree anchor identifier",
        )?;
        absolute(payload.get("worktree_common_dir"))?;
    }
    let project = ["swarm_id", "project_id", "project_folder_id", "add_folder"];
    if project.iter().any(|key| has(key)) {
        if !["swarm_id", "project_id", "add_folder"]
            .iter()
            .all(|key| has(key))
            || !payload["add_folder"].is_boolean()
        {
            return Err(invalid(
                "project launch requires exact swarm/project and folder intent",
            ));
        }
        bounded(payload.get("swarm_id"), 128, false)?;
        identifier(payload.get("project_id"), 128, "invalid project identifier")?;
        if has("project_folder_id") {
            identifier(
                payload.get("project_folder_id"),
                128,
                "invalid project folder identifier",
            )?;
            if payload["add_folder"] == Value::Bool(true) {
                return Err(invalid("an existing folder cannot also be added"));
            }
        }
    }
    Ok(())
}

fn validate_worktree_create(payload: Option<&Value>, request_id: &str) -> PyResult<()> {
    let fields = [
        "request_id",
        "path",
        "common_dir",
        "destination",
        "branch",
        "base",
    ];
    let payload = payload
        .and_then(Value::as_object)
        .filter(|payload| {
            payload.len() == fields.len()
                && fields.iter().all(|field| payload.contains_key(*field))
                && is_str(payload.get("request_id"), request_id)
        })
        .ok_or_else(|| invalid(WORKTREE_FIELDS))?;
    for key in ["path", "common_dir", "destination"] {
        absolute(payload.get(key))?;
    }
    for key in ["branch", "base"] {
        let value = bounded(payload.get(key), 256, false)?;
        if py_strip(&value) != value || value.starts_with('-') {
            return Err(invalid("invalid worktree branch or revision"));
        }
    }
    Ok(())
}

/// The one session a listing shows under this launch UUID: its name, run
/// and conversation (empty when not yet known), or `None` while there is
/// not exactly one.
fn launched(listing: &Value, request_id: &str) -> PyResult<Option<(String, String, String)>> {
    let Some(listing) = listing.as_object() else {
        return Ok(None);
    };
    let sessions = match listing.get("sessions") {
        None => Vec::new(),
        Some(sessions) => py_iter(sessions)?,
    };
    let matches: Vec<&Map<String, Value>> = sessions
        .iter()
        .filter_map(Value::as_object)
        .filter(|row| is_str(row.get("launch_id"), request_id) && !is_archived(row))
        .collect();
    let [row] = matches.as_slice() else {
        return Ok(None);
    };
    let text = |key: &str| {
        row.get(key)
            .and_then(Value::as_str)
            .filter(|value| !value.is_empty())
    };
    let conversation = match row.get("conversation_id") {
        None | Some(Value::Null) => "",
        Some(Value::String(conversation)) => conversation.as_str(),
        Some(_) => return Ok(None),
    };
    Ok(match (text("name"), text("run_id")) {
        (Some(name), Some(run)) => Some((name.into(), run.into(), conversation.into())),
        _ => None,
    })
}

/// The creations of this module: mutations without a session target.
pub const OPERATIONS: &[&str] = &["directory_create", "launch", "worktree_create"];

pub fn handles(operation: &str) -> bool {
    OPERATIONS.contains(&operation)
}

impl<N: Native + 'static> Connector<N> {
    /// Runs one validated creation. `started` turns true once its native
    /// write may have begun.
    pub(crate) async fn create(
        &self,
        validated: &crate::validate::Validated,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let payload = validated.payload.as_ref().ok_or(Failure::Generic)?;
        match validated.operation.as_str() {
            "launch" => self.launch(payload, request_id, started).await,
            "directory_create" => self.directory_create(payload, request_id, started).await,
            _ => self.worktree_create(payload, request_id, started).await,
        }
    }

    /// `launch`: fresh account, model and folder checks (and the project
    /// choice), one fixed native launch carrying the request UUID, then the
    /// listing must show exactly that launch within 5 s. A project
    /// assignment follows a confirmed launch; its failure never repeats it.
    pub(crate) async fn launch(
        &self,
        payload: &Value,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let text = |key: &str| payload.get(key).and_then(Value::as_str).unwrap_or_default();
        let agent = text("agent");
        let raw = self
            .call(Call::new(["account", "ls"]).timeout(CATALOG_TIMEOUT))
            .await?;
        let accounts = launch_catalog(&raw).map_err(py)?;
        let rows = accounts["accounts"]
            .as_array()
            .map_or(&[][..], Vec::as_slice);
        let account = payload.get("account_id");
        let selected = |row: &&Value| {
            account.is_some_and(|account| py_eq(&row["id"], account))
                && is_str(row.get("provider"), agent)
        };
        let installed = accounts["agents"]
            .as_array()
            .is_some_and(|agents| agents.iter().any(|known| known.as_str() == Some(agent)));
        if !installed || account.is_some() && !rows.iter().any(|row| selected(&row)) {
            return Err(Failure::text(ACCOUNT_UNAVAILABLE));
        }
        if let Some(model) = payload.get("model") {
            let offered = rows.iter().find(selected).is_some_and(|row| {
                row.get("model_options")
                    .and_then(Value::as_array)
                    .is_some_and(|options| {
                        options.iter().any(|option| py_eq(&option["value"], model))
                    })
            });
            if !offered {
                return Err(Failure::text(MODEL_WITHDRAWN));
            }
            if let Some(effort) = payload.get("effort") {
                let efforts = rows.iter().find(selected).is_some_and(|row| {
                    row.get("model_options")
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                        .filter(|option| py_eq(&option["value"], model))
                        .filter_map(|option| option.get("effort_options")?.as_array())
                        .any(|efforts| efforts.iter().any(|known| py_eq(known, effort)))
                });
                if !efforts {
                    return Err(Failure::text(crate::mutations::EFFORT_NOT_OFFERED));
                }
            }
        }
        let directory = self
            .call(Call::new(["dirs", text("directory")]).timeout(CATALOG_TIMEOUT))
            .await?;
        let directory = match directory.get("path") {
            Some(Value::String(path)) if path.starts_with('/') => path.clone(),
            _ => return Err(Failure::text(DIRECTORY_UNAVAILABLE)),
        };
        if payload.get("project_id").is_some() {
            if !self.flags.get().project_launch {
                return Err(Failure::text(
                    "native scoped project assignment is unavailable",
                ));
            }
            self.project_preflight(payload, &directory).await?;
        }
        started.set(true);
        if directory.contains('\0') {
            // Python's subprocess raises ValueError, outside every handler.
            return Err(Failure::Generic);
        }
        let mut argv = vec![
            agent,
            directory.as_str(),
            "--new",
            "-n",
            text("tag"),
            "-d",
            "--launch-id",
            request_id,
        ];
        if payload.get("account_id").is_some() {
            argv.extend(["--account", text("account_id")]);
        }
        if payload.get("model").is_some() {
            argv.extend(["--model", text("model")]);
        }
        if payload.get("effort").is_some() {
            argv.extend(["--effort", text("effort")]);
        }
        self.native(Call::new(argv).timeout(LAUNCH_TIMEOUT).text())
            .await?;
        let until = self.clock() + VERIFY_SECONDS;
        let mut target = None;
        while self.clock() < until {
            let listing = self
                .call(Call::new(["ls", "--json", "--local"]).timeout(LISTING_TIMEOUT))
                .await?;
            target = launched(&listing, request_id).map_err(py)?;
            if target.is_some() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        let Some((name, run, mut conversation)) = target else {
            return Err(Failure::text(UNVERIFIED));
        };
        let mut result = json!({"request_id": request_id, "status": "created"});
        if payload.get("project_id").is_some() {
            let status = self
                .assign(
                    payload,
                    request_id,
                    &directory,
                    (&name, &run),
                    &mut conversation,
                )
                .await;
            result["project_assignment"] = json!({"status": status, "swarm_id": payload["swarm_id"],
                                                  "project_id": payload["project_id"]});
            if status != "assigned" {
                result["warning"] = json!(format!(
                    "The session was created, but its project assignment is {status}. Review Projects; this launch will not be repeated."
                ));
            }
        }
        result["result_target"] =
            json!({"name": name, "run_id": run, "conversation_id": conversation});
        Ok(result)
    }

    /// The project catalog and folder checks before a project launch; the
    /// checked folder paths replace the catalog's, as in Python.
    async fn project_preflight(&self, payload: &Value, directory: &str) -> Result<(), Failure> {
        let raw = self
            .call(Call::new(["swarm", "get"]).timeout(SWARM_TIMEOUT))
            .await?;
        let mut catalog = launch_projects(&raw).map_err(py)?;
        let project = &payload["project_id"];
        if let Some(folder_id) = payload.get("project_folder_id") {
            let path = folder(&catalog, project, folder_id)
                .map(|row| row["path"].clone())
                .ok_or_else(|| Failure::text(FOLDER_CHANGED))?;
            let argv = ["dirs".to_owned(), native_arg(&path).map_err(py)?];
            let checked = self.call(Call::new(argv).timeout(CATALOG_TIMEOUT)).await?;
            if !checked.is_object() || !is_str(checked.get("path"), directory) {
                return Err(Failure::text(FOLDER_CHANGED));
            }
            if let Some(row) = folder_mut(&mut catalog, project, folder_id) {
                row["path"] = checked["path"].clone();
            }
        }
        let mut worktrees = None;
        if let Some(anchor_id) = payload.get("worktree_folder_id") {
            if !self.flags.get().worktree {
                return Err(Failure::text(
                    "native verified worktree placement is unavailable",
                ));
            }
            let path = folder(&catalog, project, anchor_id)
                .map(|row| row["path"].clone())
                .ok_or_else(|| Failure::text("selected local project anchor changed"))?;
            let argv = ["dirs".to_owned(), native_arg(&path).map_err(py)?];
            let checked = self.call(Call::new(argv).timeout(CATALOG_TIMEOUT)).await?;
            // `checked.get('path')` of a value that is not a dict is an
            // AttributeError outside Python's handler.
            let checked = checked.as_object().ok_or(Failure::Generic)?;
            let anchor = absolute(checked.get("path")).map_err(py)?;
            if let Some(row) = folder_mut(&mut catalog, project, anchor_id) {
                row["path"] = json!(anchor);
            }
            let argv = [
                "worktrees",
                "--path",
                anchor.as_str(),
                "--refresh",
                "--json",
            ];
            let raw = self.call(Call::new(argv).timeout(CATALOG_TIMEOUT)).await?;
            worktrees = Some(worktree_catalog(&raw, &anchor).map_err(py)?);
        }
        project_choice(&catalog, payload, directory, worktrees.as_ref()).map_err(py)
    }

    /// `swarm assign-launch` for a confirmed launch: "assigned", "failed" or
    /// "uncertain". Only an exact acknowledgement counts; any failure, a
    /// stop included, leaves the assignment uncertain.
    async fn assign(
        &self,
        payload: &Value,
        request_id: &str,
        directory: &str,
        (name, run): (&str, &str),
        conversation: &mut String,
    ) -> &'static str {
        let mut stdin = json!({
            "request_id": request_id, "name": name, "expected_run_id": run,
            "expected_conversation_id": conversation.as_str(), "swarm_id": payload["swarm_id"],
            "project_id": payload["project_id"], "directory": directory,
            "add_folder": payload["add_folder"],
        });
        for field in [
            "worktree_folder_id",
            "worktree_common_dir",
            "project_folder_id",
        ] {
            if let Some(value) = payload.get(field) {
                stdin[field] = value.clone();
            }
        }
        let call = Call::new(["swarm", "assign-launch", "--json"])
            .payload(stdin)
            .timeout(CATALOG_TIMEOUT);
        let Ok(raw) = self.call(call).await else {
            return "uncertain";
        };
        let Some(raw) = raw.as_object() else {
            return "uncertain";
        };
        let same_conversation = is_str(raw.get("conversation_id"), conversation)
            || conversation.is_empty() && raw.get("conversation_id").is_some_and(Value::is_string);
        let status = ["assigned", "failed", "uncertain"]
            .into_iter()
            .find(|status| is_str(raw.get("status"), status));
        let acknowledged = is_str(raw.get("request_id"), request_id)
            && is_str(raw.get("name"), name)
            && is_str(raw.get("run_id"), run)
            && same_conversation
            && raw
                .get("swarm_id")
                .is_some_and(|value| py_eq(value, &payload["swarm_id"]))
            && raw
                .get("project_id")
                .is_some_and(|value| py_eq(value, &payload["project_id"]));
        let (true, Some(status)) = (acknowledged, status) else {
            return "uncertain";
        };
        if status == "assigned" {
            // The next heartbeat reads the projects again at once.
            self.state.borrow_mut().project_next_poll = 0.0;
            if conversation.is_empty() {
                *conversation = raw["conversation_id"].as_str().unwrap_or_default().into();
            }
        }
        status
    }

    /// `directory_create`: the parent is confirmed natively, then one
    /// literal `dirs --create` with the request UUID runs, and only its
    /// exact receipt completes the request.
    pub(crate) async fn directory_create(
        &self,
        payload: &Value,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let path = payload["path"].as_str().unwrap_or_default();
        let parent = PurePath::new(path).parent().to_string();
        let catalog = self
            .call(Call::new(["dirs", parent.as_str()]).timeout(CATALOG_TIMEOUT))
            .await?;
        if !catalog.is_object() {
            return Err(Failure::text("native directory parent catalog is invalid"));
        }
        let parent = canonical_directory(catalog.get("path")).map_err(py)?;
        started.set(true);
        let argv = ["dirs", "--create", path, "--request-id", request_id];
        let raw = self.call(Call::new(argv).timeout(CATALOG_TIMEOUT)).await?;
        directory_created(&raw, path, request_id, &parent).map_err(py)
    }

    /// `worktree_create`: the destination's parent and the repository are
    /// read fresh, then one bounded `worktrees create --mobile` runs; its
    /// supervisor is never cut short. Python reports a refused projection
    /// or receipt through its generic text: those reasons are ValueErrors
    /// outside its handlers.
    pub(crate) async fn worktree_create(
        &self,
        payload: &Value,
        request_id: &str,
        started: &Cell<bool>,
    ) -> Result<Value, Failure> {
        let text = |key: &str| payload[key].as_str().unwrap_or_default();
        let destination = PurePath::new(text("destination"));
        let name = destination.name();
        if matches!(name, "" | "." | "..") {
            return Err(Failure::text("invalid worktree destination"));
        }
        let parent = destination.parent().to_string();
        let parent = self
            .call(Call::new(["dirs", parent.as_str()]).timeout(CATALOG_TIMEOUT))
            .await?;
        let parent = parent.as_object().ok_or(Failure::Generic)?;
        let parent = absolute(parent.get("path")).map_err(|_| Failure::Generic)?;
        let confirmed = PurePath::new(&parent).join(name).to_string();
        let path = text("path");
        let argv = ["worktrees", "--path", path, "--refresh", "--json"];
        let raw = self.call(Call::new(argv).timeout(CATALOG_TIMEOUT)).await?;
        let fresh = worktree_catalog(&raw, path).map_err(|_| Failure::Generic)?;
        let current = is_str(fresh.get("state"), "ok")
            && !fresh.get("stale").is_some_and(truthy)
            && fresh
                .get("common_dir")
                .is_some_and(|value| py_eq(value, &payload["common_dir"]));
        if !current {
            return Err(Failure::text(REPOSITORY_CHANGED));
        }
        started.set(true);
        let argv = [
            "worktrees",
            "create",
            "--path",
            path,
            "--common-dir",
            text("common_dir"),
            "--branch",
            text("branch"),
            "--base",
            text("base"),
            "--destination",
            text("destination"),
            "--request-id",
            request_id,
            "--json",
            "--mobile",
        ];
        let raw = self
            .call(Call::new(argv).timeout(WORKTREE_CREATE_TIMEOUT))
            .await?;
        worktree_created(&raw, payload, request_id, &confirmed).map_err(|_| Failure::Generic)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pure_paths_follow_pathlib() {
        for (path, text, name, parent) in [
            ("/alias/new", "/alias/new", "new", "/alias"),
            ("/alias/new/", "/alias/new", "new", "/alias"),
            ("/alias/.", "/alias", "alias", "/"),
            ("/alias/..", "/alias/..", "..", "/alias"),
            ("//host/x", "//host/x", "x", "//host"),
            ("///x//y", "/x/y", "y", "/x"),
            ("/", "/", "", "/"),
            ("//", "//", "", "//"),
            ("a/./b", "a/b", "b", "a"),
            ("", ".", "", "."),
        ] {
            let pure = PurePath::new(path);
            assert_eq!(pure.to_string(), text, "{path}");
            assert_eq!(pure.name(), name, "{path}");
            assert_eq!(pure.parent().to_string(), parent, "{path}");
        }
        assert_eq!(PurePath::new("/").join("new").to_string(), "/new");
        assert_eq!(PurePath::new("//").join("new").to_string(), "//new");
        assert_eq!(PurePath::new("/a").join("b c").to_string(), "/a/b c");
    }

    #[test]
    fn directories_are_canonical_absolute_children() {
        let path = |text: &str| directory_destination(Some(&json!(text)));
        assert!(path("/a/new project;$(literal)").is_ok());
        for refused in ["relative", "/a/", "/a/.", "/a/..", "/"] {
            assert_eq!(
                path(refused),
                Err(invalid("new directory must be an absolute child path")),
                "{refused}"
            );
        }
        let long = format!("/{}", "\u{e9}".repeat(2048));
        assert!(path(&long).is_err(), "4,097 UTF-8 bytes");
        let canonical = |text: &str| canonical_directory(Some(&json!(text)));
        assert!(canonical("/").is_ok());
        for refused in ["//a", "/a/../b", "/a/", "/a//b", "/a/./b", "a"] {
            assert_eq!(canonical(refused), Err(invalid(NOT_CANONICAL)), "{refused}");
        }
        assert_eq!(
            canonical_directory(None),
            Err(invalid("invalid bounded native identity"))
        );
        // Created under the root, the folder's path has one slash.
        let raw = json!({"status": "created", "request_id": "r", "requested_path": "/x/new",
                         "parent": "/", "path": "/new"});
        assert_eq!(
            directory_created(&raw, "/x/new", "r", "/").unwrap()["path"],
            "/new"
        );
    }

    #[test]
    fn identifiers_and_models_follow_the_python_patterns() {
        assert!(account_identifier("native-codex_1"));
        assert!(!account_identifier("-native"));
        assert!(!account_identifier("n\u{e9}"));
        assert!(!account_identifier(""));
        assert!(model_provider("ollama"));
        assert!(model_provider(&"a".repeat(80)));
        assert!(!model_provider(&"a".repeat(81)));
        assert!(!model_provider(".hidden"));
        let launch = |changes: Value| {
            let mut payload = json!({"request_id": "r", "agent": "opencode", "directory": "/w",
                                     "tag": "t", "account_id": "native-opencode"});
            for (key, value) in changes.as_object().unwrap() {
                payload[key] = value.clone();
            }
            validate("launch", Some(&payload), "r")
        };
        assert!(launch(json!({"model": "ollama/qwen3-coder:30b"})).is_ok());
        for model in ["ollama", "ollama/", "/model", "a b/c", "ollama/x\u{2003}y"] {
            assert!(launch(json!({"model": model})).is_err(), "{model}");
        }
        assert!(launch(json!({"model": "a/b", "account_id": "other"})).is_err());
        // Identifiers are bounded by code points and by UTF-8 bytes.
        let project =
            |id: &str| launch(json!({"swarm_id": "s", "project_id": id, "add_folder": true}));
        assert!(project(&"p".repeat(128)).is_ok());
        assert_eq!(
            project(&"\u{e9}".repeat(65)),
            Err(invalid("invalid project identifier"))
        );
        assert_eq!(
            project(" padded"),
            Err(invalid("invalid project identifier"))
        );
        assert_eq!(
            launch(json!({"swarm_id": "s", "project_id": "p", "add_folder": 1})),
            Err(invalid(
                "project launch requires exact swarm/project and folder intent"
            ))
        );
        assert_eq!(
            launch(
                json!({"swarm_id": "s", "project_id": "p", "add_folder": false,
                          "worktree_folder_id": "\u{3000}a", "worktree_common_dir": "/r/.git"})
            ),
            Err(invalid("invalid worktree anchor identifier"))
        );
        assert_eq!(
            launch(json!({"swarm_id": "s", "project_id": "p", "add_folder": 0,
                          "worktree_folder_id": "a", "worktree_common_dir": "/r/.git"})),
            Err(invalid(
                "worktree placement requires a distinct current project anchor"
            ))
        );
    }

    fn catalog(state: &str, stale: bool, rows: Value) -> Map<String, Value> {
        json!({"state": state, "stale": stale, "common_dir": "/r/.git", "worktrees": rows})
            .as_object()
            .unwrap()
            .clone()
    }

    #[test]
    fn only_a_current_catalog_relates_a_checkout() {
        let rows = json!([
            {"path": "/r", "kind": "main", "available": true},
            {"path": "/r-linked", "kind": "linked", "available": true},
            {"path": "/r-gone", "kind": "linked", "available": false},
            {"path": "/r-bare", "kind": "bare", "available": true}]);
        let current = catalog("ok", false, rows.clone());
        assert!(worktree_contains(&current, "/r"));
        assert!(worktree_contains(&current, "/r-linked/"));
        assert!(!worktree_contains(&current, "/r-gone"));
        assert!(!worktree_contains(&current, "/r-bare"));
        // A nested repository or an ordinary subfolder is not a member.
        assert!(!worktree_contains(&current, "/r/nested"));
        assert!(!worktree_contains(&catalog("ok", true, rows.clone()), "/r"));
        assert!(!worktree_contains(&catalog("partial", false, rows), "/r"));
    }

    #[test]
    fn a_project_launch_needs_a_saved_related_or_added_folder() {
        let projects = json!({"swarm_id": "s", "projects": [{"id": "p", "folders": [
            {"id": "saved", "path": "/example/saved"}, {"id": "anchor", "path": "/r"}]}]});
        let payload = |changes: Value| {
            let mut payload = json!({"swarm_id": "s", "project_id": "p", "add_folder": false});
            for (key, value) in changes.as_object().unwrap() {
                payload[key] = value.clone();
            }
            payload
        };
        let choose = |changes: Value, directory: &str, worktrees: Option<&Map<String, Value>>| {
            project_choice(&projects, &payload(changes), directory, worktrees)
        };
        assert!(choose(json!({"add_folder": true}), "/elsewhere", None).is_ok());
        assert!(choose(
            json!({"project_folder_id": "saved"}),
            "/example/saved",
            None
        )
        .is_ok());
        assert_eq!(
            choose(
                json!({"project_folder_id": "saved"}),
                "/example/other",
                None
            ),
            Err(invalid(FOLDER_CHANGED))
        );
        assert_eq!(
            choose(json!({"swarm_id": "t"}), "/example", None),
            Err(invalid("project catalog identity changed"))
        );
        assert_eq!(
            choose(json!({"project_id": "q"}), "/example", None),
            Err(invalid("selected native project is unavailable"))
        );
        let rows = json!([{"path": "/r", "kind": "main", "available": true},
                          {"path": "/r-linked", "kind": "linked", "available": true}]);
        let related = json!({"worktree_folder_id": "anchor", "worktree_common_dir": "/r/.git"});
        let current = catalog("ok", false, rows.clone());
        assert!(choose(related.clone(), "/r-linked", Some(&current)).is_ok());
        for (directory, worktrees) in [
            ("/r/nested", catalog("ok", false, rows.clone())),
            ("/r-linked", catalog("ok", true, rows)),
        ] {
            assert_eq!(
                choose(related.clone(), directory, Some(&worktrees)),
                Err(invalid(NOT_RELATED)),
                "{directory}"
            );
        }
        let other = json!({"worktree_folder_id": "anchor", "worktree_common_dir": "/o/.git"});
        assert_eq!(
            choose(other, "/r-linked", Some(&current)),
            Err(invalid(NOT_RELATED))
        );
        assert_eq!(
            choose(json!({}), "/r-linked", None),
            Err(invalid(NOT_RELATED))
        );
    }

    #[test]
    fn only_one_unarchived_listing_row_confirms_a_launch() {
        let row = |launch: &str, changes: Value| {
            let mut row = json!({"name": "codex/x", "run_id": "run", "launch_id": launch});
            for (key, value) in changes.as_object().unwrap() {
                row[key] = value.clone();
            }
            row
        };
        let listing = |rows: Vec<Value>| json!({"sessions": rows});
        assert_eq!(
            launched(&listing(vec![row("L", json!({}))]), "L").unwrap(),
            Some(("codex/x".into(), "run".into(), "".into()))
        );
        assert_eq!(
            launched(
                &listing(vec![row("L", json!({"conversation_id": "c"}))]),
                "L"
            )
            .unwrap(),
            Some(("codex/x".into(), "run".into(), "c".into()))
        );
        for rows in [
            vec![row("L", json!({})), row("L", json!({"name": "codex/y"}))],
            vec![row("other", json!({}))],
            vec![row("L", json!({"state": "archived"}))],
            vec![row("L", json!({"run_id": ""}))],
            vec![row("L", json!({"conversation_id": 5}))],
        ] {
            assert_eq!(
                launched(&listing(rows.clone()), "L").unwrap(),
                None,
                "{rows:?}"
            );
        }
        assert_eq!(launched(&json!({}), "L").unwrap(), None);
        assert_eq!(launched(&json!([]), "L").unwrap(), None);
        assert_eq!(launched(&json!({"sessions": "LL"}), "L").unwrap(), None);
        assert_eq!(launched(&json!({"sessions": 1}), "L"), Err(PyError::Type));
        assert_eq!(
            launched(&json!({"sessions": null}), "L"),
            Err(PyError::Type)
        );
    }

    #[test]
    fn native_arguments_from_native_output_follow_subprocess() {
        assert_eq!(native_arg(&json!("/w")), Ok("/w".into()));
        assert_eq!(
            native_arg(&json!("/w\u{0}x")),
            Err(invalid("embedded null byte"))
        );
        assert_eq!(native_arg(&json!(5)), Err(PyError::Type));
        assert_eq!(native_arg(&Value::Null), Err(PyError::Type));
    }
}
