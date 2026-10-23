use clap::{Parser, Subcommand};
use std::{path::PathBuf, sync::Arc};
use tokio_util::sync::CancellationToken;
use zerus_relay::{
    config::Config,
    db::Db,
    error::Result,
    http::{serve, App},
    protocol,
    store::{Role, ServiceClass, Store},
};

#[derive(Parser)]
#[command(version, about = "Zerus trusted API v1 mobile relay")]
struct Cli {
    #[arg(long, global = true)]
    database: Option<PathBuf>,
    #[arg(long, global = true)]
    config: Option<PathBuf>,
    #[command(subcommand)]
    command: Command,
}
#[derive(Subcommand)]
enum Command {
    Serve {
        #[arg(long, default_value = "127.0.0.1")]
        host: String,
        #[arg(long, default_value_t = 8787)]
        port: u16,
    },
    Provision {
        #[arg(long)]
        name: String,
        #[arg(long, default_value = "Computer")]
        computer_name: String,
    },
    Node {
        #[arg(long)]
        workspace: String,
        #[arg(long)]
        name: String,
    },
    Invite {
        #[arg(long)]
        workspace: String,
    },
    RevokeDevice {
        #[arg(long)]
        id: String,
    },
    RevokeNode {
        #[arg(long)]
        id: String,
    },
    RevokeComputer {
        #[arg(long)]
        id: String,
    },
    /// Assign service priority by local operator policy; clients cannot set it.
    ServiceClass {
        #[arg(long)]
        workspace: String,
        #[arg(long)]
        class: String,
    },
    /// Verify the configured backend can be opened without exposing credentials.
    Check,
    /// Probe the local HTTP listener without opening another database connection.
    Healthcheck {
        #[arg(long, default_value_t = 8787)]
        port: u16,
    },
}
#[tokio::main(worker_threads = 2)]
async fn main() {
    if let Err(e) = run(Cli::parse()).await {
        eprintln!("zerus-relay: {e}");
        std::process::exit(if e.0.is_client_error() { 2 } else { 1 });
    }
}
async fn run(cli: Cli) -> Result<()> {
    if let Command::Healthcheck { port } = cli.command {
        let response = reqwest::Client::builder()
            .no_proxy()
            .timeout(std::time::Duration::from_secs(2))
            .build()
            .map_err(|_| zerus_relay::error::Error::UNAVAILABLE)?
            .get(format!("http://127.0.0.1:{port}/readyz"))
            .send()
            .await
            .map_err(|_| zerus_relay::error::Error::UNAVAILABLE)?;
        return if response.status().is_success() {
            Ok(())
        } else {
            Err(zerus_relay::error::Error::UNAVAILABLE)
        };
    }
    let cfg = Arc::new(Config::read(cli.config.as_deref())?);
    if !cfg.push_hosts.is_empty() {
        eprintln!("zerus-relay: ignoring push_hosts; UnifiedPush support was removed");
    }
    let path = cli.database.unwrap_or_else(|| {
        PathBuf::from(std::env::var_os("HOME").unwrap_or_default())
            .join(".local/share/zerus-mobile/relay.sqlite3")
    });
    let db = Db::open(&path, cfg).await?;
    let store = Store::new(db.clone());
    let outcome = async {
        let value = match cli.command {
            Command::Serve { host, port } => {
                store.maintain_at_startup(!db.postgres).await?;
                let app = App::new(store.clone()).await?;
                let listener = tokio::net::TcpListener::bind((host.as_str(), port)).await?;
                app.start_background();
                let shutdown = CancellationToken::new();
                let signal = shutdown.clone();
                tokio::spawn(async move {
                    let mut term =
                        tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
                            .expect("signal handler");
                    tokio::select! {_=tokio::signal::ctrl_c()=>{},_=term.recv()=>{}}
                    signal.cancel();
                });
                eprintln!("zerus-relay ready");
                serve(listener, app, shutdown).await?;
                return Ok(());
            }
            Command::Provision {
                name,
                computer_name,
            } => {
                protocol::text(&serde_json::json!(name), 128, false)?;
                protocol::text(&serde_json::json!(computer_name), 128, false)?;
                store.provision(&name, &computer_name).await?
            }
            Command::Node { workspace, name } => {
                protocol::text(&serde_json::json!(name), 128, false)?;
                store.node(&workspace, &name).await?
            }
            Command::Invite { workspace } => store.invite(&workspace).await?,
            Command::RevokeDevice { id } => {
                store.revoke(Role::Device, &id).await?;
                serde_json::json!({"revoked":true})
            }
            Command::RevokeNode { id } => {
                store.revoke(Role::Node, &id).await?;
                serde_json::json!({"revoked":true})
            }
            Command::RevokeComputer { id } => {
                store.revoke_computer(&id).await.map_err(|e| {
                    if e.0 == axum::http::StatusCode::NOT_FOUND {
                        zerus_relay::error::Error(e.0, "computer not found")
                    } else {
                        e
                    }
                })?;
                serde_json::json!({"revoked":true})
            }
            Command::ServiceClass { workspace, class } => {
                store
                    .set_service_class(&workspace, ServiceClass::parse(&class)?)
                    .await?;
                serde_json::json!({"workspace_id":workspace,"service_class":class})
            }
            Command::Healthcheck { .. } => unreachable!(),
            Command::Check => serde_json::json!({"ok":true,"protocol_version":1}),
        };
        // Deliberate local administration output. HTTP/provider paths never log data.
        println!("{value}");
        Ok(())
    }
    .await;
    db.close().await;
    outcome
}
