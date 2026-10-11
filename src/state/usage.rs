//! Read-only native session accounting. Cached offsets avoid replaying large
//! transcripts on every inspector poll. This cache contains counters, not text.
use super::*;
use sha2::{Digest, Sha256};
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Seek, SeekFrom};
use std::os::unix::fs::MetadataExt;

const MAX_READ: u64 = 32 * 1024 * 1024;

fn number(value: &Value) -> Option<u64> {
    value.as_u64().filter(|n| *n <= 9_007_199_254_740_991)
}
fn copy(out: &mut Value, source: &Value, fields: &[(&str, &str)]) {
    for (from, to) in fields {
        if let Some(n) = number(&source[*from]) {
            out[*to] = json!(n);
        }
    }
}
fn sum(value: &Value, keys: &[&str]) -> Option<u64> {
    keys.iter()
        .try_fold(0u64, |total, key| total.checked_add(number(&value[*key])?))
}
fn buckets(source: &Value, provider: &str) -> Value {
    let mut out = json!({});
    match provider {
        "codex" => copy(
            &mut out,
            source,
            &[
                ("input_tokens", "input"),
                ("cached_input_tokens", "cache_read"),
                ("cache_write_input_tokens", "cache_write"),
                ("output_tokens", "output"),
                ("reasoning_output_tokens", "reasoning"),
                ("total_tokens", "total"),
            ],
        ),
        "claude" => {
            copy(
                &mut out,
                source,
                &[
                    ("input_tokens", "uncached_input"),
                    ("output_tokens", "output"),
                    ("cache_read_input_tokens", "cache_read"),
                    ("cache_creation_input_tokens", "cache_write"),
                ],
            );
            copy(
                &mut out,
                &source["output_tokens_details"],
                &[("thinking_tokens", "reasoning")],
            );
            copy(
                &mut out,
                &source["server_tool_use"],
                &[
                    ("web_search_requests", "web_searches"),
                    ("web_fetch_requests", "web_fetches"),
                ],
            );
        }
        "kimi" => copy(
            &mut out,
            source,
            &[
                ("inputOther", "uncached_input"),
                ("output", "output"),
                ("inputCacheRead", "cache_read"),
                ("inputCacheCreation", "cache_write"),
            ],
        ),
        "dsh" => copy(
            &mut out,
            source,
            &[
                ("uncachedInputTokens", "uncached_input"),
                ("outputTokens", "output"),
                ("cacheReadTokens", "cache_read"),
                ("cacheWriteTokens", "cache_write"),
            ],
        ),
        _ => {}
    }
    if provider != "codex" {
        if let Some(input) = sum(&out, &["uncached_input", "cache_read", "cache_write"]) {
            out["input"] = json!(input);
        }
        if let Some(total) = sum(&out, &["input", "output"]) {
            out["total"] = json!(total);
        }
    }
    out
}

fn accumulate(state: &mut Value, sample: Value, id: Option<&str>) {
    if sample.as_object().is_none_or(|o| o.is_empty()) {
        return;
    }
    let previous = id
        .map(|id| state["seen"][id].clone())
        .unwrap_or(Value::Null);
    for (key, value) in sample.as_object().unwrap() {
        let total = number(&state["totals"][key]).unwrap_or(0);
        let before = number(&previous[key]).unwrap_or(0);
        state["totals"][key] = json!(total
            .saturating_sub(before)
            .saturating_add(number(value).unwrap_or(0)));
    }
    if let Some(id) = id {
        state["seen"][id] = sample.clone();
    }
    if previous.is_null() {
        state["requests"] = json!(number(&state["requests"]).unwrap_or(0) + 1);
    }
    state["last"] = sample;
}

