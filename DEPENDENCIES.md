# catsena 依赖清单（来源说明）

本文档说明 catsena（TheMap 图论核心）编译/链接所需的全部依赖及其来源。
目标: 库可打包分发 —— `.a`(静态) / `.dll`(Windows 动态) / `.so`(Linux, 示例注释)。

## 1. 内嵌依赖（随源码一起分发）

| 依赖 | 版本 | 来源 | 许可 | 位置 |
|------|------|------|------|------|
| cJSON | 1.7.19 | https://github.com/DaveGamble/cJSON (tag cJSON-1.7.19) | MIT | `thirdparty/cjson/`（cJSON.c / cJSON.h / LICENSE） |

- 内嵌原因：cJSON 是单文件 C 库，MIT 许可允许随源分发；避免外部构建依赖。
- 升级方式：替换 `thirdparty/cjson/cJSON.c|h`（保持 API 兼容即可）。

## 2. 外部库（链接时提供）

| 依赖 | 用途 | 本机来源 | 版本 | 说明 |
|------|------|----------|------|------|
| **Windows 系统库** | SRWLock / CRITICAL_SECTION / CreateThread | OS 自带（kernel32 / advapi32） | — | 无需显式链接开关 |

- ~~**OpenSSL (libcrypto)**~~ **已移除**：SHA256 现在由 `map.c` 自带实现（`sha256_ctx` 那一套），
  不再 `#include <openssl/sha.h>`。产物只依赖 **libc + 线程库**。
- **Linux 版（.so，已实测 Ubuntu 24.04 / gcc 13.3 / x86_64）**：
  - 系统依赖：**只要 gcc + libc**（pthread 自 glibc 2.34 起并入 libc，`ldd` 里看不到）
  - **不需要 `apt install libssl-dev`**（旧文档要求过，现已不成立）
  - 构建：`cd catsena && make` —— 同一份 Makefile 用 `uname -s` 自动判平台，
    Linux 出 `catsena.so`、Windows 出 `catsena.dll`，并跑全部自测；
    `make so` 只出库，`make test` 只跑测试，`make info` 打印识别到的平台与链接参数
  - Windows 与 POSIX 差异全部在 `platform.h`(锁/条件变量/线程封装)
  - **实测（2026-10-29）**：maptest / verify-sha256 / test-id / test-edge **全 PASS**；
    并发 8×800 → 索引开 81%、索引关 15% 伸缩效率；`.so` 导出 125 个符号；
    Python ctypes 外部加载 12 项冒烟全过（含中文名 JSON 往返、1→N 边）；`ldd` 只有 libc
- 未使用（历史遗留，勿加回来）：zlib（gzip 存档未实现）、libcurl（网络索引在旧内核）、ws2_32/crypt32/bcrypt（旧内核 server/graph 用）。

## 3. 编译环境

- **Windows**：MinGW-w64 gcc ≥ 13（本机 `D:\mingw\mingw64` 15.1）或 MSVC（需适配 `_WIN32` 宏，暂用 MinGW）。
- **内核构建请用 `catsena/Makefile`**（跨平台、只依赖 cJSON+libc）；
  上级 `../Makefile` 是**旧的 Windows 应用**（graph.c + curl + openssl），与内核无关。

## 4. 头文件依赖树

```
map.h          → mapdef.h, mapworker.h
mapdef.h       → 无(tags: 仅 C 标准头 + 自绘类型)
mapworker.h    → mapdef.h
map.c          → map.h, mapworker.h, thirdparty/cjson/cJSON.h, openssl/sha.h
mapworker.c    → mapworker.h, map.h, windows.h
```

## 5. 打包产物

`make package_catsena` 产出 `catsena-dist/`：
```
catsena-dist/
├── lib/catsena.a        # 静态库
├── bin/catsena.dll      # Windows 动态库(Python/其他语言加载)
├── include/               # map.h mapdef.h mapworker.h
├── thirdparty/cjson/      # cJSON(源码, 许可)
├── DEPENDENCIES.md        # 本文档
└── maptest.exe            # 自测程序(验证库可用)
```
