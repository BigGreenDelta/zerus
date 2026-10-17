//! A loopback relay server for the HTTP contracts, on a fixture's store.
#![allow(dead_code)]

use std::{net::SocketAddr, time::Duration};
use tokio::{io::AsyncWriteExt, net::TcpStream, task::JoinHandle};
use tokio_util::sync::CancellationToken;
use zerus_relay::{
    error::Result,
    http::{serve, App},
    store::Store,
};

pub struct Server {
    pub url: String,
    pub address: SocketAddr,
    stop: CancellationToken,
    task: JoinHandle<Result<()>>,
}

impl Server {
    pub async fn start(store: &Store) -> Self {
        Self::serve(App::new(store.clone()).await.unwrap()).await
    }
    pub async fn serve(app: App) -> Self {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let address = listener.local_addr().unwrap();
        let stop = CancellationToken::new();
        let task = tokio::spawn(serve(listener, app, stop.clone()));
        Self {
            url: format!("http://{address}"),
            address,
            stop,
            task,
        }
    }
    pub fn at(&self, path: &str) -> String {
        format!("{}{path}", self.url)
    }
    /// Sends a request head (and any body start) on its own connection and
    /// keeps the connection open, so the request stays in flight.
    pub async fn hold(&self, head: &str) -> TcpStream {
        let mut stream = TcpStream::connect(self.address).await.unwrap();
        stream.write_all(head.as_bytes()).await.unwrap();
        stream
    }
    /// A long poll held open on its own connection.
    pub async fn poll(&self, path: &str, token: &str) -> TcpStream {
        self.hold(&format!(
            "GET {path} HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {token}\r\n\r\n"
        ))
        .await
    }
    pub async fn stop(self) {
        self.stop.cancel();
        self.task.await.unwrap().unwrap();
    }
}

/// Waits up to two seconds until `probe` holds.
pub async fn eventually<F, Fut>(mut probe: F) -> bool
where
    F: FnMut() -> Fut,
    Fut: std::future::Future<Output = bool>,
{
    for _ in 0..200 {
        if probe().await {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    false
}
