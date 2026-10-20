use std::env;

pub struct Config {
    pub database_url: String,
    pub bind_addr: String,
    pub cors_origin: Option<String>,
    pub admin_username: String,
    pub admin_password: Option<String>,
    pub secure_cookies: bool,
    pub uploads_dir: String,
}

impl Config {
    pub fn from_env() -> Self {
        Self {
            database_url: env::var("DATABASE_URL").unwrap_or_else(|_| "sqlite://data/synora_cms.sqlite3?mode=rwc".into()),
            bind_addr: env::var("BIND_ADDR").unwrap_or_else(|_| "0.0.0.0:8787".into()),
            cors_origin: env::var("CORS_ORIGIN").ok(),
            admin_username: env::var("ADMIN_USERNAME").unwrap_or_else(|_| "admin".into()),
            admin_password: env::var("ADMIN_PASSWORD").ok(),
            secure_cookies: env::var("SECURE_COOKIES").map(|v| v == "true").unwrap_or(false),
            uploads_dir: env::var("UPLOADS_DIR").unwrap_or_else(|_| "data/uploads".into()),
        }
    }
}
