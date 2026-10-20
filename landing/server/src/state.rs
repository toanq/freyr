use std::sync::Arc;

use crate::db::Database;

#[derive(Clone)]
pub struct AppState {
    pub db: Arc<dyn Database>,
    pub cookie_name: &'static str,
    pub secure_cookies: bool,
    pub uploads_dir: std::path::PathBuf,
}