// TTL is read from native usage, never guessed from provider/account names.
// A mixed prefix can contain both TTLs; the shorter one is the conservative
// deadline. This remains an estimate: prompt/model changes can invalidate it.
fn cache_observation(state: &mut Value, event: &Value, usage: &Value, provider: &str) {
    let mut at=search::timestamp(&event["timestamp"]);
    if at==0.0 { at=search::timestamp(&event["time"]); if at>10_000_000_000.0 {at/=1000.0;} }
    let sample=buckets(usage,provider);
    if sample.as_object().is_none_or(|v|v.is_empty()) {return;}
    let mut cache=json!({"status":"unknown","source":"native_usage","observed_at":at,
        "cache_read":sample["cache_read"],"cache_write":sample["cache_write"],"input":sample["input"]});
    if provider=="claude" {
        let writes=&usage["cache_creation"];
        let ttl=if number(&writes["ephemeral_5m_input_tokens"]).unwrap_or(0)>0 {300}
            else if number(&writes["ephemeral_1h_input_tokens"]).unwrap_or(0)>0 {3600} else {0};
        // Do not extend a deadline for duplicate streaming snapshots of the
        // same request. It started caching by the first observed response.
        let same=state["cache_request_id"]==event["message"]["id"] && !event["message"]["id"].is_null();
        let previous=&state["prompt_cache"];
        let model=string(&event["message"],"model");
        let ttl=if ttl>0 {ttl} else if state["cache_model"]==model {previous["ttl_seconds"].as_u64().unwrap_or(0)} else {0};
        if ttl>0 && at>0.0 && number(&sample["cache_read"]).unwrap_or(0)+number(&sample["cache_write"]).unwrap_or(0)>0 {
            let deadline=if same && previous["expires_at"].is_number() {previous["expires_at"].as_f64().unwrap().min(at+ttl as f64)}
                else {at+ttl as f64};
            cache["status"]=json!("warm");cache["expires_at"]=json!(deadline);
            cache["ttl_seconds"]=json!(ttl);cache["estimated"]=json!(true);
        }
        state["cache_request_id"]=event["message"]["id"].clone();state["cache_model"]=json!(model);
    }
    state["prompt_cache"]=cache;
}

