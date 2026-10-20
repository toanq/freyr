use axum::extract::{Multipart, State};
use axum::http::StatusCode;
use axum::Json;
use uuid::Uuid;

use crate::extractors::AdminSession;
use crate::state::AppState;

const MAX_BYTES: usize = 8 * 1024 * 1024;

fn ext_for(content_type: &str) -> Option<&'static str> {
    match content_type {
        "image/png" => Some("png"),
        "image/jpeg" => Some("jpg"),
        "image/webp" => Some("webp"),
        "image/gif" => Some("gif"),
        _ => None,
    }
}

pub async fn upload_image(
    State(state): State<AppState>,
    _session: AdminSession,
    mut multipart: Multipart,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    let field = multipart
        .next_field()
        .await
        .map_err(|e| (StatusCode::BAD_REQUEST, e.to_string()))?
        .ok_or((StatusCode::BAD_REQUEST, "no file provided".to_string()))?;

    let content_type = field.content_type().unwrap_or_default().to_string();
    let ext = ext_for(&content_type).ok_or((StatusCode::BAD_REQUEST, "unsupported image type".to_string()))?;

    let bytes = field.bytes().await.map_err(|e| (StatusCode::BAD_REQUEST, e.to_string()))?;
    if bytes.len() > MAX_BYTES {
        return Err((StatusCode::PAYLOAD_TOO_LARGE, "image too large".to_string()));
    }

    tokio::fs::create_dir_all(&state.uploads_dir)
        .await
        .map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;

    let filename = format!("{}.{ext}", Uuid::new_v4());
    let path = state.uploads_dir.join(&filename);
    tokio::fs::write(&path, &bytes).await.map_err(|e| (StatusCode::INTERNAL_SERVER_ERROR, e.to_string()))?;

    Ok(Json(serde_json::json!({ "url": format!("/uploads/{filename}") })))
}
