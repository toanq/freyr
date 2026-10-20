mod auth;
mod config;
mod db;
mod extractors;
mod routes;
mod state;

use std::sync::Arc;

use axum::http::{header::CONTENT_TYPE, HeaderValue, Method};
use tower_http::cors::CorsLayer;
use tower_http::trace::TraceLayer;
use tracing_subscriber::EnvFilter;

use config::Config;
use db::SqliteDb;
use state::AppState;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let _ = dotenvy::dotenv();
    tracing_subscriber::fmt().with_env_filter(EnvFilter::from_default_env().add_directive("info".parse()?)).init();

    let config = Config::from_env();

    let db = SqliteDb::connect(&config.database_url).await?;
    if let Some(password) = &config.admin_password {
        let hash = auth::hash_password(password);
        db.ensure_admin(&config.admin_username, &hash).await?;
        tracing::info!(username = %config.admin_username, "admin account ready");
    } else {
        tracing::warn!("ADMIN_PASSWORD not set -- skipping admin account seeding");
    }

    let state = AppState {
        db: Arc::new(db),
        cookie_name: "synora_admin_session",
        secure_cookies: config.secure_cookies,
        uploads_dir: config.uploads_dir.clone().into(),
    };

    let mut cors = CorsLayer::new().allow_methods([Method::GET, Method::POST, Method::PUT, Method::DELETE]).allow_credentials(true);
    cors = match &config.cors_origin {
        Some(origin) => cors.allow_origin(origin.parse::<HeaderValue>()?),
        None => cors.allow_origin(tower_http::cors::AllowOrigin::mirror_request()),
    };
    cors = cors.allow_headers([CONTENT_TYPE]);

    let app = routes::build_router(state).layer(cors).layer(TraceLayer::new_for_http());

    let listener = tokio::net::TcpListener::bind(&config.bind_addr).await?;
    tracing::info!(addr = %config.bind_addr, "synora-cms listening");
    axum::serve(listener, app).await?;
    Ok(())
}
