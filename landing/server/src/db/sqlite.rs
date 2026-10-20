use async_trait::async_trait;
use chrono::{DateTime, Utc};
use sqlx::{FromRow, SqlitePool};
use uuid::Uuid;

use super::{AdminUser, Database, DbResult, NewOrder, NewPost, Order, Post, Session, UpdatePost};

pub struct SqliteDb {
    pool: SqlitePool,
}

impl SqliteDb {
    pub async fn connect(database_url: &str) -> anyhow::Result<Self> {
        let pool = SqlitePool::connect(database_url).await?;
        sqlx::migrate!("./migrations").run(&pool).await?;
        Ok(Self { pool })
    }

    pub async fn ensure_admin(&self, username: &str, password_hash: &str) -> anyhow::Result<()> {
        let existing = self.find_admin_by_username(username).await?;
        if existing.is_some() {
            return Ok(());
        }
        let now = Utc::now().to_rfc3339();
        sqlx::query("INSERT INTO admin_users (id, username, password_hash, created_at) VALUES (?, ?, ?, ?)")
            .bind(Uuid::new_v4().to_string())
            .bind(username)
            .bind(password_hash)
            .bind(now)
            .execute(&self.pool)
            .await?;
        Ok(())
    }
}

#[derive(FromRow)]
struct PostRow {
    id: String,
    slug: String,
    title: String,
    excerpt: String,
    content: String,
    cover_image: Option<String>,
    category: Option<String>,
    tags: String,
    published: i64,
    published_at: Option<String>,
    created_at: String,
    updated_at: String,
}

impl PostRow {
    fn into_post(self) -> Post {
        Post {
            id: self.id,
            slug: self.slug,
            title: self.title,
            excerpt: self.excerpt,
            content: self.content,
            cover_image: self.cover_image,
            category: self.category,
            tags: self.tags,
            published: self.published != 0,
            published_at: self.published_at.and_then(|s| parse_dt(&s)),
            created_at: parse_dt(&self.created_at).unwrap_or_else(Utc::now),
            updated_at: parse_dt(&self.updated_at).unwrap_or_else(Utc::now),
        }
    }
}

#[derive(FromRow)]
struct OrderRow {
    id: String,
    customer_name: String,
    phone: String,
    email: Option<String>,
    address: Option<String>,
    quantity: i64,
    note: Option<String>,
    status: String,
    created_at: String,
}

impl OrderRow {
    fn into_order(self) -> Order {
        Order {
            id: self.id,
            customer_name: self.customer_name,
            phone: self.phone,
            email: self.email,
            address: self.address,
            quantity: self.quantity,
            note: self.note,
            status: self.status,
            created_at: parse_dt(&self.created_at).unwrap_or_else(Utc::now),
        }
    }
}

fn parse_dt(raw: &str) -> Option<DateTime<Utc>> {
    DateTime::parse_from_rfc3339(raw).ok().map(|dt| dt.with_timezone(&Utc))
}

#[async_trait]
impl Database for SqliteDb {
    async fn find_admin_by_username(&self, username: &str) -> DbResult<Option<AdminUser>> {
        sqlx::query_as::<_, (String, String, String)>(
            "SELECT id, username, password_hash FROM admin_users WHERE username = ?",
        )
        .bind(username)
        .fetch_optional(&self.pool)
        .await
        .map(|row| row.map(|(id, username, password_hash)| AdminUser { id, username, password_hash }))
    }

    async fn create_session(&self, admin_id: &str, token_hash: &str, expires_at: DateTime<Utc>) -> DbResult<()> {
        sqlx::query("INSERT INTO sessions (token_hash, admin_id, expires_at, created_at) VALUES (?, ?, ?, ?)")
            .bind(token_hash)
            .bind(admin_id)
            .bind(expires_at.to_rfc3339())
            .bind(Utc::now().to_rfc3339())
            .execute(&self.pool)
            .await?;
        Ok(())
    }

    async fn find_session(&self, token_hash: &str) -> DbResult<Option<Session>> {
        let row = sqlx::query_as::<_, (String, String, String)>(
            "SELECT token_hash, admin_id, expires_at FROM sessions WHERE token_hash = ?",
        )
        .bind(token_hash)
        .fetch_optional(&self.pool)
        .await?;
        Ok(row.and_then(|(_token_hash, admin_id, expires_at)| {
            parse_dt(&expires_at).map(|expires_at| Session { admin_id, expires_at })
        }))
    }

    async fn delete_session(&self, token_hash: &str) -> DbResult<()> {
        sqlx::query("DELETE FROM sessions WHERE token_hash = ?")
            .bind(token_hash)
            .execute(&self.pool)
            .await?;
        Ok(())
    }

    async fn list_published_posts(&self) -> DbResult<Vec<Post>> {
        let rows = sqlx::query_as::<_, PostRow>(
            "SELECT id, slug, title, excerpt, content, cover_image, category, tags, published, published_at, created_at, updated_at
             FROM posts WHERE published = 1 ORDER BY published_at DESC, created_at DESC",
        )
        .fetch_all(&self.pool)
        .await?;
        Ok(rows.into_iter().map(PostRow::into_post).collect())
    }

