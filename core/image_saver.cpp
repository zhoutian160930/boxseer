#include "image_saver.h"

#include <spdlog/spdlog.h>
#include <opencv2/imgcodecs.hpp>

#include "affinity.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace image_saver {

struct Task {
  cv::Mat img;
  std::string path;
};

static std::deque<Task> g_queue;
static std::mutex g_mtx;
static std::condition_variable g_cv;
static std::thread g_thread;
static std::atomic<bool> g_run{false};
static std::size_t g_max_queue = 16;
static std::atomic<unsigned long long> g_dropped{0};
static std::atomic<unsigned long long> g_saved{0};

static void worker_fn() {
  affinity::pin_small();  /* 磁盘存图 IO 钉到 A55 小核 */
  while (true) {
    Task t;
    {
      std::unique_lock<std::mutex> lk(g_mtx);
      g_cv.wait(lk, [] { return !g_queue.empty() || !g_run.load(); });
      if (g_queue.empty() && !g_run.load()) return;
      t = std::move(g_queue.front());
      g_queue.pop_front();
    }
    bool ok = cv::imwrite(t.path, t.img);
    if (ok) {
      g_saved.fetch_add(1, std::memory_order_relaxed);
    } else {
      SPDLOG_ERROR("image_saver: imwrite failed: {}", t.path);
    }
  }
}

void init(std::size_t max_queue) {
  if (g_run.exchange(true)) return;  /* 已启动 */
  g_max_queue = max_queue;
  g_thread = std::thread(worker_fn);
  SPDLOG_INFO("image_saver: started (max_queue={})", g_max_queue);
}

void enqueue(const cv::Mat &img, const std::string &path) {
  if (!g_run.load()) {
    /* 未启动则退化为同步写盘，避免丢图 */
    if (!cv::imwrite(path, img))
      SPDLOG_ERROR("image_saver: sync imwrite failed: {}", path);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_queue.size() >= g_max_queue) {
      g_queue.pop_front();  /* 丢最旧，保留最新 */
      g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_queue.push_back({img.clone(), path});
  }
  g_cv.notify_one();
}

void shutdown() {
  if (!g_run.exchange(false)) return;
  g_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
  SPDLOG_INFO("image_saver: shutdown (saved={}, dropped={}, leftover={})",
              g_saved.load(), g_dropped.load(), g_queue.size());
}

}  // namespace image_saver
