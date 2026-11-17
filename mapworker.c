// mapworker.c —— catsena 并发骨架: 命令队列 + worker 线程池(跨平台)
// ============================================================================
// max_workers=0: engine_submit 同步执行(= map_exec 直调), 应用端自行串行。
// max_workers>0: 启动 N 个 worker 线程, 命令入队后由空闲线程取出执行
//   (map_exec 内部带图级读写锁 —— 写命令排他, 读命令共享)。
// 同步原语经 platform.h 抽象: Win32(CRITICAL_SECTION+CONDITION_VARIABLE) /
//   POSIX(pthread_mutex+pthread_cond), 见 catsena/platform.h。
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mapworker.h"
#include "map.h"
#include "platform.h"

/* ---- worker 线程主循环 ---- */

typedef struct worker_arg { map_engine* e; int id; } worker_arg;

static MAP_THREAD_RET worker_loop(LPVOID arg) {
    worker_arg* wa = (worker_arg*)arg;
    map_engine* e = wa->e;
    int wid = wa->id;
    (void)wid;
    free(wa);
    for (;;) {
        map_cmd* cmd = NULL;
        map* m = NULL;
        map_mutex_lock((map_mutex_t*)e->queue_lock);
        // 等命令或停止信号
        while (e->queue_head == NULL && !e->stopping)
            map_cond_wait((map_cond_t*)e->queue_signal, (map_mutex_t*)e->queue_lock);
        if (e->stopping && e->queue_head == NULL) {
            map_mutex_unlock((map_mutex_t*)e->queue_lock);
            break;
        }
        cmd = e->queue_head;
        if (cmd != NULL) {
            e->queue_head = cmd->next;
            if (e->queue_head == NULL) e->queue_tail = NULL;
            e->queue_len--;
        }
        // 找命令所属图(命令挂在第一个挂载图上; 多图按 m 匹配)
        m = e->maps && e->map_count > 0 ? e->maps[0] : NULL;
        map_mutex_unlock((map_mutex_t*)e->queue_lock);

        if (cmd == NULL) continue;
        if (m != NULL) {
            int rc = map_exec(m, cmd->text, cmd->out, cmd->outsz);
            if (cmd->done != NULL) cmd->done(cmd->ud, rc);
        } else if (cmd->done != NULL) {
            cmd->done(cmd->ud, -1);
        }
        free(cmd->text);
        free(cmd);
    }
    MAP_THREAD_RETURN(0);
}

/* ---- 引擎生命周期 ---- */

map_engine* map_engine_create(const MapOptions* opts) {
    map_engine* e = (map_engine*)calloc(1, sizeof(map_engine));
    if (e == NULL) return NULL;
    if (opts != NULL) e->options = *opts;
    if (e->options.max_workers <= 0) return e;   // 单线程串行模式: 无队列设施

    // 队列同步原语(平台抽象)
    map_mutex_t* ms = (map_mutex_t*)calloc(1, sizeof(map_mutex_t));
    map_cond_t*  cv = (map_cond_t*)calloc(1, sizeof(map_cond_t));
    if (ms == NULL || cv == NULL) { free(ms); free(cv); free(e); return NULL; }
    map_mutex_init(ms);
    e->queue_lock = ms;
    e->queue_signal = cv;

    // 启动 worker 线程
    int n = e->options.max_workers;
    e->workers = (map_worker*)calloc((size_t)n, sizeof(map_worker));
    e->threads = (map_thread_t*)calloc((size_t)n, sizeof(map_thread_t));
    if (e->workers == NULL || e->threads == NULL) { map_engine_destroy(e); return NULL; }
    for (int i = 0; i < n; i++) {
        e->workers[i].worker_id = i;
        e->workers[i].active = false;
        worker_arg* wa = (worker_arg*)malloc(sizeof(worker_arg));
        if (wa == NULL) break;
        wa->e = e;
        wa->id = i;
        map_thread_t t = map_thread_start(worker_loop, wa);
        if (t == (map_thread_t)0) { free(wa); break; }   // 创建失败
        e->threads[e->thread_count] = t;
        e->thread_count++;
        e->worker_count++;
    }
    return e;
}

void map_engine_destroy(map_engine* e) {
    if (e == NULL) return;
    // 停 worker
    if (e->queue_lock != NULL) {
        map_mutex_lock((map_mutex_t*)e->queue_lock);
        e->stopping = 1;
        map_cond_wakeall((map_cond_t*)e->queue_signal);
        map_mutex_unlock((map_mutex_t*)e->queue_lock);
        for (int i = 0; i < e->thread_count; i++) {
            map_thread_t t = e->threads[i];
            map_thread_wait(&t);
            map_thread_close(&t);
        }
        // 清残留队列
        map_cmd* c = e->queue_head;
        while (c != NULL) { map_cmd* nx = c->next; free(c->text); free(c); c = nx; }
        map_mutex_destroy((map_mutex_t*)e->queue_lock);
        free(e->queue_lock);
        free(e->queue_signal);
    }
    free((void*)e->maps);
    free((void*)e->workers);
    free((void*)e->threads);
    free(e);
}

int map_engine_attach_map(map_engine* e, map* m) {
    if (e == NULL || m == NULL) return -1;
    for (int i = 0; i < e->map_count; i++)
        if (e->maps[i] == m) return 0;             // 已挂, 幂等
    size_t ncap = (size_t)e->map_count + 1;   // 先转 size_t 再运算(防 int 域溢出)
    map** nm = (map**)realloc((void*)e->maps, ncap * sizeof(map*));
    if (nm == NULL) return -1;
    e->maps = nm;
    e->maps[e->map_count++] = m;
    m->engine = e;                                  // 图 → 引擎反向指针
    return 0;
}

int map_engine_submit(map_engine* e, map* m, map_cmd* cmd) {
    if (e == NULL || m == NULL || cmd == NULL) return -1;
    if (e->options.max_workers <= 0) {
        // 单线程串行: 同步执行后完成回调
        int rc = map_exec(m, cmd->text, cmd->out, cmd->outsz);
        if (cmd->done != NULL) cmd->done(cmd->ud, rc);
        free(cmd->text);
        free(cmd);
        return rc;
    }
    // 线程池模式: 入队 + 唤醒一个 worker
    if (e->queue_lock == NULL) return -1;
    if (e->options.queue_depth > 0 && e->queue_len >= e->options.queue_depth)
        return -2;   // 队列满(调用方可重试或阻塞)
    map_mutex_lock((map_mutex_t*)e->queue_lock);
    if (e->stopping) { map_mutex_unlock((map_mutex_t*)e->queue_lock); return -1; }
    cmd->next = NULL;
    if (e->queue_tail != NULL) e->queue_tail->next = cmd;
    else e->queue_head = cmd;
    e->queue_tail = cmd;
    e->queue_len++;
    map_cond_wake((map_cond_t*)e->queue_signal);
    map_mutex_unlock((map_mutex_t*)e->queue_lock);
    return 0;   // 已入队(结果经 done 回调)
}

int map_engine_gc(map_engine* e) {
    (void)e;
    // TODO: 回收 trash(需内核删除拦截)
    return 0;
}
