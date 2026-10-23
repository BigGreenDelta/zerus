//! Authentication precedes ingestion. RAII permits cover CPU work and response
//! lifetime; cancelled blocking work retains its permits until it really exits.
use crate::{
    config::Config,
    error::{Error, Result},
    json,
    protocol::{self, MIB, REQUEST_MAX},
    push::Push,
    store::{
        Credential, EncryptedSubmission, Role, ServiceClass, ServiceLease, Store,
        ENCRYPTED_REQUEST_MAX, ENCRYPTED_SMALL_MAX,
    },
};
use axum::{
    body::{Body, BodyDataStream},
    extract::{ConnectInfo, Request, State},
    http::{header, Method, StatusCode},
    response::{IntoResponse, Response},
    Router,
};
use futures_util::{stream, StreamExt};
use serde_json::{json, Value};
use std::{
    net::{IpAddr, SocketAddr},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};
use tokio::{
    io::AsyncReadExt,
    sync::{OwnedSemaphorePermit, Semaphore},
};
use tokio_util::sync::CancellationToken;

#[derive(Clone)]
pub struct App {
    pub store: Store,
    pub push: Arc<Push>,
    pub cfg: Arc<Config>,
    anonymous: Arc<Semaphore>,
    lookups: Lane,
    authenticated: Lane,
    body_bytes: Lane,
    large: Lane,
    polls: Lane,
    responses: Lane,
    spools: Lane,
    cpu: Lane,
    database: Lane,
    ready: Arc<Mutex<Ready>>,
    pub stop: CancellationToken,
}
struct Ready {
    ok: bool,
    expires: Instant,
    running: bool,
}
#[derive(Default)]
struct Budget {
    permits: Mutex<Vec<ResourcePermit>>,
    class: Mutex<ServiceClass>,
}
fn take(s: &Arc<Semaphore>) -> Result<OwnedSemaphorePermit> {
    s.clone().try_acquire_owned().map_err(|_| Error::BUSY)
}
#[derive(Clone)]
struct Lane {
    name: &'static str,
    total: Arc<Semaphore>,
    standard: Arc<Semaphore>,
}
struct ResourcePermit {
    _local: Vec<OwnedSemaphorePermit>,
    _shared: Option<ServiceLease>,
    class: ServiceClass,
}
impl Lane {
    fn new(name: &'static str, total: usize, cfg: &Config) -> Self {
        Self {
            name,
            total: Arc::new(Semaphore::new(total)),
            standard: Arc::new(Semaphore::new(cfg.standard_capacity(total))),
        }
    }
    fn take(&self, units: usize, class: ServiceClass) -> Result<Vec<OwnedSemaphorePermit>> {
        let units = u32::try_from(units).map_err(|_| Error::BAD)?;
        let mut permits = Vec::with_capacity(2);
        if class == ServiceClass::Standard {
            permits.push(
                self.standard
                    .clone()
                    .try_acquire_many_owned(units)
                    .map_err(|_| Error::BUSY)?,
            );
        }
        permits.push(
            self.total
                .clone()
                .try_acquire_many_owned(units)
                .map_err(|_| Error::BUSY)?,
        );
        Ok(permits)
    }
}
/// Refusals made before `App::body` reads (forwarding, Content-Encoding or
/// Content-Type, authentication, rate limits, request lanes, routing, the
/// handler deadline) answer clients that are unverified, misdirected or
/// already over a limit, so their discard stops here instead of at the
/// route's body limit.
const EARLY_DISCARD: usize = 64 * 1024;
/// A bounded discard lets a client already uploading observe a rejection
/// instead of an unread-body TCP reset. No payload is retained.
/// Expect/continue clients can receive rejection immediately.
async fn discard(request: Request, maximum: usize) {
    if request.headers().contains_key(header::EXPECT) {
        return;
    }
    drain(request.into_body().into_data_stream(), maximum).await;
}
/// Reads and drops about `maximum` more bytes for at most two seconds.
async fn drain(mut stream: BodyDataStream, maximum: usize) {
    let _ = tokio::time::timeout(Duration::from_secs(2), async {
        let mut discarded = 0usize;
        while let Some(Ok(chunk)) = stream.next().await {
            discarded = discarded.saturating_add(chunk.len());
            if discarded > maximum {
                break;
            }
        }
    })
    .await;
}
impl Budget {
    fn class(&self) -> ServiceClass {
        *self.class.lock().expect("budget class")
    }
    fn keep(&self, p: ResourcePermit) {
        self.permits.lock().expect("budget mutex").push(p);
    }
}
struct Poll {
    store: Store,
    id: String,
    _permit: Option<ResourcePermit>,
}
impl Drop for Poll {
    fn drop(&mut self) {
        let store = self.store.clone();
        let id = self.id.clone();
        let permit = self._permit.take();
        let class = permit.as_ref().map_or(ServiceClass::Standard, |p| p.class);
        let tasks = store.db.tasks.clone();
        tasks.spawn(async move {
            let _permit = permit;
            let _ = store.release_poll_reserved(&id, class).await;
        });
    }
}
impl App {
    pub async fn new(store: Store) -> Result<Self> {
        let cfg = store.db.cfg.clone();
        let push = Arc::new(Push::new(store.clone()).await?);
        let sem = |n| Arc::new(Semaphore::new(n));
        let stop = store.db.stop.clone();
        Ok(Self {
            store,
            push,
            anonymous: sem(cfg.max_anonymous_requests),
            lookups: Lane::new("lookup", cfg.max_authenticated_requests, &cfg),
            authenticated: Lane::new("request", cfg.max_authenticated_requests, &cfg),
            body_bytes: Lane::new("body", cfg.max_body_bytes, &cfg),
            large: Lane::new("large", cfg.large_payload_slots, &cfg),
            polls: Lane::new("poll", cfg.max_polls_global, &cfg),
            responses: Lane::new("response", cfg.max_small_responses, &cfg),
            spools: Lane::new("spool", cfg.max_spool_bytes, &cfg),
            cpu: Lane::new("cpu", 4, &cfg),
            database: Lane::new("database", cfg.payload_pool_max() as usize, &cfg),
            cfg,
            ready: Arc::new(Mutex::new(Ready {
                ok: false,
                expires: Instant::now(),
                running: false,
            })),
            stop,
        })
    }
    pub fn router(self) -> Router {
        Router::new().fallback(dispatch).with_state(self)
    }
    async fn prepare_encrypted(
        &self,
        c: &Credential,
        body: Value,
        budget: Arc<Budget>,
    ) -> Result<EncryptedSubmission> {
        let sender = self.store.encrypted_sender(c).await?;
        self.work(budget, move || EncryptedSubmission::new(body, sender))
            .await
    }
    async fn admit(
        &self,
        lane: &Lane,
        units: usize,
        class: ServiceClass,
    ) -> Result<ResourcePermit> {
        let local = lane.take(units, class)?;
        let shared = self.store.acquire_service(lane.name, class, units).await?;
        Ok(ResourcePermit {
            _local: local,
            _shared: shared,
            class,
        })
    }
    async fn work<T: Send + 'static>(
        &self,
        budget: Arc<Budget>,
        f: impl FnOnce() -> Result<T> + Send + 'static,
    ) -> Result<T> {
        let permit = self.admit(&self.cpu, 1, budget.class()).await?;
        tokio::task::spawn_blocking(move || {
            let _keep = (permit, budget);
            f()
        })
        .await
        .map_err(|_| Error::UNAVAILABLE)?
    }
    /// Takes the request out of `unread` only to read or discard its body;
    /// a header refusal leaves it to the smaller discard in `dispatch`.
    async fn body(
        &self,
        unread: &mut Option<Request>,
        budget: Arc<Budget>,
        maximum: usize,
    ) -> Result<Value> {
        let headers = unread.as_ref().map(|r| r.headers()).ok_or(Error::BAD)?;
        if headers
            .get(header::CONTENT_TYPE)
            .and_then(|v| v.to_str().ok())
            .and_then(|s| s.split(';').next())
            != Some("application/json")
        {
            return Err(Error(
                StatusCode::UNSUPPORTED_MEDIA_TYPE,
                "application/json required",
            ));
        }
        let length = headers
            .get(header::CONTENT_LENGTH)
            .map(|v| {
                v.to_str()
                    .ok()
                    .and_then(|s| s.parse::<usize>().ok())
                    .ok_or(Error::BAD)
            })
            .transpose()?;
        let request = unread.take().ok_or(Error::BAD)?;
        if length.is_some_and(|n| n > maximum) {
            discard(request, maximum).await;
            return Err(Error::LARGE);
        }
        let admission = if length.is_none_or(|n| n > MIB) {
            self.admit(&self.large, 1, budget.class()).await
        } else {
            self.admit(&self.body_bytes, length.unwrap_or(0), budget.class())
                .await
        };
        let admission = match admission {
            Ok(permit) => permit,
            Err(error) => {
                discard(request, maximum).await;
                return Err(error);
            }
        };
        let mut raw = Vec::with_capacity(length.unwrap_or(65536).min(maximum));
        let mut body = request.into_body().into_data_stream();
        let deadline =
            tokio::time::Instant::now() + Duration::from_secs(if maximum > MIB { 60 } else { 10 });
        loop {
            let next = tokio::time::timeout_at(deadline, body.next())
                .await
                .map_err(|_| Error::TIMEOUT)?;
            let Some(chunk) = next else {
                break;
            };
            let chunk = chunk.map_err(|_| Error::BAD)?;
            if raw.len().saturating_add(chunk.len()) > maximum {
                // Only an unknown-length stream can cross its limit here; it
                // releases the payload and its large payload lane before the
                // rest is discarded.
                drop((raw, chunk, admission));
                drain(body, maximum).await;
                return Err(Error::LARGE);
            }
            raw.extend_from_slice(&chunk);
        }
        budget.keep(admission);
        let depth = self.cfg.max_json_depth;
        let values = self.cfg.max_json_values;
        self.work(budget, move || json::parse(&raw, depth, values))
            .await
    }
    async fn response(
        &self,
        value: Value,
        status: StatusCode,
        budget: Arc<Budget>,
        maximum: usize,
    ) -> Result<Response> {
        // Reserve disk before encoding. Files are unlinked immediately and dropped
        // on success/disconnect; only the open descriptor owns the spool.
        let reserve = self.admit(&self.spools, maximum, budget.class()).await?;
        budget.keep(reserve);
        let (file, size) = self
            .work(budget.clone(), move || {
                use std::io::{Seek, Write};
                let file = tempfile::tempfile()?;
                let mut w = json::LimitWriter {
                    inner: std::io::BufWriter::new(file),
                    remaining: maximum,
                };
                json::encode(&value, &mut w).map_err(|_| Error::LARGE)?;
                w.flush()?;
                let size = maximum - w.remaining;
                let mut file = w.inner.into_inner().map_err(|_| Error::UNAVAILABLE)?;
                file.rewind()?;
                Ok((file, size))
            })
            .await?;
        let file = tokio::fs::File::from_std(file);
        let stream = stream::unfold(
            (file, budget, Instant::now() + Duration::from_secs(30)),
            |(mut file, guard, deadline)| async move {
                let mut b = vec![0u8; 65536];
                let read = tokio::time::timeout_at(deadline.into(), file.read(&mut b)).await;
                match read {
                    Ok(Ok(0)) => None,
                    Ok(Ok(n)) => {
                        b.truncate(n);
                        Some((
                            Ok::<_, std::io::Error>(bytes::Bytes::from(b)),
                            (file, guard, deadline),
                        ))
                    }
                    _ => Some((
                        Err(std::io::Error::other("response interrupted")),
                        (file, guard, Instant::now()),
                    )),
                }
            },
        );
        let mut r = Response::new(Body::from_stream(stream));
        *r.status_mut() = status;
        r.headers_mut()
            .insert(header::CONTENT_TYPE, "application/json".parse().unwrap());
        r.headers_mut().insert(header::CONTENT_LENGTH, size.into());
        Ok(r)
    }
    async fn authenticate(
        &self,
        authorization: Option<String>,
        role: Role,
        identity: &str,
        retirement: Option<bool>,
    ) -> Result<(Credential, ResourcePermit)> {
        let secret = authorization
            .as_deref()
            .filter(|v| v.len() <= 256)
            .and_then(|v| v.strip_prefix("Bearer "));
        let predicted = secret.map_or(ServiceClass::Standard, |secret| {
            self.store.db.auth_class(secret)
        });
        let lookup = self.lookups.take(1, predicted)?;
        let c = if let Some(secret) = secret {
            match self.store.authenticate_class(secret, role).await {
                Err(e) if retirement.is_some() && e.0 == StatusCode::UNAUTHORIZED => {
                    if retirement == Some(true) {
                        self.store.retirement_credential(secret).await
                    } else {
                        self.store.legacy_retirement_credential(secret).await
                    }
                }
                result => result,
            }
        } else {
            Err(Error::UNAUTHORIZED)
        };
        match c {
            Ok((c, class)) => {
                let admission = self.admit(&self.authenticated, 1, class).await?;
                drop(lookup);
                let _database = self.admit(&self.database, 1, class).await?;
                self.store
                    .rate(&json!([role.table(), c.workspace, c.id]), 2400, false)
                    .await?;
                Ok((c, admission))
            }
            Err(e) => {
                if e.0 == StatusCode::UNAUTHORIZED {
                    let _database = self
                        .admit(&self.database, 1, ServiceClass::Standard)
                        .await?;
                    self.store
                        .rate(&json!(["login", identity]), 20, true)
                        .await?;
                }
                Err(e)
            }
        }
    }
    async fn readiness(&self) -> bool {
        let probe = {
            let mut r = self.ready.lock().expect("readiness");
            if !r.running && Instant::now() >= r.expires {
                r.running = true;
                true
            } else {
                false
            }
        };
        if probe {
            let app = self.clone();
            let task = self.store.db.tasks.spawn(async move {
                let ok = tokio::time::timeout(
                    Duration::from_secs(2),
                    app.store.db.one("SELECT 1 AS ok", &[]),
                )
                .await
                .is_ok_and(|r| r.is_ok());
                let mut r = app.ready.lock().expect("readiness");
                r.ok = ok;
                r.expires = Instant::now() + Duration::from_secs(1);
                r.running = false;
            });
            let _ = task.await;
        }
        self.ready.lock().expect("readiness").ok
    }
    async fn poll(
        &self,
        c: &Credential,
        wait: f64,
        admission: ResourcePermit,
    ) -> Result<Option<Poll>> {
        if wait == 0.0 {
            drop(admission);
            return Ok(None);
        }
        let class = admission.class;
        let permit = self.admit(&self.polls, 1, class).await?;
        let database = self.admit(&self.database, 1, class).await?;
        // Ownership transfers into the acquisition task before COMMIT. Dropping
        // the awaiting HTTP future still drops/releases the eventual lease.
        let store = self.store.clone();
        let c = c.clone();
        let tracker = self.store.db.tasks.clone();
        let task = tracker.spawn(async move {
            let id = store.acquire_poll(&c, wait + 10.0).await?;
            drop((admission, database));
            Ok::<_, Error>(Poll {
                store,
                id,
                _permit: Some(permit),
            })
        });
        Ok(Some(task.await.map_err(|_| Error::UNAVAILABLE)??))
    }
    async fn wait(
        &self,
        rx: &mut tokio::sync::broadcast::Receiver<String>,
        topic: &str,
        deadline: Instant,
    ) -> Result<()> {
        let timeout = deadline.min(Instant::now() + Duration::from_secs(5));
        loop {
            tokio::select! {biased;_=self.stop.cancelled()=>return Err(Error::UNAVAILABLE),_=tokio::time::sleep_until(timeout.into())=>return Ok(()),r=rx.recv()=>match r{Ok(s) if !s.is_empty()&&s!=topic=>continue,_=>return Ok(())}}
        }
    }
    /// `unread` keeps the request until `App::body` takes it, so `dispatch`
    /// can still discard the body after an earlier refusal.
    async fn handle(&self, unread: &mut Option<Request>) -> Result<Response> {
        let req = unread.as_ref().ok_or(Error::BAD)?;
        let path = req.uri().path().to_string();
        let method = req.method().clone();
        // A request body is not `Sync`, so no borrow of the request may be
        // held across an await.
        let query = req.uri().query().unwrap_or("").to_string();
        if path == "/healthz" && method == Method::GET {
            return Ok(axum::Json(json!({"ok":true,"protocol_version":1})).into_response());
        }
        if path == "/readyz" && method == Method::GET {
            let ok = self.readiness().await;
            return Ok((
                if ok {
                    StatusCode::OK
                } else {
                    StatusCode::SERVICE_UNAVAILABLE
                },
                axum::Json(json!({"ok":ok})),
            )
                .into_response());
        }
        let peer = req
            .extensions()
            .get::<ConnectInfo<SocketAddr>>()
            .map(|v| v.0.ip())
            .unwrap_or(IpAddr::from([127, 0, 0, 1]));
        let identity = client_identity(
            peer,
            req.headers()
                .get("x-forwarded-for")
                .and_then(|v| v.to_str().ok()),
            &self.cfg,
        )?;
        if req
            .headers()
            .get(header::CONTENT_ENCODING)
            .is_some_and(|v| v != "identity")
        {
            return Err(Error(
                StatusCode::UNSUPPORTED_MEDIA_TYPE,
                "compressed requests are unsupported",
            ));
        }
        let budget = Arc::new(Budget::default());
        if path.starts_with("/v2/") && !self.cfg.public_enrollment {
            return Err(Error::NOT_FOUND);
        }
        if path == "/v2/capabilities" && method == Method::GET {
            let _lane = take(&self.anonymous)?;
            let mut value = protocol::capabilities(self.push.providers());
            value["encrypted_transport"] = json!({"version":1,"membership":"signed-linear-v1","public_enrollment":true,"request_max_bytes":ENCRYPTED_REQUEST_MAX,"result_max_bytes":ENCRYPTED_SMALL_MAX});
            return Ok(axum::Json(value).into_response());
        }
        if (path == "/v2/workspaces" || path == "/v2/enrollments") && method == Method::POST {
            let _lane = take(&self.anonymous)?;
            budget.keep(
                self.admit(&self.database, 1, ServiceClass::Standard)
                    .await?,
            );
            self.store
                .rate(&json!(["public-enrollment", identity]), 10, true)
                .await?;
            budget.keep(
                self.admit(&self.responses, 1, ServiceClass::Standard)
                    .await?,
            );
            let body = self.body(unread, budget.clone(), 64 * 1024).await?;
            let value = if path == "/v2/workspaces" {
                self.store.create_workspace(&body).await?
            } else {
                self.store.join_workspace(&body).await?
            };
            return self.response(value, StatusCode::OK, budget, MIB).await;
        }
        if path.starts_with("/v2/enrollments/") && method == Method::GET {
            let _lane = take(&self.anonymous)?;
            let id = path.strip_prefix("/v2/enrollments/").ok_or(Error::BAD)?;
            protocol::uuid(&json!(id), false)?;
            let secret = req
                .headers()
                .get(header::AUTHORIZATION)
                .and_then(|v| v.to_str().ok())
                .filter(|v| v.len() <= 256)
                .and_then(|v| v.strip_prefix("Bearer "))
                .ok_or(Error::UNAUTHORIZED)?
                .to_owned();
            budget.keep(
                self.admit(&self.database, 1, ServiceClass::Standard)
                    .await?,
            );
            self.store
                .rate(&json!(["enrollment-status", identity]), 120, true)
                .await?;
            let value = self.store.enrollment_status(id, &secret).await?;
            budget.keep(
                self.admit(&self.responses, 1, ServiceClass::Standard)
                    .await?,
            );
            return self.response(value, StatusCode::OK, budget, MIB).await;
        }
        if path == "/v1/pair" && method == Method::POST {
            let _lane = take(&self.anonymous)?;
            budget.keep(
                self.admit(&self.database, 1, ServiceClass::Standard)
                    .await?,
            );
            self.store
                .rate(&json!(["pair", identity]), 10, true)
                .await?;
            budget.keep(self.admit(&self.responses, 1, budget.class()).await?);
            let b = self.body(unread, budget.clone(), MIB).await?;
            protocol::fields(&b, &["code", "device_name"], &[])?;
            let v = self
                .store
                .pair(
                    protocol::text(&b["code"], 128, false)?,
                    protocol::text(&b["device_name"], 128, false)?,
                )
                .await?;
            return self.response(v, StatusCode::OK, budget, MIB).await;
        }
        let role = if path.starts_with("/v1/node/") || path.starts_with("/v2/node/") {
            Role::Node
        } else {
            Role::Device
        };
        let (c, admission) = self
            .authenticate(
                req.headers()
                    .get(header::AUTHORIZATION)
                    .and_then(|v| v.to_str().ok())
                    .map(str::to_owned),
                role,
                &identity,
                if (path == "/v2/node/self" && method == Method::DELETE)
                    || (path == "/v2/node/membership" && method == Method::GET)
                {
                    Some(true)
                } else if path == "/v1/node/self" && method == Method::DELETE {
                    Some(false)
                } else {
                    None
                },
            )
            .await?;
        *budget.class.lock().expect("budget class") = admission.class;
        let pre_database = self.admit(&self.database, 1, budget.class()).await?;
        let encrypted = self.store.encrypted_credential(&c).await?;
        if path.starts_with("/v2/") && !encrypted {
            return Err(Error::CONFLICT);
        }
        if path.starts_with("/v1/")
            && encrypted
            && !["/v1/capabilities", "/v1/push", "/v1/node/devices"].contains(&path.as_str())
        {
            return Err(Error::CONFLICT);
        }
        let mut params = std::collections::HashMap::new();
        for (k, v) in url::form_urlencoded::parse(query.as_bytes()) {
            if params.insert(k.into_owned(), v.into_owned()).is_some() {
                return Err(Error::BAD);
            }
        }
        let parts: Vec<_> = path.trim_matches('/').split('/').collect();
        if (path == "/v2/events" || path == "/v2/node/requests") && method == Method::GET {
            let wait = params
                .get("wait")
                .map_or(Ok(0.0), |s| s.parse::<f64>().map_err(|_| Error::BAD))?;
            let after = params
                .get("after")
                .map_or(Ok(0), |s| s.parse::<i64>().map_err(|_| Error::BAD))?;
            if !wait.is_finite() || !(0.0..=25.0).contains(&wait) || after < 0 {
                return Err(Error::BAD);
            }
            drop(pre_database);
            let _lease = self.poll(&c, wait, admission).await?;
            let deadline = Instant::now() + Duration::from_secs_f64(wait);
            let topic = if role == Role::Node {
                format!("node:{}", c.id)
            } else {
                format!("workspace:{}", c.workspace)
            };
            let mut rx = self.store.db.wake.subscribe();
            loop {
                let database = self.admit(&self.database, 1, budget.class()).await?;
                self.store.authorized(&c).await?;
                if role == Role::Node {
                    if self.store.encrypted_pending(&c).await? {
                        budget.keep(self.admit(&self.large, 1, budget.class()).await?);
                        let raw = self.store.encrypted_claim_body(&c).await?;
                        let value = self
                            .work(budget.clone(), move || {
                                Ok(match raw {
                                    Some(body) => {
                                        json!({"requests":[json::parse(body.as_bytes(),32,50000)?]})
                                    }
                                    None => json!({"requests":[]}),
                                })
                            })
                            .await?;
                        return self
                            .response(value, StatusCode::OK, budget, ENCRYPTED_REQUEST_MAX)
                            .await;
                    }
                    if Instant::now() >= deadline {
                        return self
                            .response(json!({"requests":[]}), StatusCode::OK, budget, MIB)
                            .await;
                    }
                } else {
                    let value = self.store.encrypted_events(&c, after).await?;
                    if value["events"].as_array().is_some_and(|v| !v.is_empty())
                        || Instant::now() >= deadline
                    {
                        budget.keep(self.admit(&self.responses, 1, budget.class()).await?);
                        return self.response(value, StatusCode::OK, budget, MIB).await;
                    }
                }
                drop(database);
                self.wait(&mut rx, &topic, deadline).await?;
            }
        }
        if (path == "/v1/events" || path == "/v1/node/requests") && method == Method::GET {
            let wait = params
                .get("wait")
                .map_or(Ok(0.0), |s| s.parse::<f64>().map_err(|_| Error::BAD))?;
            if !wait.is_finite() || !(0.0..=25.0).contains(&wait) {
                return Err(Error::BAD);
            }
            let after = params
                .get("after")
                .map_or(Ok(0), |s| s.parse::<i64>().map_err(|_| Error::BAD))?;
            if after < 0 {
                return Err(Error::BAD);
            }
            let allow = match params
                .get("gateway_one_hop")
                .map(String::as_str)
                .unwrap_or("0")
            {
                "0" => false,
                "1" => true,
                _ => return Err(Error::BAD),
            };
            drop(pre_database);
            let _lease = self.poll(&c, wait, admission).await?;
            let deadline = Instant::now() + Duration::from_secs_f64(wait);
            let topic = if role == Role::Node {
                format!("node:{}", c.id)
            } else {
                format!("workspace:{}", c.workspace)
            };
            let mut rx = self.store.db.wake.subscribe();
            loop {
                let database = self.admit(&self.database, 1, budget.class()).await?;
                self.store.authorized(&c).await?;
                if role == Role::Node {
                    if self.store.pending(&c, allow).await? {
                        let lane = self.admit(&self.large, 1, budget.class()).await?;
                        if let Some(claim) = self.store.claim(&c, allow).await? {
                            budget.keep(lane);
                            let b = self.work(budget.clone(), move || claim.decode()).await?;
                            return self.response(b, StatusCode::OK, budget, 32 * MIB).await;
                        }
                    }
                    if Instant::now() >= deadline {
                        return self
                            .response(json!({"requests":[]}), StatusCode::OK, budget, MIB)
                            .await;
                    }
                } else {
                    let lane = self.admit(&self.responses, 1, budget.class()).await?;
                    let events = self.store.events(&c, after).await?;
                    if !events["events"].as_array().ok_or(Error::BAD)?.is_empty()
                        || Instant::now() >= deadline
                    {
                        budget.keep(lane);
                        return self.response(events, StatusCode::OK, budget, MIB).await;
                    }
                }
                drop(database);
                self.wait(&mut rx, &topic, deadline).await?;
            }
        }
        budget.keep(admission);
        budget.keep(pre_database);
        budget.keep(self.admit(&self.responses, 1, budget.class()).await?);
        if path.starts_with("/v2/") && path != "/v2/push" {
            let page_limit = params
                .get("limit")
                .map_or(Ok(100), |v| v.parse::<usize>().map_err(|_| Error::BAD))?;
            if !(1..=100).contains(&page_limit) {
                return Err(Error::BAD);
            }
            let after = params.get("after").map(String::as_str).unwrap_or("");
            let (status, value, maximum) = match (method.as_str(), parts.as_slice()) {
                ("GET", ["v2", "computers"]) => (
                    StatusCode::OK,
                    self.store
                        .encrypted_computers(&c, after, page_limit)
                        .await?,
                    3 * MIB,
                ),
                ("POST", ["v2", "requests"]) => {
                    let body = self
                        .body(unread, budget.clone(), ENCRYPTED_REQUEST_MAX)
                        .await?;
                    let (status, value) = self
                        .store
                        .encrypted_submit(
                            &c,
                            &self.prepare_encrypted(&c, body, budget.clone()).await?,
                        )
                        .await?;
                    (status, value, ENCRYPTED_SMALL_MAX)
                }
                ("GET", ["v2", "requests", id]) => (
                    StatusCode::OK,
                    self.store.encrypted_receipt(&c, id).await?,
                    ENCRYPTED_SMALL_MAX,
                ),
                ("POST", ["v2", "node", "requests", id, "result"]) => {
                    let body = self
                        .body(unread, budget.clone(), ENCRYPTED_SMALL_MAX)
                        .await?;
                    self.store
                        .encrypted_result(
                            &c,
                            id,
                            &self.prepare_encrypted(&c, body, budget.clone()).await?,
                        )
                        .await?;
                    (StatusCode::OK, json!({"ok":true}), MIB)
                }
                ("POST", ["v2", "node", "snapshot"]) => {
                    let body = self
                        .body(unread, budget.clone(), ENCRYPTED_SMALL_MAX)
                        .await?;
                    self.store
                        .encrypted_snapshot(
                            &c,
                            &self.prepare_encrypted(&c, body, budget.clone()).await?,
                        )
                        .await?;
                    (StatusCode::OK, json!({"ok":true}), MIB)
                }
                ("GET", ["v2", "node", "routes"]) => {
                    (StatusCode::OK, self.store.encrypted_routes(&c).await?, MIB)
                }
                ("POST", ["v2", "node", "routes"]) => {
                    let body = self.body(unread, budget.clone(), MIB).await?;
                    self.store.encrypted_routes_manifest(&c, &body).await?;
                    (StatusCode::OK, json!({"ok":true}), MIB)
                }
                ("DELETE", ["v2", "node", "routes", route]) => {
                    self.store.withdraw_encrypted_route(&c, route).await?;
                    (StatusCode::OK, json!({"ok":true}), MIB)
                }
                ("POST", ["v2", "node", "signals"]) => {
                    let body = self.body(unread, budget.clone(), MIB).await?;
                    self.store.encrypted_signals(&c, &body).await?;
                    (StatusCode::OK, json!({"ok":true}), MIB)
                }
                ("POST", ["v2", "node", "invitations"]) => {
                    self.store
                        .rate(&json!(["computer-invite", &c.id]), 10, false)
                        .await?;
                    let body = self.body(unread, budget.clone(), MIB).await?;
                    (
                        StatusCode::OK,
                        self.store.computer_invitation(&c, &body).await?,
                        MIB,
                    )
                }
                ("GET", ["v2", "node", "enrollments"]) => (
                    StatusCode::OK,
                    self.store
                        .pending_enrollments(&c, after, page_limit)
                        .await?,
                    MIB,
                ),
                ("POST", ["v2", "node", "enrollments", id, "approve"]) => {
                    let body = self.body(unread, budget.clone(), 64 * 1024).await?;
                    (
                        StatusCode::OK,
                        self.store.approve_enrollment(&c, id, &body).await?,
                        MIB,
                    )
                }
                ("GET", ["v2", "membership"]) | ("GET", ["v2", "node", "membership"]) => {
                    let after = if after.is_empty() {
                        -1
                    } else {
                        after.parse::<i64>().map_err(|_| Error::BAD)?
                    };
                    if after < -1 {
                        return Err(Error::BAD);
                    }
                    (
                        StatusCode::OK,
                        self.store.membership_changes(&c, after, page_limit).await?,
                        MIB,
                    )
                }
                ("POST", ["v2", "node", "membership"]) | ("DELETE", ["v2", "node", "self"]) => {
                    let body = self.body(unread, budget.clone(), 64 * 1024).await?;
                    (
                        StatusCode::OK,
                        self.store
                            .revoke_membership(&c, &body, method == Method::DELETE)
                            .await?,
                        MIB,
                    )
                }
                _ => return Err(Error::NOT_FOUND),
            };
            return self.response(value, status, budget, maximum).await;
        }
        let (status, value) = match (method.as_str(), parts.as_slice()) {
            ("DELETE", ["v1", "node", "self"]) => {
                let body = self.body(unread, budget.clone(), 64 * 1024).await?;
                (
                    StatusCode::OK,
                    self.store.retire_legacy_node(&c, &body).await?,
                )
            }
            ("GET", ["v1", "capabilities"]) => (
                StatusCode::OK,
                protocol::capabilities(self.push.providers()),
            ),
            ("GET", ["v1", "computers"]) => (StatusCode::OK, self.store.computers(&c).await?),
            ("POST", ["v1", "requests"]) => {
                let b = self.body(unread, budget.clone(), REQUEST_MAX).await?;
                let submission = self
                    .work(budget.clone(), move || crate::store::Submission::new(b))
                    .await?;
                self.store.submit(&c, &submission).await?
            }
            ("GET", ["v1", "requests", id]) => (StatusCode::OK, self.store.receipt(&c, id).await?),
            ("POST", ["v1", "node", "heartbeat"]) => {
                let b = self.body(unread, budget.clone(), MIB).await?;
                let (b, encoded) = self
                    .work(budget.clone(), move || {
                        protocol::heartbeat(&b)?;
                        let encoded = json::canonical(&b["snapshot"])?;
                        Ok((b, encoded))
                    })
                    .await?;
                self.store.heartbeat(&c, &b, &encoded).await?;
                (StatusCode::OK, json!({"ok":true}))
            }
            ("POST", ["v1", "node", "peers", route, "heartbeat"]) => {
                protocol::uuid(&json!(route), false)?;
                let b = self.body(unread, budget.clone(), MIB).await?;
                let (b, encoded) = self
                    .work(budget.clone(), move || {
                        protocol::peer_heartbeat(&b)?;
                        let e = json::canonical(&b["snapshot"])?;
                        Ok((b, e))
                    })
                    .await?;
                self.store.peer_heartbeat(&c, route, &b, &encoded).await?;
                (StatusCode::OK, json!({"ok":true}))
            }
            ("POST", ["v1", "node", "requests", id, "result"]) => {
                let b = self.body(unread, budget.clone(), MIB).await?;
                let id_owned = id.to_string();
                let (b, encoded) = self
                    .work(budget.clone(), move || {
                        protocol::result(&b, &id_owned)?;
                        let e = if b["result"].is_null() {
                            None
                        } else {
                            Some(json::canonical(&b["result"])?)
                        };
                        Ok((b, e))
                    })
                    .await?;
                self.store.result(&c, id, &b, encoded.as_deref()).await?;
                (StatusCode::OK, json!({"ok":true}))
            }
            ("POST", ["v1", "node", "invitations"]) => {
                let b = self.body(unread, budget.clone(), MIB).await?;
                protocol::fields(&b, &[], &["workspace_id"])?;
                // The desktop names the workspace it listed: a gateway moved to
                // another workspace since then must not issue that one's code.
                if !b["workspace_id"].is_null() {
                    pinned(&c, Some(protocol::text(&b["workspace_id"], 64, false)?))?;
                }
                self.store
                    .rate(&json!(["node-invite", &c.id]), 10, false)
                    .await?;
                (StatusCode::OK, self.store.invite_from(&c).await?)
            }
            ("GET", ["v1", "node", "devices"]) => (StatusCode::OK, self.store.devices(&c).await?),
            ("DELETE", ["v1", "node", "devices", id]) => {
                pinned(&c, params.get("workspace_id").map(String::as_str))?;
                self.store.revoke_device(&c, id).await?;
                (StatusCode::OK, json!({"revoked":true}))
            }
            ("POST", ["v1", "push"]) | ("POST", ["v2", "push"]) => {
                let b = self.body(unread, budget.clone(), MIB).await?;
                let provider = b["provider"].as_str().ok_or(Error::BAD)?;
                let target = match provider {
                    "fcm" => {
                        if !self.push.fcm_enabled() {
                            return Err(Error::UNAVAILABLE);
                        }
                        protocol::fields(&b, &["provider", "token"], &[])?;
                        protocol::text(&b["token"], 4096, false)?
                    }
                    "apns" | "apns_sandbox" => {
                        if !self.push.apns_enabled() {
                            return Err(Error::UNAVAILABLE);
                        }
                        protocol::fields(&b, &["provider", "token"], &[])?;
                        &crate::push::apns_token(protocol::text(&b["token"], 200, false)?)
                            .ok_or(Error::BAD)?
                    }
                    _ => return Err(Error::BAD),
                };
                self.store.register_push(&c, provider, target).await?;
                (StatusCode::OK, json!({"ok":true}))
            }
            ("DELETE", ["v1", "push"]) | ("DELETE", ["v2", "push"]) => {
                self.store.delete_push(&c).await?;
                (StatusCode::OK, json!({"ok":true}))
            }
            ("DELETE", ["v1", "device"]) => {
                self.store.revoke(Role::Device, &c.id).await?;
                (StatusCode::OK, json!({"ok":true}))
            }
            _ => return Err(Error::NOT_FOUND),
        };
        self.response(value, status, budget, MIB).await
    }
    pub fn start_background(&self) {
        if !self.cfg.background {
            return;
        }
        let app = self.clone();
        self.store.db.tasks.spawn(async move {
            loop {
                tokio::select!{biased;
                    _=app.stop.cancelled()=>break,
                    _=tokio::time::sleep(Duration::from_secs_f64(app.cfg.maintenance_interval))=>{
                        crate::db::ordinary_control(async {
                            if app.store.maintain(false).await.is_err(){eprintln!("relay maintenance deferred");}
                            if app.cfg.public_enrollment {
                                if app.store.maintain_encrypted().await.is_err(){eprintln!("encrypted relay maintenance deferred");}
                                if app.store.maintain_opaque_signals().await.is_err(){eprintln!("encrypted signal maintenance deferred");}
                                if app.store.maintain_enrollment().await.is_err(){eprintln!("public enrollment maintenance deferred");}
                            }
                        }).await;
                    }
                }
            }
        });
        let app = self.clone();
        self.store.db.tasks.spawn(async move {
            loop {
                tokio::select!{biased;
                    _=app.stop.cancelled()=>break,
                    _=tokio::time::sleep(Duration::from_secs(1))=>{
                        if crate::db::ordinary_control(app.push.once()).await.is_err(){eprintln!("relay push delivery deferred");}
                    }
                }
            }
        });
    }
}
/// A computer's phone management names the workspace it showed; a mismatch
/// means its credential now belongs to another one.
fn pinned(c: &crate::store::Credential, workspace: Option<&str>) -> Result<()> {
    match workspace {
        Some(w) if w != c.workspace => Err(Error::CONFLICT),
        _ => Ok(()),
    }
}
async fn dispatch(State(app): State<App>, req: Request) -> Response {
    let deadline = if req.method() == Method::POST
        && ["/v1/requests", "/v2/requests"].contains(&req.uri().path())
    {
        75
    } else {
        40
    };
    let mut unread = Some(req);
    let outcome =
        tokio::time::timeout(Duration::from_secs(deadline), app.handle(&mut unread)).await;
    let mut response = match outcome {
        Ok(Ok(r)) => r,
        Ok(Err(e)) => refuse(e, unread).await,
        Err(_) => refuse(Error::TIMEOUT, unread).await,
    };
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, "no-store".parse().unwrap());
    response
        .headers_mut()
        .insert(header::X_CONTENT_TYPE_OPTIONS, "nosniff".parse().unwrap());
    response
}
/// A request still unread was refused before `App::body` took it. Its small
/// discard runs after `handle` has released every admission permit.
async fn refuse(error: Error, unread: Option<Request>) -> Response {
    if let Some(request) = unread {
        discard(request, EARLY_DISCARD).await;
    }
    error.into_response()
}
pub fn client_identity(peer: IpAddr, forwarded: Option<&str>, cfg: &Config) -> Result<String> {
    let trusted = |ip: &IpAddr| cfg.trusted_proxy_cidrs.iter().any(|net| net.contains(ip));
    if !trusted(&peer) {
        return Ok(peer.to_string());
    }
    let Some(chain) = forwarded else {
        return Ok(peer.to_string());
    };
    let hops: Vec<_> = chain.split(',').collect();
    if hops.len() > cfg.max_forwarded_hops {
        return Err(Error::BAD);
    }
    let mut addresses = Vec::with_capacity(hops.len());
    for s in hops {
        addresses.push(s.trim().parse::<IpAddr>().map_err(|_| Error::BAD)?);
    }
    let mut current = peer;
    for ip in addresses.into_iter().rev() {
        if !trusted(&current) {
            break;
        }
        current = ip;
    }
    Ok(current.to_string())
}
/// Bound connections independently of authenticated requests, slow headers and
/// idle keepalive. Hyper cancels handlers when a peer disconnects.
pub async fn serve(
    listener: tokio::net::TcpListener,
    app: App,
    shutdown: CancellationToken,
) -> Result<()> {
    use hyper_util::{
        rt::{TokioIo, TokioTimer},
        service::TowerToHyperService,
    };
    let connections = Arc::new(Semaphore::new(app.cfg.max_connections));
    let tasks = tokio_util::task::TaskTracker::new();
    let router = app.clone().router();
    loop {
        tokio::select! {biased;_=shutdown.cancelled()=>break,accept=listener.accept()=>{
            let (stream,addr)=accept?;let Ok(permit)=connections.clone().try_acquire_owned()else{drop(stream);continue;};stream.set_nodelay(true)?;
            let service=router.clone().layer(axum::Extension(ConnectInfo(addr)));let stop=shutdown.clone();
            tasks.spawn(async move{let _permit=permit;let mut builder=hyper::server::conn::http1::Builder::new();builder.timer(TokioTimer::new()).header_read_timeout(Duration::from_secs(10)).max_headers(100).max_buf_size(32768);
                let connection=builder.serve_connection(TokioIo::new(stream),TowerToHyperService::new(service));tokio::pin!(connection);
                tokio::select!{_= &mut connection=>{},_=tokio::time::sleep(Duration::from_secs(120))=>{},_=stop.cancelled()=>{connection.as_mut().graceful_shutdown();let _=tokio::time::timeout(Duration::from_secs(30),&mut connection).await;}}
            });
        }}
    }
    app.stop.cancel();
    tasks.close();
    let _ = tokio::time::timeout(Duration::from_secs(app.cfg.shutdown_timeout), tasks.wait()).await;
    Ok(())
}
