ALTER TABLE posts ADD COLUMN category TEXT;
ALTER TABLE posts ADD COLUMN tags TEXT NOT NULL DEFAULT '';

CREATE INDEX idx_posts_category ON posts (category);
