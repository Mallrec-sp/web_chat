import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';

// 不加载开发 .env，不继承 Compose 项目选择，不发布 MySQL 宿主端口。
// 每轮只操作新生成的项目及其数据卷；无全局 prune、无外部卷、无 sudo。
assert(Number(process.versions.node.split('.')[0]) >= 22, '需要 Node.js 22+');
assert.equal(process.argv.length, 2, '此 runner 不接受项目名、库名或其他参数');
const root = fileURLToPath(new URL('../..', import.meta.url));
const suffix = randomBytes(10).toString('hex');
const project = `netchat-test-${suffix}`;
const database = `netchat_test_${suffix}`;
const temp = await mkdtemp(join(tmpdir(), 'netchat-mysql-tests-'));
const envFile = join(temp, 'test.env');
const generated = {
  NETCHAT_TEST_DATABASE: database,
  MYSQL_ROOT_PASSWORD: randomBytes(32).toString('hex'),
  NETCHAT_APP_PASSWORD: randomBytes(32).toString('hex'),
  NETCHAT_MIGRATION_PASSWORD: randomBytes(32).toString('hex'),
  NETCHAT_TEST_READONLY_PASSWORD: randomBytes(32).toString('hex'),
};
const environment = { ...process.env };
for (const key of Object.keys(environment)) {
  if (key.startsWith('COMPOSE_') || key.startsWith('NETCHAT_DB_')) delete environment[key];
}
Object.assign(environment, generated);
const compose = ['compose', '--project-name', project, '--project-directory', root,
  '--env-file', envFile, '-f', join(root, 'compose.test.yml')];
let cancelActive;
let interrupted;
let cleaning = false;
let envWritten = false;

function onSignal(signal) {
  if (cleaning) return; // cleanup 有独立超时，不因重复 Ctrl-C 留下项目卷。
  interrupted ||= signal;
  cancelActive?.();
}
const onInt = () => onSignal('SIGINT');
const onTerm = () => onSignal('SIGTERM');
process.on('SIGINT', onInt);
process.on('SIGTERM', onTerm);

function run(args, { timeout = 180000, capture = false, allowFailure = false } = {}) {
  if (interrupted && !cleaning) throw new Error('测试已中断');
  console.log(`\n[test ${project}] docker compose ${args.join(' ')}`);
  return new Promise((resolve, reject) => {
    const child = spawn('docker', [...compose, ...args], {
      cwd: root, env: environment,
      stdio: capture ? ['ignore', 'pipe', 'pipe'] : ['ignore', 'inherit', 'inherit'],
    });
    let output = '';
    let timedOut = false;
    let escalation;
    if (capture) {
      const collect = data => { output = (output + data).slice(-16000); };
      child.stdout.on('data', collect);
      child.stderr.on('data', collect);
    }
    const cancel = () => {
      child.kill('SIGTERM');
      escalation ||= setTimeout(() => child.kill('SIGKILL'), 5000);
    };
    cancelActive = cancel;
    const timer = setTimeout(() => { timedOut = true; cancel(); }, timeout);
    const clear = () => {
      clearTimeout(timer);
      clearTimeout(escalation);
      if (cancelActive === cancel) cancelActive = undefined;
    };
    child.once('error', error => { clear(); reject(error); });
    child.once('close', (code, signal) => {
      clear();
      if (timedOut) return reject(new Error(`Compose 步骤超时：${args[0]}`));
      if (interrupted && !cleaning) return reject(new Error('测试已中断'));
      if (signal || (code !== 0 && !allowFailure)) {
        return reject(new Error(`Compose 步骤失败：${args[0]}，code=${code}，signal=${signal}\n${output}`));
      }
      resolve({ code, output });
    });
  });
}

try {
  await writeFile(envFile, Object.entries(generated).map(([key, value]) => `${key}=${value}\n`).join(''),
    { mode: 0o600, flag: 'wx' });
  envWritten = true;
  console.log(`隔离 MySQL 测试：项目 ${project}，数据库 ${database}`);
  await run(['config', '--quiet'], { timeout: 30000 });
  await run(['build', 'migrate', 'ddl-denied', 'database-test', 'protocol-test'], { timeout: 1200000 });
  await run(['up', '-d', '--wait', '--wait-timeout', '180', 'mysql'], { timeout: 210000 });
  await run(['run', '--rm', '--no-deps', 'migrate']);
  await run(['run', '--rm', '--no-deps', 'migrate']); // 重复迁移必须幂等成功。
  const denied = await run(['run', '--rm', '--no-deps', 'ddl-denied'], { capture: true, allowFailure: true });
  assert.equal(denied.code, 1, '仅 DML 账号执行 --migrate 应失败');
  assert.match(denied.output, /Database startup failure, code=1142, state=42000/,
    '应确实由 MySQL CREATE 权限拒绝，而非连接/配置错误导致失败');
  console.log('通过：应用 DML 账号无 DDL 权限');
  await run(['run', '--rm', '--no-deps', 'database-test']);
  await run(['run', '--rm', '--no-deps', 'protocol-test'], { timeout: 240000 });
  console.log('\n隔离 MySQL 原生/协议测试全部通过。');
} catch (error) {
  console.error(error.message);
  process.exitCode = interrupted === 'SIGINT' ? 130 : interrupted === 'SIGTERM' ? 143 : 1;
} finally {
  cleaning = true;
  if (envWritten) {
    try {
      // 只删除本轮随机项目的容器/网络/卷；不引用开发 Compose，也不删除镜像缓存。
      await run(['down', '--volumes', '--remove-orphans', '--timeout', '10'], { timeout: 90000 });
    } catch (error) {
      console.error(`本轮项目清理失败（${project}）：${error.message}`);
      process.exitCode ||= 1;
    }
  }
  await rm(temp, { recursive: true, force: true });
  process.off('SIGINT', onInt);
  process.off('SIGTERM', onTerm);
}