fn observe(state: &mut Value, event: &Value, provider: &str, conversation: &str) {
    match provider {
        "codex" if event["type"] == "event_msg" && event["payload"]["type"] == "token_count" => {
            let info = &event["payload"]["info"];
            let totals = buckets(&info["total_token_usage"], provider);
            if !totals.as_object().unwrap().is_empty() {
                state["totals"] = totals;
            }
            let last = buckets(&info["last_token_usage"], provider);
            if !last.as_object().unwrap().is_empty() {
                state["last"] = last;
                cache_observation(state,event,&info["last_token_usage"],provider);
            }
            if let Some(n) = number(&info["model_context_window"]) {
                state["context"]["limit"] = json!(n);
            }
            if let Some(n) = number(&state["last"]["total"]) {
                state["context"]["used"] = json!(n);
            }
        }
        "claude" if event["sessionId"] == conversation && event["type"] == "assistant" => {
            let message = &event["message"];
            let id = string(message, "id");
            if id.is_empty() || id.len() > 256 {
                return;
            }
            accumulate(state, buckets(&message["usage"], provider), Some(id));
            if search::timestamp(&event["timestamp"]) >= state["compaction_at"].as_f64().unwrap_or(0.0) {
                cache_observation(state,event,&message["usage"],provider);
                if let Some(n) = number(&state["last"]["input"]) {
                    state["context"]["used"] = json!(n);
                }
            }
            if state["cost_usd"].is_number() {
                state["cost_stale"] = json!(true);
            }
        }
        "claude" if event["sessionId"] == conversation && event["type"] == "cost-state" => {
            if let Some(cost) = event["totalCostUSD"]
                .as_f64()
                .filter(|n| n.is_finite() && *n >= 0.0)
            {
                state["cost_usd"] = json!(cost);
                state["cost_stale"] = json!(false);
                state["cost_incomplete"] = json!(event["hasUnknownModelCost"] == true);
            }
            copy(
                &mut state["extras"],
                event,
                &[
                    ("totalAPIDuration", "api_ms"),
                    ("totalToolDuration", "tool_ms"),
                    ("totalLinesAdded", "lines_added"),
                    ("totalLinesRemoved", "lines_removed"),
                ],
            );
        }
        "claude" if event["sessionId"] == conversation && event["type"] == "system"
            && event["subtype"] == "compact_boundary" => {
            // A new summary replaces the prompt measured by earlier requests.
            // Keep cumulative billing, but neither its old size nor cache TTL
            // describes the compacted prompt. Do not invent a new warm cache.
            state["context"].as_object_mut().unwrap().remove("used");
            if let Some(tokens) = number(&event["compactMetadata"]["postTokens"]) {
                state["context"]["used"] = json!(tokens);
            }
            state["prompt_cache"] = json!({"status":"unknown","source":"native_compaction",
                "observed_at":search::timestamp(&event["timestamp"])});
            state["compaction_at"] = json!(search::timestamp(&event["timestamp"]));
            for key in ["cache_request_id", "cache_model"] {
                state.as_object_mut().unwrap().remove(key);
            }
        }
        "kimi" if ["", "main"].contains(&string(event, "agentId")) => {
            if ["profile.bind", "llm.request"].contains(&string(event, "type")) {
                state["model"] = event["modelAlias"].clone();
            }
            if event["type"] == "usage.record" {
                // The matching message and step.end repeat this usage. Count
                // only its authoritative ledger entry (also includes compaction).
                accumulate(state, buckets(&event["usage"], provider), None);
                cache_observation(state,event,&event["usage"],provider);
            } else if event["type"] == "token_counting.measured" {
                if let Some(n) = number(&event["tokens"]) {
                    state["context"]["used"] = json!(n);
                }
            }
        }
        _ => {}
    }
}

fn output(state: &Value, provider: &str, partial: bool) -> Value {
    let available = state["totals"].as_object().is_some_and(|o| !o.is_empty())
        || state["cost_usd"].is_number()
        || state["context"]["used"].is_number();
    json!({"status":if available {"ok"} else {"unavailable"},"source":provider,
        "scope":"conversation","includes_subagents":false,"partial":partial,
        "totals":state["totals"],"last_request":state["last"],"requests":state["requests"],
        "prompt_cache":state["prompt_cache"],"context":state["context"],"model":state["model"],"cost_usd":state["cost_usd"],"cost_stale":state["cost_stale"],
        "cost_incomplete":state["cost_incomplete"],"extras":state["extras"]})
}

