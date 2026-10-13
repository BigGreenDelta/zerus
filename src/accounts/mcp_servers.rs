//! Managed profiles keep their own provider login but use the user's MCP servers.
//! Each launch merges the native user-level definitions into the profile. The
//! baseline keeps only digests of what was last inherited, so a server that the
//! profile added, changed or removed itself keeps its local state.
use super::*;
use std::io::Read;

const BASELINE: &str = ".hgs-inherited-mcp.json";
const LOCK: &str = ".hgs-inherited-mcp.lock";
const MAX_CONFIG: u64 = 16 * 1024 * 1024;

#[derive(Clone, Copy, PartialEq)]
enum Format {
    Json,
    Toml,
}

struct Layout {
    native: &'static str,
    profile: &'static str,
    format: Format,
    key: &'static str,
}

fn layout(provider: &str) -> Option<Layout> {
    match provider {
        "claude" => Some(Layout { native: ".claude.json", profile: ".claude.json", format: Format::Json, key: "mcpServers" }),
        "codex" => Some(Layout { native: ".codex/config.toml", profile: "config.toml", format: Format::Toml, key: "mcp_servers" }),
        "kimi" => Some(Layout { native: ".kimi-code/mcp.json", profile: "mcp.json", format: Format::Json, key: "mcpServers" }),
        _ => None,
    }
}

enum Document {
    Json(Value),
    Toml(toml_edit::DocumentMut),
}

fn read_text(path: &Path) -> Result<Option<String>> {
    let file = match fs::File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e.into()),
    };
    let mut text = String::new();
    file.take(MAX_CONFIG + 1).read_to_string(&mut text)
        .map_err(|_| Error::new(1, format!("Unreadable agent configuration: {}", path.display())))?;
    if text.len() as u64 > MAX_CONFIG {
        return Err(Error::new(1, format!("Agent configuration is too large: {}", path.display())));
    }
    Ok(Some(text))
}

fn parse(layout: &Layout, text: Option<&str>, path: &Path) -> Result<Document> {
    let invalid = || Error::new(1, format!("Invalid agent configuration: {}", path.display()));
    Ok(match layout.format {
        Format::Json => {
            let value: Value = match text { Some(text) => serde_json::from_str(text).map_err(|_| invalid())?, None => json!({}) };
            if !value.is_object() || !value.get(layout.key).is_none_or(Value::is_object) { return Err(invalid()); }
            Document::Json(value)
        }
        Format::Toml => {
            let document: toml_edit::DocumentMut = text.unwrap_or("").parse().map_err(|_| invalid())?;
            if document.get(layout.key).is_some_and(|item| item.as_table_like().is_none()) { return Err(invalid()); }
            Document::Toml(document)
        }
    })
}

fn toml_value(value: &toml_edit::Value) -> Value {
    match value {
        toml_edit::Value::String(v) => json!(v.value()),
        toml_edit::Value::Integer(v) => json!(v.value()),
        toml_edit::Value::Float(v) => json!(v.value()),
        toml_edit::Value::Boolean(v) => json!(v.value()),
        toml_edit::Value::Datetime(v) => json!(v.value().to_string()),
        toml_edit::Value::Array(v) => Value::Array(v.iter().map(toml_value).collect()),
        toml_edit::Value::InlineTable(v) => Value::Object(v.iter().map(|(k, v)| (k.to_owned(), toml_value(v))).collect()),
    }
}

// Formatting and comments are not part of a server's definition.
fn toml_item(item: &toml_edit::Item) -> Value {
    match item {
        toml_edit::Item::None => Value::Null,
        toml_edit::Item::Value(v) => toml_value(v),
        toml_edit::Item::Table(t) => Value::Object(t.iter().map(|(k, v)| (k.to_owned(), toml_item(v))).collect()),
        toml_edit::Item::ArrayOfTables(a) => Value::Array(a.iter()
            .map(|t| Value::Object(t.iter().map(|(k, v)| (k.to_owned(), toml_item(v))).collect())).collect()),
    }
}

