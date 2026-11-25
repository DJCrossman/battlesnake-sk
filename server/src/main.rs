use std::env;

use axum::serve::ListenerExt;
use battlesnake_sk::{Config, PAD_IDLE, router};
use tokio::net::TcpListener;
use tokio::signal::unix::{SignalKind, signal};
use tracing::info;

#[tokio::main]
async fn main() -> std::io::Result<()> {
    tracing_subscriber::fmt()
        .json()
        .with_current_span(false)
        .with_target(false)
        .init();

    let token = match env::var("CONTROL_TOKEN") {
        Ok(token) if !token.is_empty() => token,
        _ => {
            let mut bytes = [0u8; 16];
            getrandom::fill(&mut bytes).expect("the OS random source is available");
            let token = format!("{:032x}", u128::from_be_bytes(bytes));
            info!(pad = %format!("/pad#{token}"), "generated control token");
            token
        }
    };
    let config = Config {
        token,
        colour: env::var("SNAKE_COLOUR").unwrap_or_else(|_| "#DD69b4".to_owned()),
        pad_idle: PAD_IDLE,
    };
    let port: u16 = env::var("PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .unwrap_or(80);

    let listener = TcpListener::bind(("0.0.0.0", port)).await?.tap_io(|tcp| {
        let _ = tcp.set_nodelay(true);
    });
    info!(port, "listening");
    axum::serve(listener, router(config))
        .with_graceful_shutdown(shutdown_requested())
        .await?;
    info!("stopped");
    Ok(())
}

async fn shutdown_requested() {
    let mut terminate = signal(SignalKind::terminate()).expect("SIGTERM handler installs");
    tokio::select! {
        _ = terminate.recv() => {}
        _ = tokio::signal::ctrl_c() => {}
    }
    info!("shutting down");
}
