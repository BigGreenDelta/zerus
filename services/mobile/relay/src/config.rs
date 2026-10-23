use crate::error::{Error, Result};
use serde::{Deserialize, Serialize};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::{
    fs::{File, OpenOptions},
    io::Read,
    path::Path,
};

#[derive(Clone, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    pub database_url: Option<String>,
    pub pool_min: u32,
    pub pool_max: u32,
    pub global_max_bytes: i64,
    pub quota_shards: i64,
    pub queue_ttl: u64,
    pub claim_ttl: u64,
    pub retention: u64,
    pub max_queue: i64,
    pub max_queue_bytes: i64,
    pub online_timeout: u64,
    /// Retired UnifiedPush allowlist. Existing private configs still load; it is ignored.
    #[serde(skip_serializing)]
    pub push_hosts: Vec<String>,
    pub fcm_credentials: Option<String>,
    /// Private APNs token-signing key (`.p8`, PKCS#8 P-256) for iOS wakes.
    pub apns_key: Option<String>,
    /// The ten-character key identifier Apple issued with `apns_key`.
    pub apns_key_id: Option<String>,
    /// The Apple Developer team that owns `apns_key`.
    pub apns_team_id: Option<String>,
    /// The iOS application's bundle identifier.
    pub apns_topic: Option<String>,
    pub background: bool,
    pub trusted_proxy_cidrs: Vec<ipnet::IpNet>,
    pub max_forwarded_hops: usize,
    pub max_anonymous_requests: usize,
    pub max_authenticated_requests: usize,
    pub max_body_bytes: usize,
    pub large_payload_slots: usize,
    pub max_json_depth: usize,
    pub max_json_values: usize,
    pub max_polls_per_credential: i64,
    pub max_polls_per_workspace: i64,
    pub max_polls_global: usize,
    pub max_small_responses: usize,
    /// Disk-backed response spools, independently bounded from resident bodies.
    pub max_spool_bytes: usize,
    pub max_catalog_bytes: usize,
    pub max_rate_entries_authenticated: i64,
    pub max_rate_entries_anonymous: i64,
    pub maintenance_interval: f64,
    pub maintenance_batch: i64,
    pub max_connections: usize,
    pub shutdown_timeout: u64,
    /// Public enrollment is opt-in; legacy installations retain their limits.
    pub public_enrollment: bool,
    pub max_public_workspaces: i64,
    /// Fraction of every bounded transport lane unavailable to standard work.
    pub team_reserved_percent: usize,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            database_url: None,
            pool_min: 2,
            pool_max: 10,
            global_max_bytes: 32 * 1024 * 1024 * 1024,
            quota_shards: 64,
            queue_ttl: 120,
            claim_ttl: 90,
            retention: 7 * 86400,
            max_queue: 200,
            max_queue_bytes: 100 * 1024 * 1024,
            online_timeout: 45,
            push_hosts: vec![],
            fcm_credentials: None,
            apns_key: None,
            apns_key_id: None,
            apns_team_id: None,
            apns_topic: None,
            background: true,
            trusted_proxy_cidrs: vec![],
            max_forwarded_hops: 8,
            max_anonymous_requests: 16,
            max_authenticated_requests: 112,
            max_body_bytes: 8 * 1024 * 1024,
            large_payload_slots: 1,
            max_json_depth: 32,
            max_json_values: 50000,
            max_polls_per_credential: 2,
            max_polls_per_workspace: 32,
            max_polls_global: 256,
            max_small_responses: 8,
            max_spool_bytes: 64 * 1024 * 1024,
            max_catalog_bytes: 1024 * 1024,
            max_rate_entries_authenticated: 100000,
            max_rate_entries_anonymous: 10000,
            maintenance_interval: 1.0,
            maintenance_batch: 256,
            max_connections: 1024,
            shutdown_timeout: 40,
            public_enrollment: false,
            max_public_workspaces: 10000,
            team_reserved_percent: 25,
        }
    }
}
impl Config {
    /// Two bounded connections for each authentication/control class. Public
    /// payload connections share the remaining declared pool capacity.
    pub fn payload_pool_max(&self) -> u32 {
        if self.public_enrollment {
            self.pool_max.saturating_sub(4)
        } else {
            self.pool_max
        }
    }
    pub fn service_lanes(&self) -> [(&'static str, usize); 9] {
        [
            ("request", self.max_authenticated_requests),
            ("poll", self.max_polls_global),
            ("body", self.max_body_bytes),
            ("large", self.large_payload_slots),
            ("response", self.max_small_responses),
            ("spool", self.max_spool_bytes),
            ("cpu", 4),
            ("push", 4),
            ("database", self.payload_pool_max() as usize),
        ]
    }
    pub fn standard_capacity(&self, total: usize) -> usize {
        if self.public_enrollment {
            total - (total * self.team_reserved_percent).div_ceil(100)
        } else {
            total
        }
    }
    pub fn read(path: Option<&Path>) -> Result<Self> {
        let value: Self = match path {
            Some(p) => serde_json::from_slice(&read_private(p, 1024 * 1024)?)?,
            None => Self::default(),
        };
        value.validate()?;
        Ok(value)
    }
    pub fn validate(&self) -> Result<()> {
        let valid = self.pool_min > 0
            && self.pool_min <= self.pool_max
            && self.pool_max <= 256
            && (1..=1024).contains(&self.quota_shards)
            && self.global_max_bytes / self.quota_shards >= 1048576
            && self.max_queue > 0
            && self.max_queue_bytes >= 1048576
            && self.queue_ttl > 0
            && self.claim_ttl > 0
            && self.retention > 0
            && self.online_timeout > 0
            && (1..=8).contains(&self.large_payload_slots)
            && (1..=32).contains(&self.max_json_depth)
            && (1..=50000).contains(&self.max_json_values)
            && (1..=32).contains(&self.max_forwarded_hops)
            && (1..=65536).contains(&self.max_polls_global)
            && self.max_polls_per_credential > 0
            && self.max_polls_per_workspace > 0
            && (1..=4096).contains(&self.max_authenticated_requests)
            && (1..=4096).contains(&self.max_anonymous_requests)
            && (1..=64).contains(&self.max_small_responses)
            && (1..=1024 * 1024 * 1024).contains(&self.max_spool_bytes)
            && (1..=1048576).contains(&self.max_catalog_bytes)
            && (1..=67108864).contains(&self.max_body_bytes)
            && (1..=4096).contains(&self.maintenance_batch)
            && self.maintenance_interval.is_finite()
            && (0.1..=3600.0).contains(&self.maintenance_interval)
            && self.max_rate_entries_authenticated > 0
            && self.max_rate_entries_anonymous > 0
            && (1..=65536).contains(&self.max_connections)
            && (1..=120).contains(&self.shutdown_timeout);
        if !valid {
            return Err(Error::BAD);
        }
        if !(1..=1000000).contains(&self.max_public_workspaces)
            || !(1..=50).contains(&self.team_reserved_percent)
            || (self.public_enrollment
                && (self.large_payload_slots < 2
                    || self.max_authenticated_requests < 2
                    || self.max_small_responses < 2
                    || self.max_polls_global < 2
                    || self.pool_max < 6
                    || self.standard_capacity(self.max_spool_bytes) < 41 * 1024 * 1024
                    || self.max_spool_bytes - self.standard_capacity(self.max_spool_bytes)
                        < 41 * 1024 * 1024
                    || self.standard_capacity(self.max_body_bytes) == 0
                    || self
                        .standard_capacity((self.global_max_bytes / self.quota_shards) as usize)
                        <= 2 * 1024 * 1024))
        {
            return Err(Error::BAD);
        }
        let apns = [&self.apns_key, &self.apns_key_id, &self.apns_team_id];
        if apns.iter().any(|v| v.is_some()) && !apns.iter().all(|v| v.is_some()) {
            return Err(Error::BAD);
        }
        let identifier = |v: &Option<String>| {
            v.as_deref().is_none_or(|s| {
                s.len() == 10
                    && s.bytes()
                        .all(|b| b.is_ascii_uppercase() || b.is_ascii_digit())
            })
        };
        let topic = self.apns_topic.as_deref().is_none_or(|s| {
            (1..=255).contains(&s.len())
                && s.bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b"-.".contains(&b))
        });
        if !identifier(&self.apns_key_id) || !identifier(&self.apns_team_id) || !topic {
            return Err(Error::BAD);
        }
        if self
            .database_url
            .as_ref()
            .is_some_and(|s| !s.starts_with("postgresql://") && !s.starts_with("postgres://"))
        {
            return Err(Error::BAD);
        }
        Ok(())
    }
}
/// Open once with no-follow, then check the opened inode rather than a path race.
pub fn read_private(path: &Path, maximum: u64) -> Result<Vec<u8>> {
    let mut f = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)?;
    let m = f.metadata()?;
    if !m.is_file() || m.mode() & 0o077 != 0 || m.len() > maximum {
        return Err(Error::BAD);
    }
    let mut b = Vec::new();
    (&mut f).take(maximum + 1).read_to_end(&mut b)?;
    if b.len() as u64 > maximum {
        return Err(Error::LARGE);
    }
    Ok(b)
}
pub fn sqlite_file(path: &Path) -> Result<File> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    if !parent.exists() {
        use std::os::unix::fs::DirBuilderExt;
        std::fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(parent)?;
    }
    let m = parent.metadata()?;
    // A temporary owned file determines the effective uid without unsafe calls.
    let probe = tempfile::tempfile_in(parent)?;
    if m.mode() & 0o077 != 0 || m.uid() != probe.metadata()?.uid() {
        return Err(Error::BAD);
    }
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)?;
    let info = file.metadata()?;
    if !info.is_file() || info.uid() != m.uid() {
        return Err(Error::BAD);
    }
    file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    fs2::FileExt::try_lock_exclusive(&file).map_err(|_| Error::BUSY)?;
    Ok(file)
}