    async fn list_all_posts(&self) -> DbResult<Vec<Post>> {
        let rows = sqlx::query_as::<_, PostRow>(
            "SELECT id, slug, title, excerpt, content, cover_image, category, tags, published, published_at, created_at, updated_at
             FROM posts ORDER BY created_at DESC",
        )
        .fetch_all(&self.pool)
        .await?;
        Ok(rows.into_iter().map(PostRow::into_post).collect())
    }

    async fn get_post(&self, id: &str) -> DbResult<Option<Post>> {
        let row = sqlx::query_as::<_, PostRow>(
            "SELECT id, slug, title, excerpt, content, cover_image, category, tags, published, published_at, created_at, updated_at
             FROM posts WHERE id = ?",
        )
        .bind(id)
        .fetch_optional(&self.pool)
        .await?;
        Ok(row.map(PostRow::into_post))
    }

    async fn create_post(&self, input: NewPost) -> DbResult<Post> {
        let id = Uuid::new_v4().to_string();
        let now = Utc::now();
        let now_str = now.to_rfc3339();
        let published_at = if input.published { Some(now_str.clone()) } else { None };
        sqlx::query(
            "INSERT INTO posts (id, slug, title, excerpt, content, cover_image, category, tags, published, published_at, created_at, updated_at)
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        )
        .bind(&id)
        .bind(&input.slug)
        .bind(&input.title)
        .bind(&input.excerpt)
        .bind(&input.content)
        .bind(&input.cover_image)
        .bind(&input.category)
        .bind(&input.tags)
        .bind(input.published as i64)
        .bind(&published_at)
        .bind(&now_str)
        .bind(&now_str)
        .execute(&self.pool)
        .await?;

        Ok(Post {
            id,
            slug: input.slug,
            title: input.title,
            excerpt: input.excerpt,
            content: input.content,
            cover_image: input.cover_image,
            category: input.category,
            tags: input.tags,
            published: input.published,
            published_at: published_at.and_then(|s| parse_dt(&s)),
            created_at: now,
            updated_at: now,
        })
    }

    async fn update_post(&self, id: &str, input: UpdatePost) -> DbResult<Option<Post>> {
        let existing = self.get_post(id).await?;
        let Some(existing) = existing else { return Ok(None) };

        let now = Utc::now();
        let now_str = now.to_rfc3339();
        let published_at = if input.published {
            existing.published_at.map(|d| d.to_rfc3339()).unwrap_or_else(|| now_str.clone())
        } else {
            String::new()
        };
        let published_at_opt = if input.published { Some(published_at) } else { None };

        sqlx::query(
            "UPDATE posts SET slug = ?, title = ?, excerpt = ?, content = ?, cover_image = ?, category = ?, tags = ?,
             published = ?, published_at = ?, updated_at = ? WHERE id = ?",
        )
        .bind(&input.slug)
        .bind(&input.title)
        .bind(&input.excerpt)
        .bind(&input.content)
        .bind(&input.cover_image)
        .bind(&input.category)
        .bind(&input.tags)
        .bind(input.published as i64)
        .bind(&published_at_opt)
        .bind(&now_str)
        .bind(id)
        .execute(&self.pool)
        .await?;

        self.get_post(id).await
    }

    async fn delete_post(&self, id: &str) -> DbResult<bool> {
        let result = sqlx::query("DELETE FROM posts WHERE id = ?").bind(id).execute(&self.pool).await?;
        Ok(result.rows_affected() > 0)
    }

    async fn create_order(&self, input: NewOrder) -> DbResult<Order> {
        let id = Uuid::new_v4().to_string();
        let now = Utc::now();
        let now_str = now.to_rfc3339();
        let status = "new".to_string();
        sqlx::query(
            "INSERT INTO orders (id, customer_name, phone, email, address, quantity, note, status, created_at)
             VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        )
        .bind(&id)
        .bind(&input.customer_name)
        .bind(&input.phone)
        .bind(&input.email)
        .bind(&input.address)
        .bind(input.quantity)
        .bind(&input.note)
        .bind(&status)
        .bind(&now_str)
        .execute(&self.pool)
        .await?;

        Ok(Order {
            id,
            customer_name: input.customer_name,
            phone: input.phone,
            email: input.email,
            address: input.address,
            quantity: input.quantity,
            note: input.note,
            status,
            created_at: now,
        })
    }

    async fn list_orders(&self) -> DbResult<Vec<Order>> {
        let rows = sqlx::query_as::<_, OrderRow>(
            "SELECT id, customer_name, phone, email, address, quantity, note, status, created_at
             FROM orders ORDER BY created_at DESC",
        )
        .fetch_all(&self.pool)
        .await?;
        Ok(rows.into_iter().map(OrderRow::into_order).collect())
    }

    async fn update_order_status(&self, id: &str, status: &str) -> DbResult<Option<Order>> {
        sqlx::query("UPDATE orders SET status = ? WHERE id = ?")
            .bind(status)
            .bind(id)
            .execute(&self.pool)
            .await?;

        let row = sqlx::query_as::<_, OrderRow>(
            "SELECT id, customer_name, phone, email, address, quantity, note, status, created_at
             FROM orders WHERE id = ?",
        )
        .bind(id)
        .fetch_optional(&self.pool)
        .await?;
        Ok(row.map(OrderRow::into_order))
    }
}
