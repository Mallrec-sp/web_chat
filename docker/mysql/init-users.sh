#!/bin/bash
set -euo pipefail

# 由官方 MySQL entrypoint 在空数据卷的首次启动时执行。
# 数据库名用于标识符，严格限制；密码按十六进制传入 SQL，不拼接未转义字面量。
if [[ ! ${MYSQL_DATABASE:-} =~ ^[a-zA-Z][a-zA-Z0-9_]{0,63}$ ]]; then
    printf '%s\n' 'Invalid MYSQL_DATABASE' >&2
    exit 1
fi
: "${MYSQL_ROOT_PASSWORD:?required}"
: "${NETCHAT_APP_PASSWORD:?required}"
: "${NETCHAT_MIGRATION_PASSWORD:?required}"
app_hex=$(printf '%s' "$NETCHAT_APP_PASSWORD" | od -An -tx1 | tr -d ' \n')
migration_hex=$(printf '%s' "$NETCHAT_MIGRATION_PASSWORD" | od -An -tx1 | tr -d ' \n')

MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --protocol=socket -u root <<SQL
SET @app_password = CONVERT(0x${app_hex} USING utf8mb4);
SET @migration_password = CONVERT(0x${migration_hex} USING utf8mb4);
SET @create_app = CONCAT('CREATE USER \'netchat_app\'@\'%\' IDENTIFIED BY ', QUOTE(@app_password), ' REQUIRE SSL');
PREPARE create_account FROM @create_app;
EXECUTE create_account;
DEALLOCATE PREPARE create_account;
SET @create_migration = CONCAT('CREATE USER \'netchat_migrator\'@\'%\' IDENTIFIED BY ', QUOTE(@migration_password), ' REQUIRE SSL');
PREPARE create_account FROM @create_migration;
EXECUTE create_account;
DEALLOCATE PREPARE create_account;
GRANT SELECT, INSERT, UPDATE, DELETE ON \`${MYSQL_DATABASE}\`.* TO 'netchat_app'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, ALTER, INDEX, DROP, REFERENCES ON \`${MYSQL_DATABASE}\`.* TO 'netchat_migrator'@'%';
SQL
