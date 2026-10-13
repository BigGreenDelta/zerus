//! Small, read-only host telemetry for the dashboard. No external processes or agent input.
//! The CLI is short lived: persist CPU counters between polls instead of sleeping to sample.
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    fs,
    io::Write,
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Debug, Deserialize, Serialize)]
struct CpuSample {
    total: u64,
    idle: u64,
    uptime: f64,
    percent: Option<f64>,
    interval: Option<f64>,
}

fn cpu_delta(previous: &CpuSample, current: &CpuSample) -> Option<(f64, f64)> {
    let interval = current.uptime - previous.uptime;
    // Reboots, suspended machines and old caches start a new measurement window.
    if !(0.2..=600.).contains(&interval)
        || current.total <= previous.total
        || current.idle < previous.idle
    {
        return None;
    }
    let total = current.total - previous.total;
    let idle = current.idle - previous.idle;
    if idle > total {
        return None;
    }
    Some((100. * (total - idle) as f64 / total as f64, interval))
}

fn sampled_cpu(root: &Path, mut current: CpuSample) -> std::io::Result<(Option<f64>, Option<f64>)> {
    let mut attempt = || -> std::io::Result<(Option<f64>, Option<f64>)> {
        let directory = root.join("telemetry");
        fs::create_dir_all(&directory)?;
        let lock = fs::OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(directory.join("cpu.lock"))?;
        // Concurrent listings (the desktop, peers over SSH, the mobile connector)
        // hold this lock for moments; wait briefly rather than report CPU unavailable.
        let deadline = std::time::Instant::now() + std::time::Duration::from_millis(500);
        while let Err(error) = lock.try_lock_exclusive() {
            if !crate::platform::lock_contended(&error) || std::time::Instant::now() >= deadline {
                return Err(error);
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        let _lock = crate::platform::FileLock::held(lock);
        let path = directory.join("cpu.json");
        let previous: Option<CpuSample> = fs::metadata(&path)
            .ok()
            .filter(|m| m.len() < 2048)
            .and_then(|_| fs::read(&path).ok())
            .and_then(|bytes| serde_json::from_slice(&bytes).ok());
        if let Some(previous) = previous {
            let elapsed = current.uptime - previous.uptime;
            // Simultaneous CLI clients should not replace a useful baseline with a zero interval.
            if (0.0..0.2).contains(&elapsed) {
                return Ok((previous.percent, previous.interval));
            }
            if let Some((percent, interval)) = cpu_delta(&previous, &current) {
                current.percent = Some(percent);
                current.interval = Some(interval);
            }
        }
        let mut file = tempfile::NamedTempFile::new_in(&directory)?;
        file.write_all(&serde_json::to_vec(&current)?)?;
        crate::platform::persist(file, path).map_err(|e| e.error)?;
        Ok((current.percent, current.interval))
    };
    attempt()
}

#[cfg(target_os = "linux")]
fn linux_cpu(text: &str, uptime: f64) -> Option<CpuSample> {
    let line = text.lines().find(|line| line.starts_with("cpu "))?;
    // guest/guest_nice are already included in user/nice and must not be counted twice.
    let counters = line
        .split_whitespace()
        .skip(1)
        .take(8)
        .map(str::parse::<u64>)
        .collect::<Result<Vec<_>, _>>()
        .ok()?;
    if counters.len() < 4 {
        return None;
    }
    let total = counters
        .iter()
        .try_fold(0u64, |sum, value| sum.checked_add(*value))?;
    let idle = counters[3].checked_add(counters.get(4).copied().unwrap_or(0))?;
    Some(CpuSample {
        total,
        idle,
        uptime,
        percent: None,
        interval: None,
    })
}

#[cfg(target_os = "linux")]
fn linux_memory(text: &str) -> Option<(u64, u64)> {
    let values: std::collections::HashMap<&str, u64> = text
        .lines()
        .filter_map(|line| {
            let (key, rest) = line.split_once(':')?;
            let bytes = rest
                .split_whitespace()
                .next()?
                .parse::<u64>()
                .ok()?
                .checked_mul(1024)?;
            Some((key, bytes))
        })
        .collect();
    let total = *values.get("MemTotal")?;
    if total == 0 {
        return None;
    }
    let available = values
        .get("MemAvailable")
        .copied()
        .unwrap_or_else(|| {
            ["MemFree", "Buffers", "Cached", "SReclaimable"]
                .iter()
                .map(|key| values.get(key).copied().unwrap_or(0))
                .fold(0u64, u64::saturating_add)
                .saturating_sub(values.get("Shmem").copied().unwrap_or(0))
        })
        .min(total);
    Some((total, total - available))
}

#[cfg(target_os = "linux")]
fn native() -> (Option<CpuSample>, Option<(u64, u64)>, Option<f64>) {
    let uptime = fs::read_to_string("/proc/uptime")
        .ok()
        .and_then(|text| text.split_whitespace().next()?.parse::<f64>().ok());
    let cpu = uptime.and_then(|uptime| {
        fs::read_to_string("/proc/stat")
            .ok()
            .and_then(|text| linux_cpu(&text, uptime))
    });
    let memory = fs::read_to_string("/proc/meminfo")
        .ok()
        .and_then(|text| linux_memory(&text));
    (cpu, memory, uptime)
}

#[cfg(target_os = "macos")]
fn native() -> (Option<CpuSample>, Option<(u64, u64)>, Option<f64>) {
    unsafe fn sysctl<T: Copy>(name: &[u8]) -> Option<T> {
        let mut value = std::mem::MaybeUninit::<T>::uninit();
        let mut size = std::mem::size_of::<T>();
        if libc::sysctlbyname(
            name.as_ptr().cast(),
            value.as_mut_ptr().cast(),
            &mut size,
            std::ptr::null_mut(),
            0,
        ) == 0
            && size == std::mem::size_of::<T>()
        {
            Some(value.assume_init())
        } else {
            None
        }
    }
    unsafe {
        let uptime = sysctl::<libc::timeval>(b"kern.boottime\0")
            .map(|boot| {
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_secs_f64()
                    - boot.tv_sec as f64
                    - boot.tv_usec as f64 / 1_000_000.
            })
            .filter(|value| value.is_finite() && *value >= 0.);
        // libc only deprecates its binding in favor of another crate; the Darwin API is stable.
        #[allow(deprecated)]
        let host = libc::mach_host_self();
        let mut ticks = [0u32; 4];
        let mut count = libc::HOST_CPU_LOAD_INFO_COUNT;
        let cpu = if libc::host_statistics(
            host,
            libc::HOST_CPU_LOAD_INFO,
            ticks.as_mut_ptr().cast(),
            &mut count,
        ) == 0
            && count == 4
        {
            uptime.map(|uptime| CpuSample {
                total: ticks.iter().map(|n| *n as u64).sum(),
                idle: ticks[2] as u64,
                uptime,
                percent: None,
                interval: None,
            })
        } else {
            None
        };
        let total = sysctl::<u64>(b"hw.memsize\0");
        let mut memory = std::mem::zeroed::<libc::vm_statistics64>();
        let mut count = libc::HOST_VM_INFO64_COUNT;
        let memory = if libc::host_statistics64(
            host,
            libc::HOST_VM_INFO64,
            (&mut memory as *mut libc::vm_statistics64).cast(),
            &mut count,
        ) == 0
        {
            let page = libc::sysconf(libc::_SC_PAGESIZE);
            total.filter(|total| *total > 0 && page > 0).map(|total| {
                // Activity Monitor's app + wired + compressed memory. File cache is reclaimable.
                let used_pages = (memory.internal_page_count as u64)
                    .saturating_sub(memory.purgeable_count as u64)
                    .saturating_add(memory.wire_count as u64)
                    .saturating_add(memory.compressor_page_count as u64);
                (total, used_pages.saturating_mul(page as u64).min(total))
            })
        } else {
            None
        };
        // Each sample runs in its own short-lived CLI; the host send right dies with the process.
        (cpu, memory, uptime)
    }
}

#[cfg(windows)]
fn native() -> (Option<CpuSample>, Option<(u64, u64)>, Option<f64>) {
    use windows_sys::Win32::{
        Foundation::FILETIME,
        System::{
            SystemInformation::{GetTickCount64, GlobalMemoryStatusEx, MEMORYSTATUSEX},
            Threading::GetSystemTimes,
        },
    };
    let ticks = |time: FILETIME| (u64::from(time.dwHighDateTime) << 32) | u64::from(time.dwLowDateTime);
    let uptime = unsafe { GetTickCount64() } as f64 / 1000.;
    let zero = FILETIME { dwLowDateTime: 0, dwHighDateTime: 0 };
    let (mut idle, mut kernel, mut user) = (zero, zero, zero);
    // Kernel time includes idle time, as Linux's total includes idle.
    let cpu = (unsafe { GetSystemTimes(&mut idle, &mut kernel, &mut user) } != 0).then(|| CpuSample {
        total: ticks(kernel).saturating_add(ticks(user)),
        idle: ticks(idle),
        uptime,
        percent: None,
        interval: None,
    });
    let mut status: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
    status.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
    let memory = (unsafe { GlobalMemoryStatusEx(&mut status) } != 0 && status.ullTotalPhys > 0)
        .then(|| (status.ullTotalPhys, status.ullTotalPhys - status.ullAvailPhys.min(status.ullTotalPhys)));
    (cpu, memory, Some(uptime))
}

#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
fn native() -> (Option<CpuSample>, Option<(u64, u64)>, Option<f64>) {
    (None, None, None)
}

pub fn snapshot(root: &Path) -> Value {
    let (cpu, memory, uptime) = native();
    let sampled = cpu.map(|cpu| sampled_cpu(root, cpu));
    let unavailable = sampled.as_ref().is_none_or(|result| result.is_err());
    let (percent, interval) = sampled.and_then(Result::ok).unwrap_or((None, None));
    let cpu_state = if unavailable {
        "unavailable"
    } else if percent.is_some() {
        "available"
    } else {
        "warming"
    };
    #[cfg(unix)]
    let (load, loaded) = {
        let mut load = [0.; 3];
        let loaded = unsafe { libc::getloadavg(load.as_mut_ptr(), 3) };
        (load, loaded)
    };
    // Windows has no load average; its load fields stay null.
    #[cfg(not(unix))]
    let (load, loaded): ([f64; 3], i32) = ([0.; 3], 0);
    let (total, used) = memory
        .map(|(total, used)| (Some(total), Some(used)))
        .unwrap_or((None, None));
    json!({
        "sampled_at":SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs_f64(),
        "platform": std::env::consts::OS,
        "cpu_percent":percent, "cpu_interval_seconds":interval, "cpu_state":cpu_state,
        "cpu_count":std::thread::available_parallelism().ok().map(|n| n.get()),
        "load_1":if loaded > 0 { Some(load[0]) } else { None },
        "load_5":if loaded > 1 { Some(load[1]) } else { None },
        "load_15":if loaded > 2 { Some(load[2]) } else { None },
        "memory_total_bytes":total, "memory_used_bytes":used,
        "memory_available_bytes":memory.map(|(total,used)|total-used), "uptime_seconds":uptime,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn cpu(total: u64, idle: u64, uptime: f64) -> CpuSample {
        CpuSample {
            total,
            idle,
            uptime,
            percent: None,
            interval: None,
        }
    }
    #[test]
    fn cpu_measurements_use_deltas_and_reject_reboots() {
        assert_eq!(
            cpu_delta(&cpu(100, 60, 100.), &cpu(200, 90, 102.)),
            Some((70., 2.))
        );
        assert_eq!(cpu_delta(&cpu(100, 60, 100.), &cpu(101, 61, 100.01)), None);
        assert_eq!(cpu_delta(&cpu(100, 60, 100.), &cpu(20, 10, 1.)), None);
        assert_eq!(cpu_delta(&cpu(100, 60, 100.), &cpu(200, 90, 900.)), None);
        assert_eq!(cpu_delta(&cpu(100, 60, 100.), &cpu(101, 62, 101.)), None);
    }
    #[test]
    fn cache_is_atomic_and_rapid_polls_keep_a_useful_measurement() {
        let root = tempfile::tempdir().unwrap();
        assert_eq!(
            sampled_cpu(root.path(), cpu(100, 60, 100.)).unwrap(),
            (None, None)
        );
        assert_eq!(
            sampled_cpu(root.path(), cpu(200, 90, 102.)).unwrap(),
            (Some(70.), Some(2.))
        );
        assert_eq!(
            sampled_cpu(root.path(), cpu(201, 91, 102.01)).unwrap(),
            (Some(70.), Some(2.))
        );
        assert_eq!(
            sampled_cpu(root.path(), cpu(300, 130, 104.)).unwrap(),
            (Some(60.), Some(2.))
        );
        fs::write(root.path().join("telemetry/cpu.json"), b"bad cache").unwrap();
        assert_eq!(
            sampled_cpu(root.path(), cpu(400, 170, 106.)).unwrap(),
            (None, None)
        );
    }
    #[test]
    fn briefly_held_lock_delays_sampling_instead_of_failing() {
        use fs2::FileExt;
        let root = tempfile::tempdir().unwrap();
        sampled_cpu(root.path(), cpu(100, 60, 100.)).unwrap();
        // Another listing (or a child between fork and exec) holds the lock for a moment.
        let holder = fs::File::open(root.path().join("telemetry/cpu.lock")).unwrap();
        holder.lock_exclusive().unwrap();
        let release = std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(60));
            // Like a listing: closing alone may release the lock late on Windows.
            FileExt::unlock(&holder).unwrap();
        });
        assert_eq!(
            sampled_cpu(root.path(), cpu(200, 90, 102.)).unwrap(),
            (Some(70.), Some(2.))
        );
        release.join().unwrap();
        // A lock that stays held still fails, within a bounded wait.
        let holder = fs::File::open(root.path().join("telemetry/cpu.lock")).unwrap();
        holder.lock_exclusive().unwrap();
        let started = std::time::Instant::now();
        assert!(sampled_cpu(root.path(), cpu(300, 130, 104.)).is_err());
        assert!(started.elapsed() < std::time::Duration::from_secs(2));
    }

    #[test]
    fn unavailable_cache_is_not_a_warmup() {
        let root = tempfile::tempdir().unwrap();
        fs::write(root.path().join("telemetry"), "not a directory").unwrap();
        let result = snapshot(root.path());
        assert_eq!(result["cpu_state"], "unavailable");
        assert!(result["cpu_percent"].is_null());
        assert!(result["memory_total_bytes"].as_u64().unwrap() > 0);
    }
    #[cfg(target_os = "linux")]
    #[test]
    fn linux_cpu_does_not_double_count_guests_and_iowait_is_idle() {
        let sample = linux_cpu("cpu  100 10 30 400 20 1 2 3 50 5\ncpu0 2 3", 50.).unwrap();
        assert_eq!(sample.total, 566);
        assert_eq!(sample.idle, 420);
        assert!(linux_cpu("cpu invalid", 1.).is_none());
    }
    #[cfg(target_os = "linux")]
    #[test]
    fn linux_available_memory_includes_reclaimable_cache() {
        assert_eq!(
            linux_memory("MemTotal: 1000 kB\nMemAvailable: 700 kB\nMemFree: 50 kB"),
            Some((1024000, 307200))
        );
        assert_eq!(linux_memory("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 200 kB\nSReclaimable: 100 kB\nShmem: 50 kB"),Some((1024000,614400)));
    }
    #[test]
    fn native_snapshot_has_memory_and_no_invented_first_cpu() {
        let root = tempfile::tempdir().unwrap();
        let result = snapshot(root.path());
        assert!(result["sampled_at"].as_f64().unwrap() > 1_000_000_000.);
        assert!(result["memory_total_bytes"].as_u64().unwrap() > 0);
        assert!(
            result["memory_used_bytes"].as_u64().unwrap()
                <= result["memory_total_bytes"].as_u64().unwrap()
        );
        assert!(result["cpu_percent"].is_null());
        assert!(result["uptime_seconds"].as_f64().unwrap() > 0.);
    }
}