// Digests let the baseline record what was inherited without storing headers or tokens.
fn servers(document: &Document, key: &str) -> BTreeMap<String, String> {
    let digest = |value: Value| format!("{:x}", Sha256::digest(value.to_string()));
    match document {
        Document::Json(root) => root.get(key).and_then(Value::as_object).into_iter().flatten()
            .map(|(name, value)| (name.clone(), digest(value.clone()))).collect(),
        Document::Toml(root) => root.get(key).and_then(toml_edit::Item::as_table_like).into_iter()
            .flat_map(|table| table.iter()).map(|(name, item)| (name.to_owned(), digest(toml_item(item)))).collect(),
    }
}

// Native provisioning comments (for example managed-block markers) describe the
// native file only. Positions restart so a server's subtables stay together.
fn detach(item: &mut toml_edit::Item) {
    match item {
        toml_edit::Item::Table(table) => {
            table.decor_mut().clear();
            table.set_position(None);
            for (_, child) in table.iter_mut() { detach(child); }
        }
        toml_edit::Item::ArrayOfTables(tables) => {
            for table in tables.iter_mut() {
                table.decor_mut().clear();
                table.set_position(None);
                for (_, child) in table.iter_mut() { detach(child); }
            }
        }
        toml_edit::Item::Value(value) => { value.decor_mut().clear(); }
        toml_edit::Item::None => {}
    }
}

fn apply(document: &mut Document, native: &Document, key: &str, name: &str) -> Result<()> {
    match (document, native) {
        (Document::Json(root), Document::Json(source)) => {
            let servers = root.as_object_mut().unwrap().entry(key).or_insert_with(|| json!({}));
            let servers = servers.as_object_mut().unwrap();
            match source.get(key).and_then(|s| s.get(name)) {
                Some(server) => { servers.insert(name.to_owned(), server.clone()); }
                None => { servers.remove(name); }
            }
        }
        (Document::Toml(root), Document::Toml(source)) => {
            let server = source.get(key).and_then(toml_edit::Item::as_table_like).and_then(|t| t.get(name)).cloned();
            if !root.contains_key(key) {
                if server.is_none() { return Ok(()); }
                let mut table = toml_edit::Table::new();
                table.set_implicit(true);
                root.insert(key, toml_edit::Item::Table(table));
            }
            let servers = root[key].as_table_like_mut().ok_or_else(|| Error::new(1, "Invalid MCP server table."))?;
            match server {
                Some(mut server) => { detach(&mut server); servers.insert(name, server); }
                None => { servers.remove(name); }
            }
        }
        _ => unreachable!("documents of one layout share a format"),
    }
    Ok(())
}

fn render(document: &Document) -> Result<String> {
    Ok(match document {
        Document::Json(value) => serde_json::to_string_pretty(value).map_err(|e| Error::new(1, e.to_string()))? + "\n",
        Document::Toml(document) => document.to_string(),
    })
}

// Agents rewrite their own configuration, possibly from another session of this
// profile. Never replace a version this merge did not read.
fn replace(path: &Path, before: Option<&str>, after: &str) -> Result<bool> {
    let mut file = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
    platform::make_private(file.as_file())?;
    file.write_all(after.as_bytes())?;
    file.as_file().sync_all()?;
    if read_text(path)?.as_deref() != before { return Ok(false); }
    platform::persist(file, path).map_err(|e| Error::new(1, e.to_string()))?;
    Ok(true)
}

fn read_baseline(path: &Path) -> Result<BTreeMap<String, String>> {
    let Some(text) = read_text(path)? else { return Ok(BTreeMap::new()) };
    let value: Value = serde_json::from_str(&text).map_err(|_| Error::new(1, "Invalid MCP inheritance baseline."))?;
    if value["version"] != 1 { return Err(Error::new(1, "Unsupported MCP inheritance baseline version.")); }
    serde_json::from_value(value["servers"].clone()).map_err(|_| Error::new(1, "Invalid MCP inheritance baseline."))
}

