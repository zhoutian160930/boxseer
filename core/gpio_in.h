#pragma once
#include <atomic>

namespace gpio_in {

/* VR58H3 DI 通道(板子丝印脚号 1-4 → gpiof_in0~3) */
bool init(int ch);
void shutdown();

/* 快速读: 持久 fd + pread, 供高频轮询(每次仅一个系统调用)。
 * 返回 0/1, 失败 -1 */
int read_fast();

/* 兼容接口: 一次性打开读, 低频场景用 */
int read();

/* 外部暂停控制: DI=HIGH → 暂停 */
bool is_system_paused();
void set_paused(bool v);
}
