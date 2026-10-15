//! Durable wake hints only. Never transmit conversation content to a provider.
use crate::{
    config::read_private,
    db::{now, s},
    error::{Error, Result},
    store::Store,
};
use futures_util::StreamExt;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    path::Path,
    time::{Duration, Instant},
};
use tokio::sync::Mutex;
pub struct Push {
    store: Store,
    client: reqwest::Client,
    fcm: Option<Fcm>,
}
struct Fcm {
    email: String,
    project: String,
    key: ring::signature::RsaKeyPair,
    token: Mutex<Option<(String, Instant)>>,
}
#[derive(Deserialize)]
struct ServiceAccount {
    client_email: String,
    project_id: String,
    private_key: String,
    token_uri: Option<String>,
}
impl Push {
    pub async fn new(store: Store) -> Result<Self> {
        let client = reqwest::Client::builder()
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .timeout(Duration::from_secs(10))
            .connect_timeout(Duration::from_secs(3))
            .build()
            .map_err(|_| Error::BAD)?;
        let fcm = if let Some(path) = &store.db.cfg.fcm_credentials {
            let c: ServiceAccount = serde_json::from_slice(&read_private(Path::new(path), 65536)?)?;
            if c.project_id.is_empty()
                || c.project_id.len() > 128
                || !c
                    .project_id
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b"-_:".contains(&b))
                || c.token_uri
                    .as_deref()
                    .is_some_and(|u| u != "https://oauth2.googleapis.com/token")
            {
                return Err(Error::BAD);
            }
            let mut pem = std::io::Cursor::new(c.private_key.as_bytes());
            let key = match rustls_pemfile::read_one(&mut pem)? {
                Some(rustls_pemfile::Item::Pkcs8Key(k)) => {
                    ring::signature::RsaKeyPair::from_pkcs8(k.secret_pkcs8_der())
                }
                Some(rustls_pemfile::Item::Pkcs1Key(k)) => {
                    ring::signature::RsaKeyPair::from_der(k.secret_pkcs1_der())
                }
                _ => return Err(Error::BAD),
            }
            .map_err(|_| Error::BAD)?;
            Some(Fcm {
                email: c.client_email,
                project: c.project_id,
                key,
                token: Mutex::new(None),
            })
        } else {
            None
        };
        Ok(Self { store, client, fcm })
    }
    pub fn fcm_enabled(&self) -> bool {
        self.fcm.is_some()
    }
    pub fn providers(&self) -> Vec<&str> {
        if self.fcm.is_some() {
            vec!["fcm"]
        } else {
            vec![]
        }
    }
    async fn oauth(&self, fcm: &Fcm) -> Result<String> {
        let mut cache = fcm.token.lock().await;
        if let Some((t, expires)) = &*cache {
            if *expires > Instant::now() {
                return Ok(t.clone());
            }
        }
        #[derive(Serialize)]
        struct Claims<'a> {
            iss: &'a str,
            scope: &'a str,
            aud: &'a str,
            iat: u64,
            exp: u64,
        }
        let at = now() as u64;
        let claims = Claims {
            iss: &fcm.email,
            scope: "https://www.googleapis.com/auth/firebase.messaging",
            aud: "https://oauth2.googleapis.com/token",
            iat: at,
            exp: at + 3600,
        };
        use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
        let message = format!(
            "{}.{}",
            URL_SAFE_NO_PAD.encode(br#"{"alg":"RS256","typ":"JWT"}"#),
            URL_SAFE_NO_PAD.encode(serde_json::to_vec(&claims)?)
        );
        let mut signature = vec![0; fcm.key.public().modulus_len()];
        fcm.key
            .sign(
                &ring::signature::RSA_PKCS1_SHA256,
                &ring::rand::SystemRandom::new(),
                message.as_bytes(),
                &mut signature,
            )
            .map_err(|_| Error::BAD)?;
        let jwt = format!("{message}.{}", URL_SAFE_NO_PAD.encode(signature));
        let response = self
            .client
            .post("https://oauth2.googleapis.com/token")
            .form(&[
                ("grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer"),
                ("assertion", jwt.as_str()),
            ])
            .send()
            .await
            .map_err(|_| Error::UNAVAILABLE)?;
        if !response.status().is_success() {
            return Err(Error::UNAVAILABLE);
        }
        let b = bounded_response(response, 16384).await?;
        let v: Value = serde_json::from_slice(&b)?;
        let token = v["access_token"]
            .as_str()
            .filter(|s| s.len() <= 8192 && !s.is_empty())
            .ok_or(Error::UNAVAILABLE)?
            .to_string();
        let ttl = v["expires_in"].as_u64().unwrap_or(300).clamp(60, 3600) - 30;
        *cache = Some((token.clone(), Instant::now() + Duration::from_secs(ttl)));
        Ok(token)
    }
    async fn deliver(&self, provider: &str, target: &str, payload: &Value) -> Result<(bool, bool)> {
        if provider == "fcm" {
            if let Some(fcm) = &self.fcm {
                let message = fcm_message(target, payload)?;
                let token = self.oauth(fcm).await?;
                let response = self
                    .client
                    .post(format!(
                        "https://fcm.googleapis.com/v1/projects/{}/messages:send",
                        fcm.project
                    ))
                    .bearer_auth(token)
                    .json(&message)
                    .send()
                    .await
                    .map_err(|_| Error::UNAVAILABLE)?;
                if response.status().is_success() {
                    return Ok((true, false));
                }
                if response.status().as_u16() == 401 {
                    *fcm.token.lock().await = None;
                }
                let body = bounded_response(response, 65536).await?;
                let body: Value = serde_json::from_slice(&body)?;
                return Ok((false, fcm_unregistered(&body)));
            }
        }
        // Registrations from retired providers such as UnifiedPush can never deliver.
        Ok((false, provider != "fcm"))
    }
    async fn one(&self, job: Value) -> Result<()> {
        let registration = self.store.push_registration(s(&job, "device_id")).await?;
        let (delivered, invalid) = if let Some(reg) = &registration {
            let payload: Value = serde_json::from_str(s(&job, "payload"))?;
            self.deliver(s(reg, "provider"), s(reg, "target"), &payload)
                .await
                .unwrap_or((false, false))
        } else {
            (false, false)
        };
        self.store
            .finish_push(&job, delivered, invalid, registration.as_ref())
            .await
    }
    pub async fn once(&self) -> Result<()> {
        let jobs = self.store.claim_push_jobs().await?;
        let results = futures_util::future::join_all(jobs.into_iter().map(|j| self.one(j))).await;
        for r in results {
            r?;
        }
        Ok(())
    }
}
/// The FCM v1 message for one wake: string data only, with no notification,
/// so the provider never carries conversation content or display text.
pub fn fcm_message(target: &str, payload: &Value) -> Result<Value> {
    let mut data = serde_json::Map::new();
    for (k, v) in payload.as_object().ok_or(Error::BAD)? {
        data.insert(
            k.clone(),
            json!(v
                .as_str()
                .map(str::to_string)
                .unwrap_or_else(|| v.to_string())),
        );
    }
    Ok(json!({"message":{"token":target,"data":data,"android":{"priority":"high","ttl":"300s"}}}))
}
/// Whether an FCM error response says the registration token is gone.
pub fn fcm_unregistered(body: &Value) -> bool {
    body["error"]["details"].as_array().is_some_and(|a| {
        a.iter().any(|v| {
            v["@type"] == "type.googleapis.com/google.firebase.fcm.v1.FcmError"
                && v["errorCode"] == "UNREGISTERED"
        })
    })
}
async fn bounded_response(r: reqwest::Response, max: usize) -> Result<Vec<u8>> {
    let mut stream = r.bytes_stream();
    let mut b = Vec::new();
    while let Some(c) = stream.next().await {
        let c = c.map_err(|_| Error::UNAVAILABLE)?;
        if b.len() + c.len() > max {
            return Err(Error::LARGE);
        }
        b.extend_from_slice(&c);
    }
    Ok(b)
}
