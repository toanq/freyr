use axum::extract::State;
use axum::http::StatusCode;
use axum::Json;
use axum_extra::extract::cookie::{Cookie, SameSite};
use axum_extra::extract::CookieJar;
use chrono::{Duration, Utc};
use serde::{Deserialize, Serialize};

use crate::auth::{generate_session_token, hash_token, verify_password};
use crate::extractors::AdminSession;
use crate::state::AppState;

const SESSION_TTL_DAYS: i64 = 7;

#[derive(Deserialize)]
pub struct LoginRequest {
    username: String,
    password: String,
}

#[derive(Serialize)]
pub struct LoginResponse {
    username: String,
}

pub async fn login(
    State(state): State<AppState>,
    jar: CookieJar,
    Json(body): Json<LoginRequest>,
) -> Result<(CookieJar, Json<LoginResponse>), (StatusCode, &'static str)> {
    let admin = state
        .db
        .find_admin_by_username(&body.username)
        .await
        .map_err(|_| (StatusCode::INTERNAL_SERVER_ERROR, "lookup failed"))?
        .ok_or((StatusCode::UNAUTHORIZED, "invalid credentials"))?;

    if !verify_password(&body.password, &admin.password_hash) {
        return Err((StatusCode::UNAUTHORIZED, "invalid credentials"));
    }

    let (raw_token, token_hash) = generate_session_token();
    let expires_at = Utc::now() + Duration::days(SESSION_TTL_DAYS);
    state
        .db
        .create_session(&admin.id, &token_hash, expires_at)
        .await
        .map_err(|_| (StatusCode::INTERNAL_SERVER_ERROR, "session creation failed"))?;

    let cookie = Cookie::build((state.cookie_name, raw_token))
        .http_only(true)
        .secure(state.secure_cookies)
        .same_site(SameSite::Lax)
        .path("/")
        .max_age(time::Duration::days(SESSION_TTL_DAYS))
        .build();

    Ok((jar.add(cookie), Json(LoginResponse { username: admin.username })))
}

pub async fn logout(State(state): State<AppState>, jar: CookieJar) -> Result<CookieJar, StatusCode> {
    if let Some(cookie) = jar.get(state.cookie_name) {
        let token_hash = hash_token(cookie.value());
        let _ = state.db.delete_session(&token_hash).await;
    }
    Ok(jar.remove(Cookie::from(state.cookie_name)))
}

pub async fn me(session: AdminSession) -> Json<serde_json::Value> {
    Json(serde_json::json!({ "admin_id": session.admin_id }))
}
