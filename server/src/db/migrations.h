#ifndef NETCHAT_MIGRATIONS_H
#define NETCHAT_MIGRATIONS_H

#include <cstdint>

class Database;

inline constexpr int64_t kSchemaVersion = 1;

// 仅由 --migrate 显式调用。MySQL DDL 会隐式提交：迁移可重跑，但不是可回滚事务。
// 连接级 GET_LOCK 序列化迁移；全部 DDL 完成后才记录版本。
// 数据库故障抛出 DbError，无法取得迁移锁或版本不兼容时返回 false。
bool runMigrations(Database& db);

// 正常启动只检查版本，不创建或修改任何表。未迁移/版本不兼容返回 false。
bool verifySchema(Database& db);

#endif // NETCHAT_MIGRATIONS_H