fn scan(path: &Path, cache_path: &Path, provider: &str, conversation: &str) -> Result<Value> {
    let mut file = File::open(path).map_err(|e| e.to_string())?;
    let meta = file.metadata().map_err(|e| e.to_string())?;
    let mut prefix = vec![0; meta.len().min(256) as usize];
    file.read_exact(&mut prefix).map_err(|e| e.to_string())?;
    let fingerprint = format!(
        "{}:{}:{:x}",
        meta.dev(),
        meta.ino(),
        Sha256::digest(&prefix)
    );
    if provider == "codex" {
        file.seek(SeekFrom::Start(0)).map_err(|e| e.to_string())?;
        let mut first = String::new();
        BufReader::new((&mut file).take(256 * 1024))
            .read_line(&mut first)
            .map_err(|e| e.to_string())?;
        let first: Value = serde_json::from_str(&first).map_err(|e| e.to_string())?;
        if first["type"] != "session_meta" || first["payload"]["id"] != conversation {
            return Err("Native conversation identity mismatch".into());
        }
    }
    let _guard = lock(Some(&cache_path.with_extension("lock")))?;
    let mut state: Value = fs::read(cache_path)
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or(Value::Null);
    let mut offset = number(&state["offset"]).unwrap_or(0);
    let stamp = json!([meta.mtime(), meta.mtime_nsec()]);
    if state["version"] != 4
        || state["fingerprint"] != fingerprint
        || offset > meta.len()
        || (state["size"] == meta.len() && state["stamp"] != stamp)
    {
        state = json!({"version":4,"fingerprint":fingerprint,"totals":{},"seen":{},"context":{}});
        offset = 0;
        // Codex's last snapshot is cumulative; no need to replay its full log.
        if provider == "codex" && meta.len() > 8 * 1024 * 1024 {
            offset = meta.len() - 8 * 1024 * 1024;
            file.seek(SeekFrom::Start(offset))
                .map_err(|e| e.to_string())?;
            let mut skip = Vec::new();
            offset += BufReader::new((&mut file).take(MAX_READ))
                .read_until(b'\n', &mut skip)
                .map_err(|e| e.to_string())? as u64;
        }
    }
    if state["offset"] == meta.len() && state["stamp"] == stamp {
        return Ok(output(&state, provider, false));
    }
    file.seek(SeekFrom::Start(offset))
        .map_err(|e| e.to_string())?;
    let mut reader = BufReader::new(file.take(MAX_READ));
    let mut line = Vec::new();
    loop {
        line.clear();
        let count = reader
            .read_until(b'\n', &mut line)
            .map_err(|e| e.to_string())?;
        if count == 0 || !line.ends_with(b"\n") {
            break;
        }
        offset += count as u64;
        if let Ok(event) = serde_json::from_slice::<Value>(&line) {
            observe(&mut state, &event, provider, conversation);
        }
    }
    state["offset"] = json!(offset);
    state["size"] = json!(meta.len());
    state["stamp"] = stamp;
    atomic(cache_path, &state.to_string())?;
    Ok(output(&state, provider, offset < meta.len()))
}

fn kimi_capacity(config: &str, model: &str) -> Option<u64> {
    if model.is_empty() {
        return None;
    }
    let config = config.parse::<toml_edit::DocumentMut>().ok()?;
    let value = config
        .get("models")?
        .get(model)?
        .get("max_context_size")?
        .as_integer()?;
    (value > 0 && value <= 9_007_199_254_740_991).then_some(value as u64)
}

pub(super) fn read(record: &Value) -> Value {
    let provider = string(record, "agent");
    let id = string(record, "conversation_id");
    let read = || -> Result<Value> {
        let path = if provider == "kimi" {
            questions::wire_paths(record)?
                .into_iter()
                .next()
                .ok_or("No native usage yet")?
        } else {
            if !["codex", "claude"].contains(&provider) || uuid::Uuid::parse_str(id).is_err() {
                return Err("Native usage unavailable".into());
            }
            let p = PathBuf::from(string(record, "transcript"));
            if !p.is_absolute()
                || !p
                    .file_name()
                    .is_some_and(|f| f.to_string_lossy().contains(id))
            {
                return Err("Native transcript unavailable".into());
            }
            p
        };
        let key = format!(
            "cache-v2-{:x}",
            Sha256::digest(json!([provider, id, path]).to_string().as_bytes())
        );
        let mut result = scan(
            &path,
            &root().join("usage-cache").join(format!("{key}.json")),
            provider,
            id,
        )?;
        if provider == "kimi" {
            // Native alias configuration declares context capacity explicitly.
            // llm.request.maxTokens is an output limit and must not be used.
            let mut config = String::new();
            if File::open(questions::kimi_home(record).join("config.toml"))
                .and_then(|file| file.take(1024 * 1024).read_to_string(&mut config))
                .is_ok()
            {
                if let Some(limit) = kimi_capacity(&config, string(&result, "model")) {
                    result["context"]["limit"] = json!(limit);
                    result["context"]["limit_source"] = json!("model_config");
                }
            }
        }
        Ok(result)
    };
    read().unwrap_or_else(|_| json!({"status":"unavailable","source":provider}))
}