pub(super) fn inherit(provider: &str, home: &Path, profile: &Path) -> Result<()> {
    let Some(layout) = layout(provider) else { return Ok(()) };
    let source = home.join(layout.native);
    let target = profile.join(layout.profile);
    if source == target { return Ok(()); }
    if fs::symlink_metadata(&target).is_ok_and(|m| m.file_type().is_symlink()) {
        return Err(Error::new(1, "Managed agent configuration is a symlink; MCP servers were not inherited."));
    }
    let guard = platform::private_open(fs::OpenOptions::new().create(true).truncate(false).read(true).write(true)).open(profile.join(LOCK))?;
    let _guard = platform::FileLock::exclusive(guard)?;
    let native = parse(&layout, read_text(&source)?.as_deref(), &source)?;
    let inherited = servers(&native, layout.key);
    let baseline = profile.join(BASELINE);
    let previous = read_baseline(&baseline)?;
    let mut written = false;
    for _ in 0..3 {
        let before = read_text(&target)?;
        let mut document = parse(&layout, before.as_deref(), &target)?;
        let current = servers(&document, layout.key);
        let names: BTreeSet<&String> = previous.keys().chain(inherited.keys()).chain(current.keys()).collect();
        let mut changed = false;
        for name in names {
            // A profile value that differs from the last inherited one is a profile override.
            if current.get(name) != previous.get(name) || current.get(name) == inherited.get(name) { continue; }
            apply(&mut document, &native, layout.key, name)?;
            changed = true;
        }
        if !changed || replace(&target, before.as_deref(), &render(&document)?)? { written = true; break; }
    }
    if !written {
        return Err(Error::new(1, "Agent configuration kept changing; MCP servers were not inherited."));
    }
    let next = json!({"version": 1, "provider": provider, "servers": inherited});
    if read_text(&baseline)?.and_then(|text| serde_json::from_str::<Value>(&text).ok()) != Some(next.clone()) {
        replace(&baseline, read_text(&baseline)?.as_deref(), &(next.to_string() + "\n"))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn dirs() -> (tempfile::TempDir, PathBuf, PathBuf) {
        let tmp = tempfile::tempdir().unwrap();
        let (home, profile) = (tmp.path().join("home"), tmp.path().join("profile"));
        fs::create_dir_all(home.join(".codex")).unwrap();
        fs::create_dir_all(home.join(".kimi-code")).unwrap();
        fs::create_dir_all(&profile).unwrap();
        (tmp, home, profile)
    }
    fn json_file(path: &Path) -> Value { serde_json::from_str(&fs::read_to_string(path).unwrap()).unwrap() }
    const SECRET: &str = "Bearer fixture-token";

    #[test]
    fn claude_profile_gains_user_servers_and_keeps_its_own_state() {
        let (_tmp, home, profile) = dirs();
        let memory = json!({"type": "http", "url": "https://memory.example.com/mcp", "headers": {"Authorization": SECRET}});
        let docs = json!({"type": "stdio", "command": "docs-mcp"});
        fs::write(home.join(".claude.json"), json!({"mcpServers": {"memory": memory, "docs": docs}, "oauthAccount": {"email": "native"}}).to_string()).unwrap();
        fs::write(profile.join(".claude.json"), json!({"oauthAccount": {"email": "managed"}, "mcpServers": {"local": {"command": "own"}}}).to_string()).unwrap();
        inherit("claude", &home, &profile).unwrap();
        let first = json_file(&profile.join(".claude.json"));
        assert_eq!(first["mcpServers"], json!({"memory": memory, "docs": docs, "local": {"command": "own"}}));
        assert_eq!(first["oauthAccount"]["email"], "managed");
        let baseline = fs::read_to_string(profile.join(BASELINE)).unwrap();
        assert!(!baseline.contains("fixture-token") && !baseline.contains("memory.example.com"));
        assert_eq!(fs::metadata(profile.join(".claude.json")).unwrap().permissions().mode() & 0o777, 0o600);

        // The profile changes one server and removes another; the user then edits both.
        let mut local = first.clone();
        local["mcpServers"]["docs"] = json!({"type": "stdio", "command": "profile-docs"});
        local["mcpServers"].as_object_mut().unwrap().remove("memory");
        fs::write(profile.join(".claude.json"), local.to_string()).unwrap();
        let rotated = json!({"type": "http", "url": "https://memory.example.com/mcp", "headers": {"Authorization": "Bearer rotated"}});
        fs::write(home.join(".claude.json"), json!({"mcpServers": {"memory": rotated, "docs": {"command": "native-docs"}, "new": {"command": "new"}}}).to_string()).unwrap();
        inherit("claude", &home, &profile).unwrap();
        let next = json_file(&profile.join(".claude.json"));
        assert_eq!(next["mcpServers"], json!({"docs": {"type": "stdio", "command": "profile-docs"}, "new": {"command": "new"}, "local": {"command": "own"}}));

        // A server the user removes disappears where the profile still had the inherited copy.
        fs::write(home.join(".claude.json"), json!({"mcpServers": {"docs": {"command": "native-docs"}}}).to_string()).unwrap();
        inherit("claude", &home, &profile).unwrap();
        let bytes = fs::read(profile.join(".claude.json")).unwrap();
        assert_eq!(json_file(&profile.join(".claude.json"))["mcpServers"], json!({"docs": {"type": "stdio", "command": "profile-docs"}, "local": {"command": "own"}}));
        inherit("claude", &home, &profile).unwrap();
        assert_eq!(fs::read(profile.join(".claude.json")).unwrap(), bytes);
    }

    #[test]
    fn codex_profile_inherits_server_tables_without_native_settings_or_markers() {
        let (_tmp, home, profile) = dirs();
        fs::write(home.join(".codex/config.toml"), format!(r#"model = "native-model"

# BEGIN MANAGED MEMORY
[mcp_servers.memory]
url = "https://memory.example.com/mcp"
startup_timeout_sec = 30
[mcp_servers.memory.http_headers]
Authorization = "{SECRET}"
[mcp_servers.memory.tools.search]
approval_mode = "approve"
# END MANAGED MEMORY

[projects."/native"]
trust_level = "trusted"
"#)).unwrap();
        fs::write(profile.join("config.toml"), "cli_auth_credentials_store = \"file\"\n\n[projects.\"/work\"]\ntrust_level = \"trusted\"\n").unwrap();
        inherit("codex", &home, &profile).unwrap();
        let text = fs::read_to_string(profile.join("config.toml")).unwrap();
        let document: toml_edit::DocumentMut = text.parse().unwrap();
        assert_eq!(document["cli_auth_credentials_store"].as_str(), Some("file"));
        assert!(document.get("model").is_none() && document["projects"].get("/native").is_none());
        assert_eq!(toml_item(&document["mcp_servers"]["memory"]), json!({"url": "https://memory.example.com/mcp",
            "startup_timeout_sec": 30, "http_headers": {"Authorization": SECRET}, "tools": {"search": {"approval_mode": "approve"}}}));
        assert!(!text.contains("MANAGED MEMORY"), "{text}");
        let bytes = fs::read(profile.join("config.toml")).unwrap();
        inherit("codex", &home, &profile).unwrap();
        assert_eq!(fs::read(profile.join("config.toml")).unwrap(), bytes);

        fs::write(home.join(".codex/config.toml"), "model = \"native-model\"\n").unwrap();
        inherit("codex", &home, &profile).unwrap();
        let document: toml_edit::DocumentMut = fs::read_to_string(profile.join("config.toml")).unwrap().parse().unwrap();
        assert!(document.get("mcp_servers").is_none_or(|t| t.as_table_like().unwrap().is_empty()));
        assert_eq!(document["projects"]["/work"]["trust_level"].as_str(), Some("trusted"));
    }

    #[test]
    fn kimi_profile_creates_its_server_file_and_invalid_sources_change_nothing() {
        let (_tmp, home, profile) = dirs();
        let memory = json!({"url": "https://memory.example.com/mcp", "headers": {"Authorization": SECRET}});
        fs::write(home.join(".kimi-code/mcp.json"), json!({"mcpServers": {"memory": memory}}).to_string()).unwrap();
        inherit("kimi", &home, &profile).unwrap();
        assert_eq!(json_file(&profile.join("mcp.json")), json!({"mcpServers": {"memory": memory}}));

        fs::write(home.join(".kimi-code/mcp.json"), "{invalid").unwrap();
        let before = fs::read(profile.join("mcp.json")).unwrap();
        assert!(inherit("kimi", &home, &profile).is_err());
        assert_eq!(fs::read(profile.join("mcp.json")).unwrap(), before);
        assert!(inherit("dsh", &home, &profile).is_ok());
    }
}
