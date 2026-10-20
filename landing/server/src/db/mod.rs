mod sqlite;

pub use sqlite::SqliteDb;

use async_trait::async_trait;
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};

pub type DbResult<T> = Result<T, sqlx::Error>;

#[derive(Debug, Clone, Serialize)]
pub struct AdminUser {
    pub id: String,
    pub username: String,
    pub password_hash: String,
}

#[derive(Debug, Clone)]
pub struct Session {
    pub admin_id: String,
    pub expires_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Post {
    pub id: String,
    pub slug: String,
    pub title: String,
    pub excerpt: String,
    pub content: String,
    pub cover_image: Option<String>,
    pub category: Option<String>,
    pub tags: String,
    pub published: bool,
    pub published_at: Option<DateTime<Utc>>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct NewPost {
    pub slug: String,
    pub title: String,
    pub excerpt: String,
    pub content: String,
    pub cover_image: Option<String>,
    pub category: Option<String>,
    #[serde(default)]
    pub tags: String,
    pub published: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct UpdatePost {
    pub slug: String,
    pub title: String,
    pub excerpt: String,
    pub content: String,
    pub cover_image: Option<String>,
    pub category: Option<String>,
    #[serde(default)]
    pub tags: String,
    pub published: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct Order {
    pub id: String,
    pub customer_name: String,
    pub phone: String,
    pub email: Option<String>,
    pub address: Option<String>,
    pub quantity: i64,
    pub note: Option<String>,
    pub status: String,
    pub created_at: DateTime<Utc>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct NewOrder {
    pub customer_name: String,
    pub phone: String,
    pub email: Option<String>,
    pub address: Option<String>,
    pub quantity: i64,
    pub note: Option<String>,
}

/// Storage abstraction for the CMS. Implemented today against SQLite
/// (`SqliteDb`); swapping to Postgres later means writing a `PgDb` that
/// implements this same trait and changing only the bootstrap in `main.rs`.
#[async_trait]
pub trait Database: Send + Sync {
    async fn find_admin_by_username(&self, username: &str) -> DbResult<Option<AdminUser>>;

    async fn create_session(&self, admin_id: &str, token_hash: &str, expires_at: DateTime<Utc>) -> DbResult<()>;
    async fn find_session(&self, token_hash: &str) -> DbResult<Option<Session>>;
    async fn delete_session(&self, token_hash: &str) -> DbResult<()>;

    async fn list_published_posts(&self) -> DbResult<Vec<Post>>;
    async fn list_all_posts(&self) -> DbResult<Vec<Post>>;
    async fn get_post(&self, id: &str) -> DbResult<Option<Post>>;
    async fn create_post(&self, input: NewPost) -> DbResult<Post>;
    async fn update_post(&self, id: &str, input: UpdatePost) -> DbResult<Option<Post>>;
    async fn delete_post(&self, id: &str) -> DbResult<bool>;

    async fn create_order(&self, input: NewOrder) -> DbResult<Order>;
    async fn list_orders(&self) -> DbResult<Vec<Order>>;
    async fn update_order_status(&self, id: &str, status: &str) -> DbResult<Option<Order>>;
}
