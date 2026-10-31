#!/bin/bash
set -euo pipefail

# 仅 compose.test.yml 挂载。拒绝向任何非测试命名的数据库增加测试账号。
if [[ ! ${MYSQL_DATABASE:-} =~ ^netchat_test_[a-z0-9_]+$ ]]; then
    printf '%s\n' 'Refusing to initialize test roles outside netchat_test_*' >&2
    exit 1
fi
: "${MYSQL_ROOT_PASSWORD:?required}"
: "${NETCHAT_TEST_READONLY_PASSWORD:?required}"
readonly_hex=$(printf '%s' "$NETCHAT_TEST_READONLY_PASSWORD" | od -An -tx1 | tr -d ' \n')

MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --protocol=socket -u root <<SQL
SET @readonly_password = CONVERT(0x${readonly_hex} USING utf8mb4);
SET @create_readonly = CONCAT('CREATE USER \'netchat_test_readonly\'@\'%\' IDENTIFIED BY ', QUOTE(@readonly_password), ' REQUIRE SSL');
PREPARE create_account FROM @create_readonly;
EXECUTE create_account;
DEALLOCATE PREPARE create_account;
GRANT SELECT ON \`${MYSQL_DATABASE}\`.* TO 'netchat_test_readonly'@'%';
SQL
