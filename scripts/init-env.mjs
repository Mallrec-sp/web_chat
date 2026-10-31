import { randomBytes } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

// 只创建，不覆盖已有密钥；权限限制为当前用户读写，不打印密码。
const path = fileURLToPath(new URL('../.env', import.meta.url));
const content = [
  `MYSQL_ROOT_PASSWORD=${randomBytes(32).toString('hex')}`,
  `NETCHAT_APP_PASSWORD=${randomBytes(32).toString('hex')}`,
  `NETCHAT_MIGRATION_PASSWORD=${randomBytes(32).toString('hex')}`,
  'NETCHAT_MYSQL_PORT=3306',
  'NETCHAT_HTTP_PORT=8081',
  '',
].join('\n');
try {
  await writeFile(path, content, { flag: 'wx', mode: 0o600 });
  console.log('已创建 .env（权限 0600）；请妥善保存，密码未输出。');
} catch (error) {
  if (error.code === 'EEXIST') {
    console.error('.env 已存在，未覆盖。已有数据库卷的密码不能通过重新生成此文件修改。');
    process.exitCode = 1;
  } else {
    throw error;
  }
}
