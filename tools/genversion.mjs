// catsena/tools/genversion.mjs —— 从 VERSION(+git) 生成 version.h
//
//   node tools/genversion.mjs            # 写 version.h（内容没变就不动文件，免得每次全量重编）
//   node tools/genversion.mjs --print    # 只打印这次会生成的内容
//
// 为什么用 Node 生成而不是 Makefile 里拼字符串: Windows 的 mingw32-make 用 cmd.exe 当 shell,
// sh 的 $() / date 都不可用; Node 在开发机和服务层都是必装项, 一份脚本两个平台都能跑。
//
// 生成的内容:
//   CATSENA_VERSION     VERSION 文件里的语义化版本
//   CATSENA_ABI         ABI 号（改了不兼容的接口才加）
//   CATSENA_BUILD       一行短标识: 0.1.0+20261102.2347.<sha8>
//   CATSENA_BUILDINFO   完整一行: catsena 0.1.0 (build … , git …) ABI 1
//   CATSENA_GIT         git describe --always --dirty（不是仓库就 nogit）
//   CATSENA_BUILD_DATE  ISO 时间
//   CATSENA_BUILD_ID    sha256(version|git|date) 前 8 位

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ABIFALLBACK = 1;

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const versionFile = path.join(root, 'VERSION');
const outFile = path.join(root, 'version.h');
const printOnly = process.argv.includes('--print');

if (!fs.existsSync(versionFile)) {
  console.error('[genversion] 找不到 VERSION 文件: ' + versionFile);
  process.exit(1);
}

const version = fs.readFileSync(versionFile, 'utf8')
  .split(/\r?\n/)
  .map(s => s.trim())
  .filter(s => s && !s.startsWith('#'))
  .pop();
if (!version) {
  console.error('[genversion] VERSION 文件里没有版本号');
  process.exit(1);
}
if (!/^\d+\.\d+\.\d+/.test(version)) {
  console.error('[genversion] 版本号要像 0.1.0 这样: ' + version);
  process.exit(1);
}

/* git describe（不在仓库里就 nogit） */
function gitInfo() {
  try {
    const sha = execFileSync('git', ['describe', '--always', '--dirty'], {
      cwd: root, stdio: ['ignore', 'pipe', 'ignore'], encoding: 'utf8',
    }).trim();
    return sha || 'nogit';
  } catch { return 'nogit'; }
}

/* ABI: 从 map.h 里找 CATSENA_ABI_REQUIRED，没写就用 1 */
function abi() {
  try {
    const h = fs.readFileSync(path.join(root, 'map.h'), 'utf8');
    const m = h.match(/CATSENA_ABI_REQUIRED\s+(\d+)/);
    if (m) return Number(m[1]);
  } catch {}
  return ABIFALLBACK;
}

const git = gitInfo();
const abiNum = abi();
const iso = new Date().toISOString().replace(/\.\d+Z$/, 'Z');
const sha8 = crypto.createHash('sha256').update(version + '|' + git + '|' + iso).digest('hex').slice(0, 8);
const stamp = iso.replace(/[-:]/g, '').replace('T', '.').replace('Z', '');
const build = version + '+' + stamp + '.' + sha8;
const buildinfo = 'catsena ' + version + ' (build ' + iso + ', ' + git + ') ABI ' + abiNum;

const text = `/* version.h —— 自动生成, 别手改.
   由 tools/genversion.mjs 从 VERSION + git 生成; 改版本请改 VERSION 后重新 make.
   生成时间: ${iso}
   版本:     ${version}
   git:      ${git}
   ABI:      ${abiNum}
*/
#ifndef CATSENA_VERSION_H
#define CATSENA_VERSION_H

#define CATSENA_VERSION     "${version}"
#define CATSENA_ABI         ${abiNum}
#define CATSENA_BUILD       "${build}"
#define CATSENA_BUILD_ID    "${sha8}"
#define CATSENA_BUILD_DATE  "${iso}"
#define CATSENA_GIT         "${git}"
#define CATSENA_BUILDINFO   "${buildinfo}"

#endif /* CATSENA_VERSION_H */
`;

if (printOnly) {
  process.stdout.write(text);
  process.exit(0);
}

/* 内容没变就别动文件 —— 否则每次 make 都要全量重编 */
let old = '';
try { old = fs.readFileSync(outFile, 'utf8'); } catch {}
/* 生成时间每次都不同, 但只要版本/git 没变, 就复用旧文件里的时间戳 */
const sameVersion = old.includes(`#define CATSENA_VERSION     "${version}"`)
  && old.includes(`#define CATSENA_GIT         "${git}"`);
if (sameVersion) {
  console.log('[genversion] 版本/git 没变, version.h 不动 (' + version + ' ' + git + ')');
  process.exit(0);
}
fs.writeFileSync(outFile, text, 'utf8');
console.log('[genversion] version.h ← ' + build);
