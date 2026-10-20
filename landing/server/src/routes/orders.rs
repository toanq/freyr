use axum::extract::{Path, State};
use axum::http::StatusCode;
use axum::Json;
use serde::Deserialize;

use crate::db::NewOrder;
use crate::extractors::AdminSession;
use crate::state::AppState;

pub async fn create(
    State(state): State<AppState>,
    Json(body): Json<NewOrder>,
) -> Result<Json<serde_json::Value>, (StatusCode, String)> {
    if body.customer_name.trim().is_empty() || body.phone.trim().is_empty() {
        return Err((StatusCode::BAD_REQUEST, "customer_name and phone are required".to_string()));
    }
    let order = state
        .db
        .create_order(body)
        .await
        .map_err(|e| (StatusCode::BAD_REQUEST, format!("could not create order: {e}")))?;
    Ok(Json(serde_json::json!(order)))
}

pub async fn list_admin(
    State(state): State<AppState>,
    _session: AdminSession,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let orders = state.db.list_orders().await.map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?;
    Ok(Json(serde_json::json!(orders)))
}

#[derive(Deserialize)]
pub struct UpdateStatusRequest {
    status: String,
}

pub async fn update_status(
    State(state): State<AppState>,
    _session: AdminSession,
    Path(id): Path<String>,
    Json(body): Json<UpdateStatusRequest>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let order = state
        .db
        .update_order_status(&id, &body.status)
        .await
        .map_err(|_| StatusCode::INTERNAL_SERVER_ERROR)?
        .ok_or(StatusCode::NOT_FOUND)?;
    Ok(Json(serde_json::json!(order)))
}
