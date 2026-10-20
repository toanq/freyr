use axum::extract::FromRequestParts;
use axum::http::request::Parts;
use axum::http::StatusCode;
use axum_extra::extract::CookieJar;
use chrono::Utc;

use crate::auth::hash_token;
use crate::state::AppState;

/// Proof that the request carries a valid, unexpired admin session cookie.
/// Any handler that takes this as an argument is implicitly admin-only.
pub struct AdminSession {
    pub admin_id: String,
}

impl FromRequestParts<AppState> for AdminSession {
    type Rejection = (StatusCode, &'static str);

    async fn from_request_parts(parts: &mut Parts, state: &AppState) -> Result<Self, Self::Rejection> {
        let jar = CookieJar::from_headers(&parts.headers);
        let token = jar
            .get(state.cookie_name)
            .map(|c| c.value().to_string())
            .ok_or((StatusCode::UNAUTHORIZED, "missing session cookie"))?;

        let token_hash = hash_token(&token);
        let session = state
            .db
            .find_session(&token_hash)
            .await
            .map_err(|_| (StatusCode::INTERNAL_SERVER_ERROR, "session lookup failed"))?
            .ok_or((StatusCode::UNAUTHORIZED, "invalid session"))?;

        if session.expires_at < Utc::now() {
            let _ = state.db.delete_session(&token_hash).await;
            return Err((StatusCode::UNAUTHORIZED, "session expired"));
        }

        Ok(AdminSession { admin_id: session.admin_id })
    }
}
