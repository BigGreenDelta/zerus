//! OpenCode 2 question forms and permission requests as Zerus question
//! cards, and the validated replies for them. The card shapes and answer
//! outcomes are pinned by `tests/fixtures/opencode-form-contract.json`,
//! which the mobile clients' tests share.
use super::super::*;
use sha2::{Digest, Sha256};

const UNAVAILABLE: &str = "Answer this form in OpenCode";
const MAX_FIELDS: usize = 32;
const MAX_OPTIONS: usize = 64;
const MAX_TEXT: usize = 16 * 1024;
const MAX_OPTION_ID: usize = 200;

fn option_id(index: usize, option: &Value) -> String {
    match option["value"].as_str() {
        Some(value) if value.chars().count() <= MAX_OPTION_ID => value.to_owned(),
        _ => index.to_string(),
    }
}
fn options(field: &Value) -> &[Value] {
    field["options"].as_array().map_or(&[], Vec::as_slice)
}
fn hidden(field: &Value) -> bool {
    field["hidden"] == true
}
fn kind(field: &Value) -> &str {
    string(field, "type")
}
/// Whether phase 1 can show and answer every visible field of the form.
fn supported(form: &Value) -> bool {
    let Some(fields) = form["fields"].as_array() else {
        return false;
    };
    let mut visible = 0;
    for field in fields {
        if field.get("when").is_some_and(|when| !when.is_null())
            || !["string", "boolean", "multiselect"].contains(&kind(field))
            || string(field, "key").is_empty()
            || options(field).len() > MAX_OPTIONS
        {
            return false;
        }
        visible += usize::from(!hidden(field));
    }
    !fields.is_empty() && fields.len() <= MAX_FIELDS && visible > 0
}
fn item(field: &Value) -> Value {
    let header = if string(field, "title").is_empty() {
        string(field, "key")
    } else {
        string(field, "title")
    };
    let prompt = if string(field, "description").is_empty() {
        header
    } else {
        string(field, "description")
    };
    let multi = kind(field) == "multiselect";
    let mut item = json!({"id":field["key"],"header":header,"prompt":prompt,"question":prompt,
        "multi_select":multi,"allow_multiple":multi,"required":field["required"] != false});
    let listed: Vec<Value> = if kind(field) == "boolean" {
        vec![
            json!({"id":"yes","label":"Yes"}),
            json!({"id":"no","label":"No"}),
        ]
    } else {
        options(field)
            .iter()
            .take(MAX_OPTIONS)
            .enumerate()
            .map(|(i, o)| {
                let mut option = json!({"id":option_id(i,o),"label":o["label"]});
                if o["description"].is_string() {
                    option["description"] = o["description"].clone();
                }
                option
            })
            .collect()
    };
    let choice = !listed.is_empty();
    item["options"] = json!(listed);
    item["input_kind"] = json!(if choice { "choice" } else { "text" });
    item["allow_other"] = json!(match kind(field) {
        "boolean" => false,
        _ if choice => field["custom"] == true,
        _ => true,
    });
    match kind(field) {
        "boolean" => {
            if let Some(default) = field["default"].as_bool() {
                item["default_option_ids"] = json!([if default { "yes" } else { "no" }]);
            }
        }
        "multiselect" => {
            if let Some(max) = field["maxItems"].as_u64() {
                item["max_items"] = json!(max);
            }
            if let Some(min) = field["minItems"].as_u64() {
                item["min_items"] = json!(min);
            }
            let defaults: Vec<Value> = field["default"].as_array().cloned().unwrap_or_default();
            if !defaults.is_empty() {
                item["default_option_ids"] = json!(defaults);
            }
        }
        _ => {
            if let Some(max) = field["maxLength"].as_u64() {
                item["max_length"] = json!(max);
            }
            if let Some(default) = field["default"].as_str() {
                if choice {
                    item["default_option_ids"] = json!([default]);
                } else {
                    item["default_text"] = json!(default);
                }
            }
        }
    }
    item
}
fn permission_item(native: &Value) -> Value {
    let resources: Vec<&str> = native["resources"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .collect();
    let prompt = format!("{}\n{}", string(native, "action"), resources.join("\n"));
    json!({"id":"permission","header":"Permission","prompt":prompt,"question":prompt,
        "allow_multiple":false,"multi_select":false,"required":true,"allow_other":false,
        // `broader` reaches past this one request: clients confirm it separately.
        "options":[{"id":"once","label":"Allow once","scope":"once"},
            {"id":"always","label":"Always allow","scope":"broader"},
            {"id":"reject","label":"Reject","description":"Rejects every pending request in this session","scope":"once"}]})
}
/// The pending cards of this session's forms and permission requests.
pub(super) fn cards(record: &Value, forms: &Value, permissions: &Value) -> Vec<Value> {
    let mut cards = Vec::new();
    for (kind, pending) in [("form", forms), ("permission", permissions)] {
        for native in pending.as_array().into_iter().flatten() {
            if native["sessionID"] != record["conversation_id"] {
                continue;
            }
            let (mut items, mut answerable) = if kind == "permission" {
                (vec![permission_item(native)], true)
            } else {
                let items = native["fields"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .filter(|f| !hidden(f))
                    .take(MAX_FIELDS)
                    .map(item)
                    .collect::<Vec<_>>();
                (items, supported(native))
            };
            // A card never carries more than MAX_TEXT of question text.
            if json!(items).to_string().len() > MAX_TEXT {
                items.clear();
                answerable = false;
            }
            let hash = format!(
                "{:x}",
                Sha256::digest(
                    json!([record["run_id"], record["conversation_id"], native])
                        .to_string()
                        .as_bytes()
                )
            );
            let mut card = json!({"question_id":native["id"],"question_hash":hash,"run_id":record["run_id"],
                "conversation_id":record["conversation_id"],"kind":format!("opencode_{kind}"),"status":"pending",
                "answerable":answerable,"can_answer":answerable,"approval":kind=="permission",
                "answer_transport":"native_opencode","agent_id":"main","questions":items,"source":"opencode_native"});
            if kind == "permission" {
                card["approval_choices"] = json!(true);
            }
            if kind == "form" && !string(native, "title").is_empty() {
                card["title"] = native["title"].clone();
            }
            if !answerable {
                card["unavailable_reason"] = json!(UNAVAILABLE);
            }
            cards.push(card);
        }
    }
    cards
}
/// The answer object of one form reply, from the exact form and the answers
/// to the card shown for it.
pub(super) fn form_answer(card: &Value, form: &Value, answers: &[Value]) -> Result<Value> {
    if card["can_answer"] != true || !supported(form) {
        return Err(UNAVAILABLE.into());
    }
    let shown = card["questions"].as_array().map_or(0, Vec::len);
    if answers.len() != shown {
        return Err("Answer every question exactly once".into());
    }
    let mut reply = serde_json::Map::new();
    for field in form["fields"].as_array().into_iter().flatten() {
        let key = string(field, "key");
        if hidden(field) {
            if !field["default"].is_null() {
                reply.insert(key.to_owned(), field["default"].clone());
            }
            continue;
        }
        let matches: Vec<&Value> = answers.iter().filter(|a| a["question_id"] == key).collect();
        if matches.len() != 1 {
            return Err("Question answers are missing or duplicated".into());
        }
        if let Some(value) = field_value(field, matches[0])? {
            reply.insert(key.to_owned(), value);
        }
    }
    Ok(json!({"answer": reply}))
}
fn field_value(field: &Value, answer: &Value) -> Result<Option<Value>> {
    let selected = answer["selected_option_ids"]
        .as_array()
        .ok_or("Invalid selected answers")?;
    let text = string(answer, "text");
    let required = field["required"] != false;
    let kind = kind(field);
    let choices: Vec<(String, &Value)> = options(field)
        .iter()
        .enumerate()
        .map(|(i, o)| (option_id(i, o), o))
        .collect();
    let mut values: Vec<Value> = Vec::new();
    for (i, id) in selected.iter().enumerate() {
        if selected[..i].contains(id) {
            return Err("Choose each answer only once".into());
        }
        if kind == "boolean" {
            values.push(match id.as_str() {
                Some("yes") => json!(true),
                Some("no") => json!(false),
                _ => return Err("Unknown answer option".into()),
            });
            continue;
        }
        let (_, option) = choices
            .iter()
            .find(|(candidate, _)| id.as_str() == Some(candidate))
            .ok_or("Unknown answer option")?;
        values.push(option["value"].clone());
    }
    if !text.is_empty() {
        if kind == "boolean" || (!choices.is_empty() && field["custom"] != true) {
            return Err("Choose one of the offered answers".into());
        }
        if let (Some(max), true) = (field["maxLength"].as_u64(), kind == "string") {
            if text.chars().count() as u64 > max {
                return Err(format!("Answer is longer than {max} characters"));
            }
        }
        if let (Some(min), true) = (field["minLength"].as_u64(), kind == "string") {
            if (text.chars().count() as u64) < min {
                return Err(format!("Answer is shorter than {min} characters"));
            }
        }
        values.push(json!(text));
    }
    if values.is_empty() {
        return if required {
            Err("An answer is required".into())
        } else {
            Ok(None)
        };
    }
    if kind != "multiselect" {
        if values.len() > 1 {
            return Err("This question accepts one answer".into());
        }
        return Ok(values.pop());
    }
    if let Some(max) = field["maxItems"].as_u64() {
        if values.len() as u64 > max {
            return Err(format!("Choose at most {max} answers"));
        }
    }
    if let Some(min) = field["minItems"].as_u64() {
        if (values.len() as u64) < min {
            return Err(format!("Choose at least {min} answers"));
        }
    }
    Ok(Some(json!(values)))
}
/// The decision of one permission reply.
pub(super) fn permission_decision(answers: &[Value]) -> Result<&'static str> {
    let [answer] = answers else {
        return Err("Answer every question exactly once".into());
    };
    if !string(answer, "text").is_empty() {
        return Err("Choose an offered permission outcome".into());
    }
    match answer["selected_option_ids"].as_array().map(Vec::as_slice) {
        Some([id]) => match id.as_str() {
            Some("once") => Ok("once"),
            Some("always") => Ok("always"),
            Some("reject") => Ok("reject"),
            _ => Err("Unknown answer option".into()),
        },
        _ => Err("Choose one permission outcome".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn contract() -> Value {
        serde_json::from_slice(include_bytes!(
            "../../../tests/fixtures/opencode-form-contract.json"
        ))
        .unwrap()
    }
    #[test]
    fn contract_cases() {
        let contract = contract();
        let names: Vec<&str> = contract["cases"]
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c["name"].as_str().unwrap())
            .collect();
        assert_eq!(names.len(), 17);
        for case in contract["cases"].as_array().unwrap() {
            let record = json!({"run_id": "run", "conversation_id": "ses_A"});
            let name = case["name"].as_str().unwrap();
            let permission = case["card"]["kind"] == "opencode_permission";
            let (forms, permissions) = if permission {
                (json!([]), json!([case["form"]]))
            } else {
                (json!([case["form"]]), json!([]))
            };
            let cards = cards(&record, &forms, &permissions);
            assert_eq!(cards.len(), 1, "{name}");
            for (key, expected) in case["card"].as_object().unwrap() {
                match key.as_str() {
                    "item" => {
                        for (k, v) in expected.as_object().unwrap() {
                            assert_eq!(&cards[0]["questions"][0][k.as_str()], v, "{name}.{k}");
                        }
                    }
                    "question_count" => assert_eq!(
                        cards[0]["questions"].as_array().unwrap().len() as u64,
                        expected.as_u64().unwrap(),
                        "{name}.question_count"
                    ),
                    _ => assert_eq!(&cards[0][key.as_str()], expected, "{name}.{key}"),
                }
            }
            if case.get("answers").is_none() {
                assert!(case["card"]["can_answer"] == false, "{name}");
                continue;
            }
            {
                let check = case;
                let answers = check["answers"].as_array().unwrap();
                let result = if permission {
                    permission_decision(answers).map(|d| json!({"decision": d}))
                } else {
                    form_answer(&cards[0], &case["form"], answers)
                };
                match (check.get("reply"), check.get("error")) {
                    (Some(reply), _) => assert_eq!(&result.unwrap(), reply, "{name}"),
                    (_, Some(error)) => {
                        assert_eq!(result.unwrap_err(), error.as_str().unwrap(), "{name}")
                    }
                    _ => panic!("{name} needs reply or error"),
                }
            }
        }
    }
    #[test]
    fn unanswerable_forms_refuse_answers_and_other_sessions_are_skipped() {
        let contract = contract();
        let number = contract["cases"]
            .as_array()
            .unwrap()
            .iter()
            .find(|c| c["name"] == "unsupported-number")
            .unwrap();
        let record = json!({"run_id": "run", "conversation_id": "ses_A"});
        let cards = cards(&record, &json!([number["form"]]), &json!([]));
        let answers = [json!({"question_id":"n","selected_option_ids":[],"text":"3"})];
        assert_eq!(
            form_answer(&cards[0], &number["form"], &answers).unwrap_err(),
            UNAVAILABLE
        );
        let other = json!({"run_id": "run", "conversation_id": "ses_B"});
        assert!(super::cards(&other, &json!([number["form"]]), &json!([])).is_empty());
    }
    #[test]
    fn hash_binds_run_conversation_and_native_request() {
        let form = contract()["cases"][0]["form"].clone();
        let a = cards(
            &json!({"run_id":"r1","conversation_id":"ses_A"}),
            &json!([form]),
            &json!([]),
        );
        let b = cards(
            &json!({"run_id":"r2","conversation_id":"ses_A"}),
            &json!([form]),
            &json!([]),
        );
        assert_eq!(a[0]["question_hash"].as_str().unwrap().len(), 64);
        assert_ne!(a[0]["question_hash"], b[0]["question_hash"]);
    }
    #[test]
    fn hidden_defaults_are_submitted_and_optional_fields_may_stay_empty() {
        let form = json!({"id":"frm_1","sessionID":"ses_A","title":"T","fields":[
            {"key":"h","type":"string","hidden":true,"default":"secret"},
            {"key":"a","type":"string","title":"A","required":false},
            {"key":"b","type":"string","title":"B"}]});
        let record = json!({"run_id": "run", "conversation_id": "ses_A"});
        let cards = cards(&record, &json!([form]), &json!([]));
        assert_eq!(cards[0]["questions"].as_array().unwrap().len(), 2);
        let answers = [
            json!({"question_id":"a","selected_option_ids":[],"text":""}),
            json!({"question_id":"b","selected_option_ids":[],"text":"x"}),
        ];
        assert_eq!(
            form_answer(&cards[0], &form, &answers).unwrap(),
            json!({"answer":{"h":"secret","b":"x"}})
        );
    }
    #[test]
    fn a_card_never_carries_more_than_the_text_bound() {
        let record = json!({"run_id": "run", "conversation_id": "ses_A"});
        let long = "x".repeat(8 * 1024);
        let form = json!({"id":"frm_1","sessionID":"ses_A","title":"T","fields":[
            {"key":"a","type":"string","title":"A","description":long},
            {"key":"b","type":"string","title":"B","description":long},
            {"key":"c","type":"string","title":"C","description":long}]});
        let card = cards(&record, &json!([form]), &json!([])).remove(0);
        assert_eq!(card["can_answer"], false);
        assert_eq!(card["unavailable_reason"], UNAVAILABLE);
        assert!(card["questions"].as_array().unwrap().is_empty());
        assert!(card.to_string().len() < MAX_TEXT);
        let permission = json!({"id":"per_1","sessionID":"ses_A","action":"shell","resources":[long, long, long]});
        let card = cards(&record, &json!([]), &json!([permission])).remove(0);
        assert_eq!(card["can_answer"], false);
        assert!(card["questions"].as_array().unwrap().is_empty());
    }
    #[test]
    fn duplicate_selections_and_null_when_are_handled() {
        let form = json!({"id":"frm_1","sessionID":"ses_A","title":"T","fields":[
            {"key":"k","type":"multiselect","title":"K","when":null,
             "options":[{"value":"a","label":"A"},{"value":"b","label":"B"}]}]});
        let record = json!({"run_id": "run", "conversation_id": "ses_A"});
        let card = cards(&record, &json!([form]), &json!([])).remove(0);
        assert_eq!(card["can_answer"], true);
        let dup = [json!({"question_id":"k","selected_option_ids":["a","a"],"text":""})];
        assert_eq!(
            form_answer(&card, &form, &dup).unwrap_err(),
            "Choose each answer only once"
        );
    }
    #[test]
    fn permission_answers_are_exact() {
        for bad in [
            json!({"question_id":"permission","selected_option_ids":[],"text":""}),
            json!({"question_id":"permission","selected_option_ids":["once","always"],"text":""}),
            json!({"question_id":"permission","selected_option_ids":["maybe"],"text":""}),
            json!({"question_id":"permission","selected_option_ids":["once"],"text":"x"}),
        ] {
            assert!(permission_decision(&[bad]).is_err());
        }
        assert!(permission_decision(&[]).is_err());
    }
}
