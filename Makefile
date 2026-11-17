# catsena/Makefile —— 内核单独构建(Windows / Linux 同一份)
# ---------------------------------------------------------------------------
# 为什么独立于上一级 Makefile: 上级那份是**旧的 Windows 应用**(graph.c + curl + openssl),
# 内核要能单独打包/单独在 Linux 上编译。这里只依赖 cJSON(已内嵌) + libc + 线程库。
#
#   make              构建共享库 + 跑全部自测
#   make so           只构建共享库(catsena.so / catsena.dll)
#   make test         跑四个测试套件
#   make clean
#
# 交叉平台要点:
#   Linux 需要 -D_GNU_SOURCE(strtok_r/pthread_rwlock 才是标准声明)
#   Windows 需要 -lws2_32 -lbcrypt(平台层用的 crypto API)
#   导出: Linux 默认导出; Windows 要 -Wl,--export-all-symbols 才能被 koffi 动态加载

# Windows 下 $(OS) 系统已经给了, 不必去 shell 试 uname(cmd 不认 2>/dev/null, 会打印噪音)
ifeq ($(OS),Windows_NT)
  UNAME := Windows
else
  UNAME := $(shell uname -s 2>/dev/null || echo Windows)
endif

SRC      := map.c mapworker.c utils.c thirdparty/cjson/cJSON.c
HEADERS  := map.h mapdef.h mapworker.h utils.h platform.h version.h thirdparty/cjson/cJSON.h
# 版本的唯一真相: VERSION 文件; version.h 由它(和 git)生成
VERSION_SRC := VERSION tools/genversion.mjs
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra
# CJSON_HIDE_SYMBOLS: cJSON 1.7.x 默认把每个函数标 __declspec(dllexport),
# 那会绕过 catsena.def 把 78 个 cJSON 符号塞进我们的 ABI。定义它 → 退化成普通声明。
CPPFLAGS := -I. -DCJSON_HIDE_SYMBOLS

ifeq ($(findstring MINGW,$(UNAME)$(OS)),MINGW)
  OS_KIND := win
else ifeq ($(findstring MSYS,$(UNAME)$(OS)),MSYS)
  OS_KIND := win
else ifeq ($(OS),Windows_NT)
  OS_KIND := win
else ifeq ($(UNAME),Windows)
  OS_KIND := win
else
  OS_KIND := posix
endif

ifeq ($(OS_KIND),win)
  LIB      := catsena.dll
  EXE      := .exe
  SHARED   := -shared                # 不写 --export-all-symbols: 那会把 cJSON 一起导出
  LIBS     := -lws2_32 -lbcrypt
  PIC      :=
  RUN      :=                        # cmd.exe 认裸文件名(先查当前目录)
  RM       := del /Q
  RMERR    := 2>nul
  DEFARG   := -Wl,catsena.def -Wl,--exclude-all-symbols   # 只导出 .def 点名的符号
else
  LIB      := catsena.so
  EXE      :=
  SHARED   := -shared -fPIC
  LIBS     := -lpthread
  PIC      := -fPIC
  RUN      := ./                     # sh 必须带 ./
  RM       := rm -f
  RMERR    :=
  DEFARG   := -Wl,--version-script=catsena.map           # .def 的 Linux 等价物
  CPPFLAGS += -D_GNU_SOURCE
endif

TESTS := maptest$(EXE) verify-sha256$(EXE) test-id$(EXE) test-edge$(EXE) test-utils$(EXE) test-mt$(EXE) test-version$(EXE)

.PHONY: all so test clean exports info version clean-version deploy dist
all: so test

exports:
	@python tools/genexports.py 2>/dev/null || python3 tools/genexports.py

info:
	@echo "OS_KIND=$(OS_KIND)  LIB=$(LIB)  LIBS=$(LIBS)"
	@node -e "const t=require('fs').readFileSync('version.h','utf8');for(const l of t.split(/\r?\n/))if(/^#define CATSENA_(VERSION|GIT|BUILD|ABI) /.test(l))console.log('  '+l.replace(/^#define +/,''))"

# ---------- 版本 ----------
# version.h 从 VERSION + git 生成; 内容没变就不动文件(不会引发全量重编)。
# 用 Node 而不是 sh: Windows 的 mingw32-make 用 cmd.exe 当 shell, $(date) 那套不可用。
version.h: $(VERSION_SRC)
	@node tools/genversion.mjs

# 只打印将要生成的版本信息(不落盘)
version:
	@node tools/genversion.mjs --print

so: $(LIB)

$(LIB): $(SRC) $(HEADERS) catsena.def catsena.map
	gcc $(CFLAGS) $(CPPFLAGS) $(SHARED) $(SRC) $(DEFARG) $(LIBS) -o $(LIB)

