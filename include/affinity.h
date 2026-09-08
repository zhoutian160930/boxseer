#pragma once
/* 线程 CPU 亲和性绑定(RK3588: cpu0-3=A55 小核, cpu4-7=A76 大核)。
 * 在目标线程入口处调用(绑定自身)。失败仅告警,不影响运行。 */
#ifdef __linux__

#include <pthread.h>
#include <sched.h>

#include <spdlog/spdlog.h>

namespace affinity {

static void pin_cpus(const cpu_set_t &set, const char *tag) {
  int r = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (r != 0) {
    SPDLOG_WARN("affinity: 绑核失败 {} (errno={})", tag, r);
  }
}

/* 钉到 A76 大核 cpu4-7(推理/图像处理) */
static inline void pin_big() {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(4, &set);
  CPU_SET(5, &set);
  CPU_SET(6, &set);
  CPU_SET(7, &set);
  pin_cpus(set, "big(4-7)");
}

/* 钉到 A55 小核 cpu0-3(IO 通信/存图) */
static inline void pin_small() {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  CPU_SET(1, &set);
  CPU_SET(2, &set);
  CPU_SET(3, &set);
  pin_cpus(set, "small(0-3)");
}

/* 钉到单个核(低延迟轮询类) */
static inline void pin_cpu(int n) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(n, &set);
  pin_cpus(set, "single");
}

}  // namespace affinity

#endif  // __linux__