pub(super) fn dsh(projections: &Value) -> Value {
    let totals = buckets(&projections["tokenUsage"], "dsh");
    let mut state = json!({"totals":totals,"context":{"estimated":true}});
    copy(
        &mut state["context"],
        &projections["contextPressure"],
        &[("projectedTokens", "used"), ("contextWindow", "limit")],
    );
    if state["context"]["used"].is_null() {
        copy(
            &mut state["context"],
            &projections["contextPressure"],
            &[("pressureTokens", "used")],
        );
    }
    output(&state, "dsh", false)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn claude_compaction_replaces_context_and_invalidates_old_cache_evidence() {
        let mut state = json!({"totals":{},"seen":{},"context":{"limit":1000000}});
        let mut reply = json!({"type":"assistant","sessionId":"one","timestamp":"2026-10-11T05:00:00Z",
            "message":{"id":"before","model":"fixture","usage":{"input_tokens":10,"cache_read_input_tokens":331000,
                "cache_creation_input_tokens":200,"output_tokens":30,"cache_creation":{"ephemeral_1h_input_tokens":200}}}});
        observe(&mut state,&reply,"claude","one");
        let totals = state["totals"].clone();
        assert_eq!(state["prompt_cache"]["status"],"warm");
        let mut boundary = json!({"type":"system","subtype":"compact_boundary","sessionId":"other",
            "timestamp":"2026-10-11T06:15:00Z","compactMetadata":{"trigger":"manual","preTokens":331210,"postTokens":23982}});
        observe(&mut state,&boundary,"claude","one");
        assert_eq!(state["context"]["used"],331210);
        boundary["sessionId"]=json!("one");
        observe(&mut state,&boundary,"claude","one");
        assert_eq!(state["context"],json!({"limit":1000000,"used":23982}));
        assert_eq!(state["prompt_cache"]["status"],"unknown");
        assert!(state["prompt_cache"]["expires_at"].is_null());
        assert_eq!(state["totals"],totals);
        // Preserved pre-compaction snapshots cannot revive the old context.
        observe(&mut state,&reply,"claude","one");
        assert_eq!(state["context"]["used"],23982);
        assert_eq!(state["prompt_cache"]["status"],"unknown");
        reply["timestamp"]=json!("2026-10-11T06:16:00Z");
        reply["message"]["id"]=json!("after");
        reply["message"]["usage"]=json!({"input_tokens":10,"cache_read_input_tokens":0,"cache_creation_input_tokens":24000,
            "cache_creation":{"ephemeral_5m_input_tokens":24000},"output_tokens":20});
        observe(&mut state,&reply,"claude","one");
        assert_eq!(state["context"]["used"],24010);
        assert_eq!(state["prompt_cache"]["ttl_seconds"],300);
        boundary["timestamp"]=json!("2026-10-11T06:17:00Z");
        boundary["compactMetadata"].as_object_mut().unwrap().remove("postTokens");
        observe(&mut state,&boundary,"claude","one");
        assert!(state["context"]["used"].is_null());
        assert_eq!(state["context"]["limit"],1000000);
        assert_eq!(state["prompt_cache"]["status"],"unknown");
    }

    #[test]
    fn claude_compaction_upgrades_existing_incremental_usage_cache() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("transcript.jsonl");
        let cache = temp.path().join("usage.json");
        fs::write(&path,concat!(
            "{\"type\":\"assistant\",\"sessionId\":\"one\",\"message\":{\"id\":\"before\",\"usage\":{\"input_tokens\":331000,\"cache_read_input_tokens\":0,\"cache_creation_input_tokens\":0}}}\n",
            "{\"type\":\"system\",\"subtype\":\"compact_boundary\",\"sessionId\":\"one\",\"timestamp\":\"2026-10-11T06:15:00Z\",\"compactMetadata\":{\"postTokens\":24000}}\n"
        )).unwrap();
        assert_eq!(scan(&path,&cache,"claude","one").unwrap()["context"]["used"],24000);
        let mut old: Value = serde_json::from_slice(&fs::read(&cache).unwrap()).unwrap();
        old["version"]=json!(3); old["context"]["used"]=json!(331000);
        fs::write(&cache,old.to_string()).unwrap();
        let fresh = scan(&path,&cache,"claude","one").unwrap();
        assert_eq!(fresh["context"]["used"],24000);
        assert_eq!(fresh["prompt_cache"]["source"],"native_compaction");
        assert_eq!(fresh["totals"]["input"],331000);
    }

    #[test]
    fn cache_deadlines_use_reported_ttl_and_do_not_refresh_duplicate_responses() {
        let mut state=json!({});
        let mut event=json!({"timestamp":"2026-10-06T10:00:00Z","message":{"id":"one","model":"claude-model"}});
        let mut usage=json!({"input_tokens":12,"cache_read_input_tokens":500,"cache_creation_input_tokens":100,"cache_creation":{"ephemeral_1h_input_tokens":100,"ephemeral_5m_input_tokens":0}});
        cache_observation(&mut state,&event,&usage,"claude");let deadline=state["prompt_cache"]["expires_at"].clone();
        assert_eq!(state["prompt_cache"]["ttl_seconds"],3600);
        event["timestamp"]=json!("2026-10-06T10:01:00Z");cache_observation(&mut state,&event,&usage,"claude");assert_eq!(state["prompt_cache"]["expires_at"],deadline);
        event["message"]["id"]=json!("two");usage["cache_creation"]["ephemeral_5m_input_tokens"]=json!(10);
        cache_observation(&mut state,&event,&usage,"claude");assert_eq!(state["prompt_cache"]["ttl_seconds"],300);
        cache_observation(&mut state,&event,&json!({"input_tokens":1000,"cached_input_tokens":900}),"codex");
        assert_eq!(state["prompt_cache"]["status"],"unknown");assert!(state["prompt_cache"]["expires_at"].is_null());
    }
    #[test]
    fn kimi_capacity_uses_exact_native_alias_and_not_output_limit() {
        let config = "default_model = 'other'\n[models.'kimi-code/k3']\nmax_context_size = 1048576\nmax_output_size = 16384\n[models.other]\nmax_context_size = 262144\n";
        assert_eq!(kimi_capacity(config, "kimi-code/k3"), Some(1048576));
        assert_eq!(kimi_capacity(config, "k3"), None);
        assert_eq!(kimi_capacity(config, ""), None);
        assert_eq!(
            kimi_capacity("[models.k3]\nmax_context_size = 0", "k3"),
            None
        );
        let mut state = json!({"context":{}});
        observe(
            &mut state,
            &json!({"type":"llm.request","agentId":"main","modelAlias":"kimi-code/k3","maxTokens":16384}),
            "kimi",
            "one",
        );
        assert_eq!(state["model"], "kimi-code/k3");
        assert!(state["context"]["limit"].is_null());
        observe(
            &mut state,
            &json!({"type":"profile.bind","agentId":"main","modelAlias":"other"}),
            "kimi",
            "one",
        );
        assert_eq!(state["model"], "other");
    }
    #[test]
    fn provider_accounting_does_not_double_count_cache_or_mirrors() {
        let mut state = json!({"totals":{},"seen":{},"context":{}});
        let mut event = json!({"type":"assistant","sessionId":"one","message":{"id":"msg-a","usage":{"input_tokens":10,"cache_read_input_tokens":100,"cache_creation_input_tokens":20,"output_tokens":5,"output_tokens_details":{"thinking_tokens":3}}}});
        observe(&mut state, &event, "claude", "one");
        observe(&mut state, &event, "claude", "one");
        event["message"]["usage"]["output_tokens"] = json!(9);
        observe(&mut state, &event, "claude", "one");
        observe(&mut state, &event, "claude", "other");
        assert_eq!(state["totals"]["total"], 139);
        assert_eq!(state["totals"]["reasoning"], 3);
        assert_eq!(state["requests"], 1);
        observe(
            &mut state,
            &json!({"type":"cost-state","sessionId":"other","totalCostUSD":99}),
            "claude",
            "one",
        );
        assert!(state["cost_usd"].is_null());
        observe(
            &mut state,
            &json!({"type":"cost-state","sessionId":"one","totalCostUSD":1.2345,"totalAPIDuration":1234}),
            "claude",
            "one",
        );
        assert_eq!(state["cost_usd"], 1.2345);
        assert_eq!(state["extras"]["api_ms"], 1234);
        observe(&mut state, &event, "claude", "one");
        assert_eq!(state["cost_stale"], true);
        let codex = json!({"type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"cached_input_tokens":80,"output_tokens":10,"reasoning_output_tokens":6,"total_tokens":110}}}});
        observe(&mut state, &codex, "codex", "one");
        observe(&mut state, &codex, "codex", "one");
        assert_eq!(state["totals"]["total"], 110);
        let projections = json!({"tokenUsage":{"uncachedInputTokens":10,"outputTokens":5,"cacheReadTokens":100,"cacheWriteTokens":20},"contextPressure":{"projectedTokens":145,"contextWindow":1000}});
        let result = dsh(&projections);
        assert_eq!(result["totals"]["total"], 135);
        assert_eq!(result["context"]["used"], 145);
    }
    #[test]
    fn incremental_cache_handles_partial_append_rotation_and_kimi_mirrors() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("wire.jsonl");
        let cache = temp.path().join("cache.json");
        let event=json!({"type":"usage.record","agentId":"main","usage":{"inputOther":10,"output":5,"inputCacheRead":20,"inputCacheCreation":0}}).to_string()+"\n";
        fs::write(&path, &event).unwrap();
        let first = scan(&path, &cache, "kimi", "one").unwrap();
        assert_eq!(first["totals"]["total"], 35);
        assert_eq!(
            scan(&path, &cache, "kimi", "one").unwrap()["totals"],
            first["totals"]
        );
        fs::write(&path, format!("{event}{}", &event[..30])).unwrap();
        assert_eq!(
            scan(&path, &cache, "kimi", "one").unwrap()["totals"],
            first["totals"]
        );
        fs::write(&path, format!("{event}{event}")).unwrap();
        assert_eq!(
            scan(&path, &cache, "kimi", "one").unwrap()["totals"]["total"],
            70
        );
        fs::write(&path, &event).unwrap();
        assert_eq!(
            scan(&path, &cache, "kimi", "one").unwrap()["totals"]["total"],
            35
        );
        let mut state = json!({"totals":{},"context":{}});
        observe(
            &mut state,
            &json!({"type":"agent.message.appended","message":{"meta":{"usage":{"inputOther":99}}}}),
            "kimi",
            "one",
        );
        assert_eq!(state["totals"], json!({}));
    }
    #[test]
    fn codex_rejects_mismatched_identity_even_with_cached_usage() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("rollout.jsonl");
        let cache = temp.path().join("cache.json");
        fs::write(
            &path,
            json!({"type":"session_meta","payload":{"id":"one"}}).to_string() + "\n",
        )
        .unwrap();
        assert_eq!(
            scan(&path, &cache, "codex", "one").unwrap()["status"],
            "unavailable"
        );
        assert!(scan(&path, &cache, "codex", "other").is_err());
        assert_eq!(dsh(&json!({}))["status"], "unavailable");
        assert_eq!(
            buckets(&json!({"input_tokens":-3,"output_tokens":null}), "claude"),
            json!({})
        );
    }
}
