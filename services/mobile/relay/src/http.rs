//! Authentication precedes ingestion. RAII permits cover CPU work and response
//! lifetime; cancelled blocking work retains its permits until it really exits.
use crate::{
    config::Config,
    error::{Error, Result},
    json,
    protocol::{self, MIB, REQUEST_MAX},
    push::Push,
    store::{Credential, Role, Store},
};
use axum::{
    body::Body,
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
    lookups: Arc<Semaphore>,
    authenticated: Arc<Semaphore>,
    body_bytes: Arc<Semaphore>,
    large: Arc<Semaphore>,
    polls: Arc<Semaphore>,
    responses: Arc<Semaphore>,
    spools: Arc<Semaphore>,
    cpu: Arc<Semaphore>,
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
    permits: Mutex<Vec<OwnedSemaphorePermit>>,
}
fn take(s: &Arc<Semaphore>) -> Result<OwnedSemaphorePermit> {
    s.clone().try_acquire_owned().map_err(|_| Error::BUSY)
}
impl Budget {
    fn keep(&self, p: OwnedSemaphorePermit) {
        self.permits.lock().expect("budget mutex").push(p);
    }
}
struct Poll {
    store: Store,
    id: String,
    _permit: Option<OwnedSemaphorePermit>,
}
impl Drop for Poll {
    fn drop(&mut self) {
        let store = self.store.clone();
        let id = self.id.clone();
        let permit = self._permit.take();
        let tasks = store.db.tasks.clone();
        tasks.spawn(async move {
            let _permit = permit;
            let _ = store.release_poll(&id).await;
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
            lookups: sem(cfg.max_authenticated_requests),
            authenticated: sem(cfg.max_authenticated_requests),
            body_bytes: sem(cfg.max_body_bytes),
            large: sem(cfg.large_payload_slots),
            polls: sem(cfg.max_polls_global),
            responses: sem(cfg.max_small_responses),
            spools: sem(64 * MIB),
            cpu: sem(4),
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
    async fn work<T: Send + 'static>(
        &self,
        budget: Arc<Budget>,
        f: impl FnOnce() -> Result<T> + Send + 'static,
    ) -> Result<T> {
        let permit = take(&self.cpu)?;
        tokio::task::spawn_blocking(move || {
            let _keep = (permit, budget);
            f()
        })
        .await
        .map_err(|_| Error::UNAVAILABLE)?
    }
    async fn body(&self, request: Request, budget: Arc<Budget>, maximum: usize) -> Result<Value> {
        if request
            .headers()
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
        let length = request
            .headers()
            .get(header::CONTENT_LENGTH)
            .map(|v| {
                v.to_str()
                    .ok()
                    .and_then(|s| s.parse::<usize>().ok())
                    .ok_or(Error::BAD)
            })
            .transpose()?;
        if length.is_some_and(|n| n > maximum) {
            return Err(Error::LARGE);
        }
        let admission = if length.is_none_or(|n| n > MIB) {
            take(&self.large)
        } else {
            self.body_bytes
                .clone()
                .try_acquire_many_owned(length.unwrap_or(0) as u32)
                .map_err(|_| Error::BUSY)
        };
        match admission {
            Ok(permit) => budget.keep(permit),
            Err(error) => {
                // A bounded discard lets a client already uploading observe 429
                // instead of an unread-body TCP reset. No payload is retained.
                // Expect/continue clients can receive rejection immediately.
                if !request.headers().contains_key(header::EXPECT) {
                    let mut stream = request.into_body().into_data_stream();
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
                return Err(error);
            }
        }
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
                return Err(Error::LARGE);
            }
            raw.extend_from_slice(&chunk);
        }
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
        let reserve = self
            .spools
            .clone()
            .try_acquire_many_owned(maximum as u32)
            .map_err(|_| Error::BUSY)?;
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
    ) -> Result<(Credential, OwnedSemaphorePermit)> {
        let lookup = take(&self.lookups)?;
        let secret = authorization
            .as_deref()
            .filter(|v| v.len() <= 256)
            .and_then(|v| v.strip_prefix("Bearer "));
        let c = if let Some(secret) = secret {
            self.store.authenticate(secret, role).await
        } else {
            Err(Error::UNAUTHORIZED)
        };
        match c {
            Ok(c) => {
                let admission = take(&self.authenticated)?;
                drop(lookup);
                self.store
                    .rate(&json!([role.table(), c.workspace, c.id]), 2400, false)
                    .await?;
                Ok((c, admission))
            }
            Err(e) => {
                if e.0 == StatusCode::UNAUTHORIZED {
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
        admission: OwnedSemaphorePermit,
    ) -> Result<Option<Poll>> {
        if wait == 0.0 {
            drop(admission);
            return Ok(None);
        }
        let permit = take(&self.polls)?;
        // Ownership transfers into the acquisition task before COMMIT. Dropping
        // the awaiting HTTP future still drops/releases the eventual lease.
        let store = self.store.clone();
        let c = c.clone();
        let tracker = self.store.db.tasks.clone();
        let task = tracker.spawn(async move {
            let id = store.acquire_poll(&c, wait + 10.0).await?;
            drop(admission);
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
    async fn handle(&self, req: Request) -> Result<Response> {
        let path = req.uri().path().to_string();
        let method = req.method().clone();
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
        if path == "/v1/pair" && method == Method::POST {
            let _lane = take(&self.anonymous)?;
            self.store
                .rate(&json!(["pair", identity]), 10, true)
                .await?;
            budget.keep(take(&self.responses)?);
            let b = self.body(req, budget.clone(), MIB).await?;
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
        let role = if path.starts_with("/v1/node/") {
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
            )
            .await?;
        let mut params = std::collections::HashMap::new();
        for (k, v) in url::form_urlencoded::parse(req.uri().query().unwrap_or("").as_bytes()) {
            if params.insert(k.into_owned(), v.into_owned()).is_some() {
                return Err(Error::BAD);
            }
        }
        let parts: Vec<_> = path.trim_matches('/').split('/').collect();
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
            let _lease = self.poll(&c, wait, admission).await?;
            let deadline = Instant::now() + Duration::from_secs_f64(wait);
            let topic = if role == Role::Node {
                format!("node:{}", c.id)
            } else {
                format!("workspace:{}", c.workspace)
            };
            let mut rx = self.store.db.wake.subscribe();
            loop {
                self.store.authorized(&c).await?;
                if role == Role::Node {
                    if self.store.pending(&c, allow).await? {
                        let lane = take(&self.large)?;
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
                    let lane = take(&self.responses)?;
                    let events = self.store.events(&c, after).await?;
                    if !events["events"].as_array().ok_or(Error::BAD)?.is_empty()
                        || Instant::now() >= deadline
                    {
                        budget.keep(lane);
                        return self.response(events, StatusCode::OK, budget, MIB).await;
                    }
                }
                self.wait(&mut rx, &topic, deadline).await?;
            }
        }
        budget.keep(admission);
        budget.keep(take(&self.responses)?);
        let (status, value) = match (method.as_str(), parts.as_slice()) {
            ("GET", ["v1", "capabilities"]) => (
                StatusCode::OK,
                protocol::capabilities(self.push.providers()),
            ),
            ("GET", ["v1", "computers"]) => (StatusCode::OK, self.store.computers(&c).await?),
            ("POST", ["v1", "requests"]) => {
                let b = self.body(req, budget.clone(), REQUEST_MAX).await?;
                let submission = self
                    .work(budget.clone(), move || crate::store::Submission::new(b))
                    .await?;
                self.store.submit(&c, &submission).await?
            }
            ("GET", ["v1", "requests", id]) => (StatusCode::OK, self.store.receipt(&c, id).await?),
            ("POST", ["v1", "node", "heartbeat"]) => {
                let b = self.body(req, budget.clone(), MIB).await?;
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
                let b = self.body(req, budget.clone(), MIB).await?;
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
                let b = self.body(req, budget.clone(), MIB).await?;
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
            ("POST", ["v1", "push"]) => {
                let b = self.body(req, budget.clone(), MIB).await?;
                let provider = b["provider"].as_str().ok_or(Error::BAD)?;
                let target = match provider {
                    "fcm" => {
                        if !self.push.fcm_enabled() {
                            return Err(Error::UNAVAILABLE);
                        }
                        protocol::fields(&b, &["provider", "token"], &[])?;
                        protocol::text(&b["token"], 4096, false)?
                    }
                    _ => return Err(Error::BAD),
                };
                self.store.register_push(&c, provider, target).await?;
                (StatusCode::OK, json!({"ok":true}))
            }
            ("DELETE", ["v1", "push"]) => {
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
        self.store.db.tasks.spawn(async move{loop{tokio::select!{biased;_=app.stop.cancelled()=>break,_=tokio::time::sleep(Duration::from_secs_f64(app.cfg.maintenance_interval))=>{if app.store.maintain(false).await.is_err(){eprintln!("relay maintenance deferred");}}}}});
        let app = self.clone();
        self.store.db.tasks.spawn(async move{loop{tokio::select!{biased;_=app.stop.cancelled()=>break,_=tokio::time::sleep(Duration::from_secs(1))=>{if app.push.once().await.is_err(){eprintln!("relay push delivery deferred");}}}}});
    }
}
async fn dispatch(State(app): State<App>, req: Request) -> Response {
    let deadline = if req.method() == Method::POST && req.uri().path() == "/v1/requests" {
        75
    } else {
        40
    };
    let mut response =
        match tokio::time::timeout(Duration::from_secs(deadline), app.handle(req)).await {
            Ok(Ok(r)) => r,
            Ok(Err(e)) => e.into_response(),
            Err(_) => Error::TIMEOUT.into_response(),
        };
    response
        .headers_mut()
        .insert(header::CACHE_CONTROL, "no-store".parse().unwrap());
    response
        .headers_mut()
        .insert(header::X_CONTENT_TYPE_OPTIONS, "nosniff".parse().unwrap());
    response
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
