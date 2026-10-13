//! Managed Claude accounts keep credentials separate while inheriting the user's
//! launch/UI preferences. A three-way merge preserves later profile overrides.
use super::*;
use std::io::Read;

const KEYS: &[&str] = &[
    "permissions", "model", "effortLevel", "modelSettings", "alwaysThinkingEnabled",
    "tui", "theme", "language", "outputStyle", "sandbox", "respectGitignore",
    "skipDangerousModePermissionPrompt", "switchModelsOnFlag", "remoteControlAtStartup",
    "enabledPlugins", "extraKnownMarketplaces", "attribution", "includeCoAuthoredBy",
    "spinnerTipsEnabled", "spinnerVerbs", "terminalProgressBarEnabled", "promptSuggestionEnabled",
];
// Only behavior controls belong here. Credentials, endpoints, account selectors
// and config-directory redirects must never flow in from another account.
const ENV_KEYS: &[&str] = &[
    "CLAUDE_CODE_DISABLE_ADAPTIVE_THINKING", "_CLAUDE_CODE_EFFORT_LEVEL",
    "CLAUDE_CODE_EFFORT_LEVEL", "MAX_THINKING_TOKENS", "CLAUDE_CODE_MAX_OUTPUT_TOKENS",
    "MAX_MCP_OUTPUT_TOKENS",
];
fn read_object(path: &Path) -> Result<Value> {
    let file = match fs::File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(json!({})),
        Err(e) => return Err(e.into()),
    };
    let mut bytes = Vec::new(); file.take(1024 * 1024 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > 1024 * 1024 { return Err(Error::new(1, format!("Claude settings are too large: {}",path.display()))); }
    let value: Value = serde_json::from_slice(&bytes)
        .map_err(|_| Error::new(1, format!("Invalid Claude settings JSON: {}",path.display())))?;
    if !value.is_object() { return Err(Error::new(1,format!("Claude settings must be an object: {}",path.display()))); }
    Ok(value)
}
fn preferences(source: &Value) -> Value {
    let mut result = json!({});
    for key in KEYS { if let Some(value) = source.get(key) { result[*key] = value.clone(); } }
    let mut environment = json!({});
    for key in ENV_KEYS { if let Some(value) = source["env"].get(key) { environment[*key] = value.clone(); } }
    if !environment.as_object().unwrap().is_empty() { result["env"] = environment; }
    result
}
fn merge(previous: Option<&Value>, current: Option<&Value>, base: Option<&Value>) -> Option<Value> {
    if current == previous { return base.cloned(); }
    if current.is_some_and(Value::is_object)
        && previous.is_none_or(Value::is_object) && base.is_none_or(Value::is_object) {
        let mut keys = BTreeSet::new();
        for value in [previous,current,base].into_iter().flatten() {
            keys.extend(value.as_object().unwrap().keys());
        }
        let mut result = json!({});
        for key in keys {
            if let Some(value) = merge(previous.and_then(|v|v.get(key)),current.and_then(|v|v.get(key)),base.and_then(|v|v.get(key))) {
                result[key] = value;
            }
        }
        return Some(result);
    }
    current.cloned()
}
fn write(path: &Path, value: &Value) -> Result<()> {
    let mut file = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
    platform::make_private(file.as_file())?;
    file.write_all(&serde_json::to_vec_pretty(value).map_err(|e|Error::new(1,e.to_string()))?)?;
    file.write_all(b"\n")?;file.as_file().sync_all()?;
    platform::persist(file, path).map_err(|e|Error::new(1,e.to_string()))?;
    Ok(())
}

pub(super) fn preview(native: &Path, profile: &Path) -> Result<Value> {
    let current = read_object(&profile.join("settings.json"))?;
    if native == profile { return Ok(current); }
    let previous = read_object(&profile.join(".hgs-user-settings.json"))?;
    if previous.get("version").is_some() && previous["version"] != 1 { return Err(Error::new(1,"Unsupported settings inheritance version.")); }
    let base = preferences(&read_object(&native.join("settings.json"))?);
    let old = previous.get("base").cloned().unwrap_or_else(||json!({}));
    if !old.is_object() { return Err(Error::new(1,"Invalid settings inheritance baseline.")); }
    Ok(merge(Some(&old),Some(&current),Some(&base)).unwrap_or_else(||json!({})))
}

