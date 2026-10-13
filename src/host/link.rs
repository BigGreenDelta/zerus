//! A connection between the session host and one client, as protocol frames
//! (spec D5). On Windows it is a named pipe (`pipe::PipeStream`); the host's
//! request handling, attach streams, the supervisor channel and the client
//! library are written against this trait, so their tests run on every
//! platform over `memory_pair`.
use super::protocol::{self, Frame, FrameError, Kind};
use serde::Serialize;
use std::{io, time::Duration};

/// One end of a connection. Clones of a link share it through `Arc`: one thread
/// may block reading while others write; frames never interleave.
pub(crate) trait Link: Send + Sync + 'static {
    /// The next frame; None when the peer closed the connection between frames
    /// or `shutdown` was called.
    fn read_frame(&self) -> Result<Option<Frame>, FrameError>;
    /// Write one whole frame.
    fn write_frame(&self, kind: Kind, payload: &[u8]) -> Result<(), FrameError>;
    /// How long a read waits; None waits for ever. A read that times out inside
    /// a frame leaves the connection out of step: close it.
    fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()>;
    /// Wake every thread waiting on this end: reads end, writes fail, now and
    /// later. The peer sees the end once the last reference is dropped.
    fn shutdown(&self);
}

/// Write `value` as one JSON frame.
pub(crate) fn write_json<T: Serialize + ?Sized>(
    link: &dyn Link,
    kind: Kind,
    value: &T,
) -> Result<(), FrameError> {
    let payload = serde_json::to_vec(value).map_err(io::Error::from)?;
    link.write_frame(kind, &payload)
}

/// The bytes of one frame, as `protocol::write_frame` writes them.
pub(crate) fn frame_bytes(kind: Kind, payload: &[u8]) -> Result<Vec<u8>, FrameError> {
    let mut bytes = Vec::with_capacity(payload.len() + 5);
    protocol::write_frame(&mut bytes, kind, payload)?;
    Ok(bytes)
}

#[cfg(test)]
pub(crate) use self::memory::memory_pair;

#[cfg(test)]
mod memory {
    use super::*;
    use std::{
        collections::VecDeque,
        io::Read,
        sync::{
            atomic::{AtomicBool, Ordering},
            Arc, Condvar, Mutex, PoisonError,
        },
        time::Instant,
    };

    /// One direction of an in-memory connection.
    #[derive(Default)]
    struct Channel {
        state: Mutex<(VecDeque<u8>, bool)>,
        ready: Condvar,
    }

    impl Channel {
        fn close(&self) {
            self.state.lock().unwrap_or_else(PoisonError::into_inner).1 = true;
            self.ready.notify_all();
        }
    }

    /// An end of an in-memory connection that behaves like a pipe end: the peer
    /// reads end of file once this end is dropped.
    pub(crate) struct MemoryLink {
        incoming: Arc<Channel>,
        outgoing: Arc<Channel>,
        timeout: Mutex<Option<Duration>>,
        down: AtomicBool,
    }

    /// Two connected ends.
    pub(crate) fn memory_pair() -> (MemoryLink, MemoryLink) {
        let (a, b) = (Arc::new(Channel::default()), Arc::new(Channel::default()));
        let end = |incoming: &Arc<Channel>, outgoing: &Arc<Channel>| MemoryLink {
            incoming: incoming.clone(),
            outgoing: outgoing.clone(),
            timeout: Mutex::new(None),
            down: AtomicBool::new(false),
        };
        (end(&a, &b), end(&b, &a))
    }

    struct Reader<'a>(&'a MemoryLink);

    impl Read for Reader<'_> {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            let link = self.0;
            let timeout = *link.timeout.lock().unwrap_or_else(PoisonError::into_inner);
            let deadline = timeout.map(|timeout| Instant::now() + timeout);
            let mut state = link
                .incoming
                .state
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            loop {
                if link.down.load(Ordering::SeqCst) {
                    return Ok(0);
                }
                if !state.0.is_empty() {
                    let count = buffer.len().min(state.0.len());
                    for (slot, byte) in buffer.iter_mut().zip(state.0.drain(..count)) {
                        *slot = byte;
                    }
                    return Ok(count);
                }
                if state.1 {
                    return Ok(0);
                }
                let wait = match deadline {
                    Some(deadline) => {
                        let left = deadline.saturating_duration_since(Instant::now());
                        if left.is_zero() {
                            return Err(io::ErrorKind::TimedOut.into());
                        }
                        left.min(Duration::from_millis(20))
                    }
                    None => Duration::from_millis(20),
                };
                state = link
                    .incoming
                    .ready
                    .wait_timeout(state, wait)
                    .unwrap_or_else(PoisonError::into_inner)
                    .0;
            }
        }
    }

    impl Link for MemoryLink {
        fn read_frame(&self) -> Result<Option<Frame>, FrameError> {
            protocol::read_frame(&mut Reader(self))
        }

        fn write_frame(&self, kind: Kind, payload: &[u8]) -> Result<(), FrameError> {
            let bytes = frame_bytes(kind, payload)?;
            if self.down.load(Ordering::SeqCst) {
                return Err(io::Error::from(io::ErrorKind::BrokenPipe).into());
            }
            let mut state = self
                .outgoing
                .state
                .lock()
                .unwrap_or_else(PoisonError::into_inner);
            if state.1 {
                return Err(io::Error::from(io::ErrorKind::BrokenPipe).into());
            }
            state.0.extend(bytes);
            self.outgoing.ready.notify_all();
            Ok(())
        }

        fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
            *self.timeout.lock().unwrap_or_else(PoisonError::into_inner) = timeout;
            Ok(())
        }

        fn shutdown(&self) {
            self.down.store(true, Ordering::SeqCst);
            self.incoming.ready.notify_all();
        }
    }

    impl Drop for MemoryLink {
        fn drop(&mut self) {
            self.outgoing.close();
            self.incoming.close();
        }
    }

    #[test]
    fn memory_links_carry_frames_and_end_like_pipes() {
        let (a, b) = memory_pair();
        a.write_frame(Kind::Json, b"{}").unwrap();
        a.write_frame(Kind::Stream, b"bytes").unwrap();
        assert_eq!(b.read_frame().unwrap().unwrap().payload, b"{}");
        assert_eq!(b.read_frame().unwrap().unwrap().kind, Kind::Stream);
        b.set_read_timeout(Some(Duration::from_millis(30))).unwrap();
        assert!(
            matches!(b.read_frame(), Err(FrameError::Io(e)) if e.kind() == io::ErrorKind::TimedOut)
        );
        b.set_read_timeout(None).unwrap();
        drop(a);
        assert!(b.read_frame().unwrap().is_none());
        assert!(b.write_frame(Kind::Json, b"{}").is_err());
        let (c, _d) = memory_pair();
        let c = Arc::new(c);
        let reader = {
            let c = c.clone();
            std::thread::spawn(move || c.read_frame().unwrap().is_none())
        };
        std::thread::sleep(Duration::from_millis(50));
        c.shutdown();
        assert!(reader.join().unwrap());
    }
}
