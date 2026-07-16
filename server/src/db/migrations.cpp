#include "migrations.h"
#include "database.h"
#include "utils/logger.h"

bool runMigrations(Database& db) {
    LOG_INFO("Running database migrations...");

    // 用户表
    bool ok = db.execute(
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY,"
        "  username TEXT UNIQUE NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  nickname TEXT DEFAULT '',"
        "  created_at INTEGER NOT NULL"
        ")"
    );
    if (!ok) return false;

    // Token 表
    ok = db.execute(
        "CREATE TABLE IF NOT EXISTS tokens ("
        "  token TEXT PRIMARY KEY,"
        "  user_id INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ")"
    );
    if (!ok) return false;

    // 消息表
    ok = db.execute(
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  from_user_id INTEGER NOT NULL,"
        "  to_user_id INTEGER NOT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ")"
    );
    if (!ok) return false;

    // 索引
    db.execute("CREATE INDEX IF NOT EXISTS idx_tokens_user ON tokens(user_id)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_messages_from ON messages(from_user_id)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_messages_to ON messages(to_user_id)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_messages_time ON messages(created_at)");

    // 好友关系表
    db.execute(
        "CREATE TABLE IF NOT EXISTS friendships ("
        "  user_id INTEGER NOT NULL,"
        "  friend_id INTEGER NOT NULL,"
        "  status INTEGER DEFAULT 0,"  // 0: pending, 1: accepted
        "  created_at INTEGER NOT NULL,"
        "  PRIMARY KEY (user_id, friend_id)"
        ")"
    );
    db.execute("CREATE INDEX IF NOT EXISTS idx_friendships_user ON friendships(user_id)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_friendships_friend ON friendships(friend_id)");

    LOG_INFO("Database migrations complete");
    return true;
}
