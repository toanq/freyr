pub mod auth;
pub mod orders;
pub mod posts;
pub mod uploads;

use axum::routing::{get, post};
use axum::Router;
use tower_http::services::ServeDir;

use crate::state::AppState;

pub fn build_router(state: AppState) -> Router {
    let uploads_dir = state.uploads_dir.clone();
    Router::new()
        .route("/health", get(|| async { "ok" }))
        .route("/auth/login", post(auth::login))
        .route("/auth/logout", post(auth::logout))
        .route("/auth/me", get(auth::me))
        .route("/posts", get(posts::list_public))
        .route("/posts/{slug}", get(posts::get_by_slug))
        .route("/orders", post(orders::create))
        .route("/admin/posts", get(posts::list_admin).post(posts::create))
        .route(
            "/admin/posts/{id}",
            get(posts::get_one).put(posts::update).delete(posts::delete),
        )
        .route("/admin/orders", get(orders::list_admin))
        .route("/admin/orders/{id}/status", post(orders::update_status))
        .route("/admin/uploads", post(uploads::upload_image))
        .nest_service("/uploads", ServeDir::new(uploads_dir))
        .with_state(state)
}
