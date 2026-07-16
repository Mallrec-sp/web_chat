#ifndef NETCHAT_MIGRATIONS_H
#define NETCHAT_MIGRATIONS_H

class Database;

/**
 * runMigrations — 初始化数据库 Schema。
 *
 * 使用 CREATE TABLE IF NOT EXISTS，幂等执行。
 * 后续可以扩展为带版本号的迁移系统。
 */
bool runMigrations(Database& db);

#endif