# 每个测试独立编译(测试之间有 main 冲突, 不能合到一起)
maptest$(EXE): maptest.c $(SRC) $(HEADERS)
	gcc $(CFLAGS) $(CPPFLAGS) $(SRC) maptest.c $(LIBS) -o $@
verify-sha256$(EXE): verify-sha256.c $(SRC) $(HEADERS)
	gcc $(CFLAGS) $(CPPFLAGS) $(SRC) verify-sha256.c $(LIBS) -o $@
test-id$(EXE): test-id.c $(SRC) $(HEADERS)
	gcc $(CFLAGS) $(CPPFLAGS) $(SRC) test-id.c $(LIBS) -o $@
test-edge$(EXE): test-edge.c $(SRC) $(HEADERS)
	gcc $(CFLAGS) $(CPPFLAGS) $(SRC) test-edge.c $(LIBS) -o $@
test-utils$(EXE): test-utils.c $(SRC) $(HEADERS)
	gcc $(CFLAGS) $(CPPFLAGS) $(SRC) test-utils.c $(LIBS) -o $@
test-mt$(EXE): test-mt.c $(SRC) $(HEADERS)
	gcc $(CFLAGS) $(CPPFLAGS) $(SRC) test-mt.c $(LIBS) -o $@
test-version$(EXE): test-version.c $(SRC) $(HEADERS)
	gcc $(CFLAGS) $(CPPFLAGS) $(SRC) test-version.c $(LIBS) -o $@

test: $(TESTS)
	@echo ===== maptest =====
	@$(RUN)maptest$(EXE)
	@echo ===== sha256 =====
	@$(RUN)verify-sha256$(EXE)
	@echo ===== id/uuid =====
	@$(RUN)test-id$(EXE)
	@echo ===== edge 1-N and shared =====
	@$(RUN)test-edge$(EXE)
	@echo ===== utils =====
	@$(RUN)test-utils$(EXE)
	@echo ===== threads =====
	@$(RUN)test-mt$(EXE) 8 800 1
	@echo ===== version =====
	@$(RUN)test-version$(EXE)

# 把库同步到上层目录(服务层 resolveEngineLib 先找 '../' 那一份, 不 sync 会用到旧 DLL)
# 用 node 拷贝: sh/cmd 两种 shell 下行为一样, 失败(库被占用)时给出人话提示
deploy: $(LIB)
	@node -e "const fs=require('fs');try{fs.copyFileSync(process.argv[1],process.argv[2]);console.log('deploy: '+process.argv[2])}catch(e){console.error('deploy 失败: '+e.message+' —— 库多半被占用, 先停 node server.mjs 再试');process.exit(1)}" $(LIB) ../$(LIB)

# ---------- 打包 ----------
# dist/include + dist/lib + dist/bin + dist/thirdparty —— 给别人用的最小集合。
# 用 node 做拷贝: mingw32-make 在 Windows 用 cmd.exe 当 shell, 而 Linux 用 sh,
# mkdir/cp 的参数不通用(node 两边行为一样, 且 version.h 本来就依赖 node)。
DIST := dist

dist: $(LIB)
	@node -e "const fs=require('fs'),p=require('path');const d='dist';['include','lib','bin','thirdparty/cjson'].forEach(x=>fs.mkdirSync(p.join(d,x),{recursive:true}));['map.h','mapdef.h','mapworker.h','utils.h','platform.h','version.h'].forEach(f=>fs.copyFileSync(f,p.join(d,'include',f)));fs.copyFileSync('$(LIB)',p.join(d,'lib','$(LIB)'));fs.copyFileSync('$(LIB)',p.join(d,'bin','$(LIB)'));fs.copyFileSync('thirdparty/cjson/LICENSE',p.join(d,'thirdparty/cjson','LICENSE'));['LICENSE','README.md','API.md','DEPENDENCIES.md'].forEach(f=>fs.copyFileSync(f,p.join(d,f)));console.log('dist/: include('+fs.readdirSync(p.join(d,'include')).length+') lib bin thirdparty 已生成')"
# clean 用 $(RM)/$(RMERR): cmd 下是 del /Q ... 2>nul, sh 下是 rm -f —— 同一份 Makefile 两边都能跑。
# 前缀 '-' 表示"文件本来就不存在"不算错误。
clean:
	-$(RM) $(LIB) $(TESTS) *.o *.exe *.dll *.so test-*.map _rt.map $(RMERR)

# 版本头也清掉(下次 make 会用新时间戳重新生成)
clean-version:
	-$(RM) version.h $(RMERR)