pub(super) fn inherit(native: &Path, profile: &Path) -> Result<()> {
    let path = profile.join("settings.json");
    let source = native.join("settings.json");
    if native == profile { return Ok(()); }
    if fs::symlink_metadata(&path).is_ok_and(|m|m.file_type().is_symlink()) {
        return Err(Error::new(1,"Managed Claude settings are a symlink; use a profile settings file to inherit preferences."));
    }
    let lock_path = profile.join("settings.json.hgs.lock");
    let guard = platform::private_open(fs::OpenOptions::new().create(true).truncate(false).read(true).write(true)).open(lock_path)?;
    let _guard = platform::FileLock::exclusive(guard)?;
    let base = preferences(&read_object(&source)?);
    let metadata = profile.join(".hgs-user-settings.json");
    let previous = read_object(&metadata)?;
    if previous.get("version").is_some() && previous["version"] != 1 {
        return Err(Error::new(1,"Unsupported Claude settings inheritance version."));
    }
    let old_base = previous.get("base").cloned().unwrap_or_else(||json!({}));
    if !old_base.is_object() { return Err(Error::new(1,"Invalid Claude settings inheritance baseline.")); }
    let current = read_object(&path)?;
    let merged = merge(Some(&old_base),Some(&current),Some(&base)).unwrap_or_else(||json!({}));
    if merged != current { write(&path,&merged)?; }
    let next = json!({"version":1,"source":source,"base":base});
    if next != previous { write(&metadata,&next)?; }
    Ok(())
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    #[test]
    fn inheritance_updates_without_replacing_profile_overrides_or_identity() {
        let tmp=tempfile::tempdir().unwrap(); let native=tmp.path().join("native");let profile=tmp.path().join("managed");
        fs::create_dir_all(&native).unwrap();fs::create_dir_all(&profile).unwrap();
        let src=native.join("settings.json");let dst=profile.join("settings.json");
        write(&src,&json!({"permissions":{"defaultMode":"bypassPermissions","allow":["Read"]},"theme":"dark","model":"opus",
            "env":{"ANTHROPIC_API_KEY":"secret","CLAUDE_CONFIG_DIR":"other","CLAUDE_CODE_EFFORT_LEVEL":"high"},
            "apiKeyHelper":"secret-helper", "hooks":{"wrong":true}})).unwrap();
        write(&dst,&json!({"theme":"light","hooks":{"hgs":true},"env":{"PROFILE_ONLY":"keep"}})).unwrap();
        inherit(&native,&profile).unwrap(); let first=read_object(&dst).unwrap();
        assert_eq!(first["permissions"]["defaultMode"],"bypassPermissions");assert_eq!(first["theme"],"light");
        assert_eq!(first["hooks"],json!({"hgs":true}));assert!(first.get("apiKeyHelper").is_none());
        assert_eq!(first["env"],json!({"PROFILE_ONLY":"keep","CLAUDE_CODE_EFFORT_LEVEL":"high"}));
        let mut local=first.clone();local["permissions"]["allow"]=json!(["Write"]);write(&dst,&local).unwrap();
        write(&src,&json!({"permissions":{"defaultMode":"default","allow":["Bash"]},"theme":"system"})).unwrap();
        inherit(&native,&profile).unwrap(); let next=read_object(&dst).unwrap();
        assert_eq!(next["permissions"],json!({"defaultMode":"default","allow":["Write"]}));
        assert_eq!(next["theme"],"light");assert!(next.get("model").is_none());assert_eq!(next["env"],json!({"PROFILE_ONLY":"keep"}));
        let bytes=fs::read(&dst).unwrap(); inherit(&native,&profile).unwrap();assert_eq!(fs::read(&dst).unwrap(),bytes);
        assert_eq!(fs::metadata(&dst).unwrap().permissions().mode()&0o777,0o600);
    }
    #[test]
    fn invalid_source_does_not_overwrite_profile() {
        let tmp=tempfile::tempdir().unwrap();let native=tmp.path().join("native");let profile=tmp.path().join("managed");
        fs::create_dir_all(&native).unwrap();fs::create_dir_all(&profile).unwrap();
        fs::write(native.join("settings.json"),b"invalid").unwrap();fs::write(profile.join("settings.json"),b"{}").unwrap();
        assert!(inherit(&native,&profile).is_err());assert_eq!(fs::read(profile.join("settings.json")).unwrap(),b"{}");
    }
}
