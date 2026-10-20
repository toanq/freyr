use axum::extract::{Path, State};
use axum::http::StatusCode;
use axum::Json;

use crate::db::{NewPost, UpdatePost};
use crate::extractors::AdminSession;
use crate::state::AppState;

pub async fn list_public(State(state): State<AppState>) -> Result<Json<serde_json::Value>, StatusCode> {
    let posts = state.db.list_published_posts().await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    Ok(Json(serde_json::json!(posts)))
}

pub async fn get_by_slug(
    State(state): State<AppState>,
    Path(slug): Path<String>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let posts = state.db.list_published_posts().await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    let post = posts.into_iter().find(|p| p.slug == slug).ok_or(StatusCode::NOT_FOUND)?;
    Ok(Json(serde_json::json!(post)))
}

pub async fn list_admin(
    State(state): State<AppState>,
    _session: AdminSession,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let posts = state.db.list_all_posts().await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    Ok(Json(serde_json::json!(posts)))
}

pub async fn get_one(
    State(state): State<AppState>,
    _session: AdminSession,
    Path(id): Path<String>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let post = state
        .db
        .get_post(&id)
        .await
        .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?
        .ok_or(StatusCode::NOT_FOUND)?;
    Ok(Json(serde_json::json!(post)))
}

pub async fn create(
    State(state): State<AppState>,
    _session: AdminSession,
    Json(body): Json<NewPost>,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    let post = state
        .db
        .create_post(body)
        .await
        .map_err(|e| (StatusCode::BAD_REQUEST, format!("could not create post: {e}")))?;
    Ok(Json(serde_json::json!(post)))
}

pub async fn update(
    State(state): State<AppState>,
    _session: AdminSession,
    Path(id): Path<String>,
    Json(body): Json<UpdatePost>,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    let post = state
        .db
        .update_post(&id, body)
        .await
        .map_err(|e| (StatusCode::BAD_REQUEST, format!("could not update post: {e}")))?
        .ok_or((StatusCode::NOT_FOUND, "post not found".to_string()))?;
    Ok(Json(serde_json::json!(post)))
}

pub async fn delete(
    State(state): State<AppState>,
    _session: AdminSession,
    Path(id): Path<String>,
) -> Result<StatusCode, StatusCode> {
    let deleted = state.db.delete_post(&id).await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    if deleted {
        Ok(StatusCode::NO_CONTENT)
    } else {
        Err(StatusCode::NOT_FOUND)
    }
}
