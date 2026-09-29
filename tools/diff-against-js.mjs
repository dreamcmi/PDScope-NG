#!/usr/bin/env node
/**
 * diff-against-js.mjs — 与 JS 基线（`../PDScope/tools/cli.js`）做**逐字节** CSV 差分
 *
 * 迁移计划 §2 的验收口径：
 *   「用同一批样例与 JS 输出差分。CSV 需逐字节核对。」
 *
 * 为什么盯 CSV：
 *   · CSV 是三个出口（界面「另存为」、桌面版命令行、独立 CLI）**共用**的那份实现，
 *     它的每一列都直接映射到报文的语义字段（序号 / 有序集 / 类型 / ID / 方向 /
 *     对象数 / 时间 / VBUS·IBUS / 数据字节 / CRC / 摘要）——
 *     它一字节不差，等于这些字段与基线逐字符一致；
 *   · 反过来，只要有一个字段的**格式**（补零、大小写、小数位、时间戳写法）漂了，
 *     这里立刻红。实测已经靠它抓到过：`\r\r\n`、`SID`/`SVID`、不补零十六进制被截断、
 *     以及「命令行导出误套筛选」四类问题。
 *
 * JSON 不在这里比：JS 的 `cli.js --json` 直接 dump 内部对象，形状与 C++ 的
 * FFI 对外 schema 本就不同（连字段集都不一样），逐字节没有意义。
 *
 * 用法：
 *   node tools/diff-against-js.mjs                 # 自动扫上级目录的 *.atkcc / *.sqlite
 *   node tools/diff-against-js.mjs a.atkcc b.sqlite
 *   node tools/diff-against-js.mjs --js ../PDScope --bin build/out/pdscope-cli.exe
 *   node tools/diff-against-js.mjs --keep          # 保留临时 CSV 便于肉眼比对
 *
 * 退出码：0 = 全部一致（或基线不存在，跳过）；1 = 有差异或运行失败。
 */
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const NG_ROOT = path.resolve(HERE, '..');          // PDScope-NG/
const SAMPLES_DIR = path.resolve(NG_ROOT, '..');   // ATKDecom/（抓包样本都在这一层）

/* ────────────────────────── 参数 ────────────────────────── */

const args = process.argv.slice(2);
let jsDir = path.resolve(NG_ROOT, '..', 'PDScope');
let bin = path.join(NG_ROOT, 'build', 'out',
                    process.platform === 'win32' ? 'pdscope-cli.exe' : 'pdscope-cli');
let keep = false;
const files = [];

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--js') jsDir = path.resolve(process.cwd(), args[++i]);
  else if (a === '--bin') bin = path.resolve(process.cwd(), args[++i]);
  else if (a === '--keep') keep = true;
  else if (a === '-h' || a === '--help') {
    console.log(readFileSync(fileURLToPath(import.meta.url), 'utf8').split('*/')[0].replace(/^\/\*\*?/, ''));
    process.exit(0);
  } else files.push(path.resolve(process.cwd(), a));
}

/* ────────────────────────── 前置检查 ────────────────────────── */

const jsCli = path.join(jsDir, 'tools', 'cli.js');
if (!existsSync(jsCli)) {
  console.log(`[skip] 找不到 JS 基线 ${jsCli} —— 本机没装基线时跳过差分（不算失败）`);
  process.exit(0);
}
if (!existsSync(bin)) {
  console.error(`[fail] 找不到 C++ CLI：${bin}\n       先构建：cmake --build build`);
  process.exit(1);
}

if (files.length === 0) {
  for (const f of readdirSync(SAMPLES_DIR)) {
    const l = f.toLowerCase();
    // `.pdStream` 不进默认扫描：样本目录里没有实测文件，只有 tools/ 生成的合成件
    if (l.endsWith('.atkcc') || l.endsWith('.sqlite')) files.push(path.join(SAMPLES_DIR, f));
  }
}
if (files.length === 0) {
  console.error('[fail] 没有可比的样本文件');
  process.exit(1);
}

/* ────────────────────────── 跑两个 CLI ────────────────────────── */

const MAXBUF = 64 * 1024 * 1024;   // UFCS 的 CSV 有 4 MB+，默认 1 MB 会爆

function run(cmd, argv) {
  return execFileSync(cmd, argv, { maxBuffer: MAXBUF, stdio: ['ignore', 'pipe', 'pipe'] });
}

/** JS 基线：`--csv` 打到标准输出，本来就不带 BOM。 */
function jsCsv(file) {
  return run(process.execPath, [jsCli, file, '--csv']);
}

/**
 * C++ CLI：`--csv -` 打到标准输出。**必须 `--no-bom`** —— 输出被重定向到文件时
 * 它默认会按「落盘」加 BOM，而基线这条路径是不带 BOM 的。
 */
function cppCsv(file) {
  return run(bin, [file, '--csv', '-', '--no-bom']);
}

/* ────────────────────────── 比对 ────────────────────────── */

/** 找到第一处不同的字节偏移；完全一致返回 -1。 */
function firstDiff(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return a.length === b.length ? -1 : n;
}

/** 把偏移换算成「第几行」，并给出该行两边的内容（截断），用于肉眼定位。 */
function describe(a, b, off) {
  const lineOf = (buf, at) => {
    let n = 1;
    for (let i = 0; i < at && i < buf.length; i++) if (buf[i] === 0x0A) n++;
    return n;
  };
  const lineText = (buf, ln) => {
    const s = buf.toString('utf8').split(/\r?\n/);
    return (s[ln - 1] ?? '').slice(0, 200);
  };
  const la = lineOf(a, off), lb = lineOf(b, off);
  const lines = [`        首个差异字节偏移 ${off}（JS 第 ${la} 行 / C++ 第 ${lb} 行）`];
  lines.push(`        JS  : ${lineText(a, la)}`);
  lines.push(`        C++ : ${lineText(b, lb)}`);
  return lines.join('\n');
}

const tmp = mkdtempSync(path.join(tmpdir(), 'pdscope-diff-'));
let pass = 0, fail = 0;

try {
  for (const file of files) {
    const name = path.basename(file);
    let a, b;
    try {
      a = jsCsv(file);
    } catch (e) {
      console.log(`  ⚠ ${name}  JS 基线自己就没跑通，跳过（${String(e.stderr || e.message).trim().slice(0, 120)}）`);
      continue;
    }
    try {
      b = cppCsv(file);
    } catch (e) {
      console.log(`  ✗ ${name}  C++ CLI 失败：${String(e.stderr || e.message).trim().slice(0, 200)}`);
      fail++;
      continue;
    }

    const off = firstDiff(a, b);
    if (off < 0) {
      console.log(`  ✓ ${name}  ${b.length} 字节逐字节一致`);
      pass++;
    } else {
      console.log(`  ✗ ${name}  JS=${a.length} C++=${b.length} 字节`);
      console.log(describe(a, b, off));
      if (keep) {
        const pa = path.join(tmp, `js-${name}.csv`);
        const pb = path.join(tmp, `cpp-${name}.csv`);
        writeFileSync(pa, a);
        writeFileSync(pb, b);
        console.log(`        对比文件：${pa}\n                  ${pb}`);
      }
      fail++;
    }
  }
} finally {
  if (!keep) rmSync(tmp, { recursive: true, force: true });
}

console.log(`\n──── CSV 逐字节差分：一致 ${pass} / 差异 ${fail} ────`);
process.exit(fail === 0 ? 0 : 1);
