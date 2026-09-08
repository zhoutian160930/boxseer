#include "pipeline.h"

#include <unistd.h>

#include "affinity.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <vector>

#include <opencv2/opencv.hpp>

#include "camera_capture.h"
#include "can_bus.h"
#include "config.h"
#include "gpio_in.h"
#include "gpio_out.h"
#include "image_process.h"
#include "image_saver.h"
#include "judgment.h"
#include "mobilesam/mobilesam_pool.h"

namespace fs = std::filesystem;

namespace pipeline {

std::atomic<int> g_line_left_x{0};
std::atomic<int> g_line_right_x{0};
std::atomic<int> g_target_count{10};
std::atomic<int> g_material_class{0};
std::atomic<int> g_box_class{1};
std::atomic<int> g_correct_count{0};
std::atomic<int> g_wrong_count{0};
std::atomic<int> g_proc_count{0};
std::atomic<int> g_state{ST_IDLE};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_capture_mode{false};

FrameBus g_bus;
FramePayload g_last_payload;
bool g_has_last = false;

int g_view_width = 820;

/* ---- 输入源配置(Start 时快照) ---- */
static AppConfig g_cfg;
static std::string g_input_path;
static std::string g_yolo_path;
static std::string g_label_path;
static std::string g_sam_enc_path;
static std::string g_sam_dec_path;
static bool g_use_sam = false;
static int g_yolo_threads = 3;
static bool g_use_camera = false;

static std::unique_ptr<RknnPool> g_yolo_pool; /* 常驻 YOLO 模型 */
static std::thread g_worker;

void set_input_source(const std::string &path) { g_input_path = path; }
void set_use_camera(bool use) { g_use_camera = use; }
void set_yolo(const std::string &model_path, const std::string &label_path) {
  g_yolo_path = model_path;
  g_label_path = label_path;
}
void set_sam(const std::string &enc, const std::string &dec, bool use) {
  g_sam_enc_path = enc;
  g_sam_dec_path = dec;
  g_use_sam = use;
}
void set_yolo_threads(int n) { g_yolo_threads = n; }

bool release_yolo_pool() {
  if (g_state == ST_RUNNING) return false;
  if (g_worker.joinable()) return false;
  if (g_yolo_pool) {
    g_yolo_pool.reset();
    SPDLOG_INFO("YOLO model pool released (hot switch ready)");
  }
  return true;
}

bool running() { return g_worker.joinable(); }

/* ---- worker(逻辑与旧 ui_app.cpp::worker_fn 一致) ---- */
static void worker_fn() {
  affinity::pin_big();  /* 图像处理主循环钉到 A76 大核 */
  g_state = ST_RUNNING;
  g_proc_count = 0;
  g_correct_count = 0;
  g_wrong_count = 0;
  g_stop = false;
  AppConfig cfg = g_cfg;

  SPDLOG_DEBUG("worker start: raw input_path='{}' len={}", cfg.input_path,
               cfg.input_path.size());
  while (!cfg.input_path.empty()) {
    char c = cfg.input_path.back();
    if (c == '/' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      cfg.input_path.pop_back();
    } else {
      break;
    }
  }
  SPDLOG_DEBUG("worker: trimmed input_path='{}' len={}", cfg.input_path,
               cfg.input_path.size());

  SPDLOG_INFO("loading YOLO model: {} (threads={})", cfg.yolo_path,
              cfg.yolo_threads);
  if (!g_yolo_pool) {
    RknnPool *p = new RknnPool(cfg.yolo_path, cfg.yolo_threads, cfg.label_path);
    g_yolo_pool.reset(p);
    SPDLOG_INFO("YOLO model loaded (常驻)");
  }
  RknnPool &yolo = *g_yolo_pool;
  while (yolo.GetImageResultFromQueue().img != nullptr) {
  }
  std::unique_ptr<MobileSamPool> sam;
  if (cfg.use_sam && !cfg.sam_enc_path.empty() && !cfg.sam_dec_path.empty()) {
    SPDLOG_INFO("init SAM: {} / {}", cfg.sam_enc_path, cfg.sam_dec_path);
    sam = std::make_unique<MobileSamPool>(cfg.sam_enc_path, cfg.sam_dec_path,
                                          cfg.sam_threads);
    if (sam->Init() != 0) {
      SPDLOG_ERROR("SAM init failed, SAM disabled");
      sam.reset();
    }
  }

  bool is_dir = fs::is_directory(cfg.input_path);
  bool is_image = false;
  cv::Mat img0;
  if (!is_dir) {
    img0 = cv::imread(cfg.input_path);
    is_image = !img0.empty();
  }
  cv::VideoCapture cap;
  bool is_video = false;
  if (!is_dir && !is_image) {
    cap.open(cfg.input_path);
    is_video = cap.isOpened();
  }
  SPDLOG_DEBUG("input kind: is_dir={} is_image={} is_video={}", is_dir, is_image,
               is_video);
  if (!g_use_camera && !is_dir && !is_image && !is_video) {
    SPDLOG_ERROR("invalid input (not dir/image/video): {}", cfg.input_path);
    g_state = ST_STOPPED;
    g_bus.set_done();
    return;
  }

  /* 存图目录:每次程序启动共用一个(首次运行创建),重复点"开始"不再新建 */
  static const std::string save_dir = [] {
    std::time_t now_t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now_t));
    std::string d = config::g.detImg_root + "/" + ts;
    fs::create_directories(d);
    return d;
  }();
  /* 存图文件序号:跨多次"开始"单调递增,同一目录内不覆盖旧图 */
  static std::atomic<int> save_idx{0};
  SPDLOG_INFO("save_dir={} (本次程序运行共用)", save_dir);

  int submitted = 0;
  auto stopped = [&] { return g_stop.load(); };
  const int VW = g_view_width > 0 ? g_view_width : 1; /* 竖线像素→比例 */

  auto drain_one = [&] {
    YoloResult r = yolo.GetImageResultFromQueue();
    if (!r.img) return false;
    auto t_draw0 = std::chrono::high_resolution_clock::now();
    cv::Mat out = r.img->clone();
    ImageProcess ip(out.cols, out.rows, 640);
    ip.ImagePostProcess(out, r.results);

    /* clamp 到 [0,1]: 防御上游坐标异常,保证线始终画在图像内 */
    float llf = std::clamp(g_line_left_x.load() / (float)VW, 0.0f, 1.0f);
    float rrf = std::clamp(g_line_right_x.load() / (float)VW, 0.0f, 1.0f);
    int target = g_target_count.load();
    int bc = g_box_class.load(), mc = g_material_class.load();
    auto boxes = judge_all_boxes(r.results, out.cols, llf, rrf, target, bc, mc);
    JudgeSummary sum = summarize(boxes);
    bool qualified = is_qualified(sum);
    if (sum.total > 0) {
      if (qualified)
        g_correct_count.fetch_add(1);
      else
        g_wrong_count.fetch_add(1);
    }
    if (config::g.can_enabled) can_bus::send_result(qualified);
    gpio_out::set_qualified(qualified);
    SPDLOG_INFO("frame {} 合格={}", g_proc_count.load(), qualified);
    int ll = (int)(llf * out.cols), rr = (int)(rrf * out.cols);
    cv::line(out, cv::Point(ll, 0), cv::Point(ll, out.rows),
             cv::Scalar(255, 255, 0), 2);
    cv::line(out, cv::Point(rr, 0), cv::Point(rr, out.rows),
             cv::Scalar(255, 0, 255), 2);
    char top[128];
    snprintf(top, sizeof(top), "Boxes: %d | FULL %d  NOTFULL %d  NOTREVEALED %d",
             sum.total, sum.full, sum.not_full, sum.not_revealed);
    cv::Scalar top_col =
        (sum.not_full == 0 && sum.not_revealed == 0 && sum.total > 0)
            ? cv::Scalar(0, 255, 0)
            : cv::Scalar(0, 165, 255);
    cv::putText(out, top, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 0.9,
                top_col, 2);
    for (auto &b : boxes) {
      const char *tag;
      cv::Scalar col;
      char buf[64];
      if (!b.revealed) {
        tag = "NOT REVEALED";
        col = cv::Scalar(0, 165, 255);
        snprintf(buf, sizeof(buf), "%s", tag);
      } else if (b.full) {
        col = cv::Scalar(0, 255, 0);
        snprintf(buf, sizeof(buf), "FULL %d/%d", b.material_count, target);
      } else {
        col = cv::Scalar(0, 0, 255);
        snprintf(buf, sizeof(buf), "NOT FULL %d/%d", b.material_count, target);
      }
      int y = b.box_top - 8 > 10 ? b.box_top - 8 : b.box_bottom + 22;
      cv::putText(out, buf, cv::Point(b.box_left, y), cv::FONT_HERSHEY_SIMPLEX,
                  0.8, col, 2);
    }

    FramePayload payload;
    payload.frame = out;
    payload.results = r.results;
    payload.orig_w = out.cols;
    payload.orig_h = out.rows;
    int cur_idx = g_proc_count.load();
    SPDLOG_DEBUG("push frame {} to UI", cur_idx);
    auto t_draw1 = std::chrono::high_resolution_clock::now();
    g_bus.push(payload);
    for (int i = 0; i < 333 && g_bus.is_pending() && !stopped(); ++i)
      usleep(3000);
    auto t_ui1 = std::chrono::high_resolution_clock::now();
    double draw_ms =
        std::chrono::duration<double, std::milli>(t_draw1 - t_draw0).count();
    double ui_ms =
        std::chrono::duration<double, std::milli>(t_ui1 - t_draw1).count();
    SPDLOG_INFO(
        "frame {} timing: pre={:.1f} infer={:.1f draw={:.1f} uiwait={:.1f} ms",
        cur_idx, r.preprocess_us / 1000.0, r.inference_us / 1000.0, draw_ms,
        ui_ms);

    char savepath[512] = "";
    /* 用跨运行单调递增的序号命名,同一目录多次"开始"不覆盖旧图 */
    int sidx = save_idx.fetch_add(1);
    if (sum.total > 0) {
      snprintf(savepath, sizeof(savepath), "%s/%05d.jpg", save_dir.c_str(),
               sidx);
      image_saver::enqueue(out, savepath);
    }

    if (sum.total > 0) {
      SPDLOG_INFO(
          "frame {} | boxes={} full={} notfull={} notrevealed={} "
          "lines(frac)={:.2f}~{:.2f} target={} | save={}",
          cur_idx, sum.total, sum.full, sum.not_full, sum.not_revealed, llf,
          rrf, target, savepath);
    } else {
      SPDLOG_INFO("frame {} | no box(cls{}) detected | save={}", cur_idx, bc,
                  savepath);
    }

    if (sam) {
      std::vector<mobilesam_box> sboxes;
      for (int i = 0; i < r.results.count; i++) {
        auto &d = r.results.results[i];
        sboxes.push_back({d.box.left, d.box.top, d.box.right, d.box.bottom});
      }
      char nm[512];
      snprintf(nm, sizeof(nm), "%s/%05d_sam.jpg", save_dir.c_str(), sidx);
      sam->AddInferenceTask(*r.img, nm, sboxes);
    }
    g_proc_count++;
    return true;
  };

  auto process_one = [&](std::shared_ptr<cv::Mat> src) {
    while (gpio_in::is_system_paused() && !stopped()) {
      usleep(200000);
    }
    auto t0 = std::chrono::high_resolution_clock::now();
    ImageProcess ip(src->cols, src->rows, 640);
    yolo.AddInferenceTask(src, ip, "");
    submitted++;
    SPDLOG_DEBUG("submit frame {}, {}x{}", submitted, src->cols, src->rows);
    while (g_proc_count.load() < submitted && !stopped()) {
      drain_one();
      usleep(2000);
    }
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - t0)
                    .count();
    SPDLOG_INFO("frame {} total cost {:.1f} ms", submitted - 1, ms);
  };

  if (g_use_camera) {
    if (!camera_capture::init(0)) {
      SPDLOG_ERROR("camera: 初始化失败，退出 worker");
    } else {
      SPDLOG_INFO("camera: 开始实时推理");
      int cam_frames = 0;
      while (!stopped()) {
        cv::Mat frame;
        if (camera_capture::grab(frame)) {
          cam_frames++;
          SPDLOG_DEBUG("camera: grab 帧 {}", cam_frames);
          process_one(std::make_shared<cv::Mat>(frame.clone()));
        } else {
          usleep(5000);
        }
      }
      SPDLOG_INFO("camera: 循环退出, 共处理 {} 帧, stopped={}", cam_frames,
                  stopped());
    }
  } else if (is_image) {
    process_one(std::make_shared<cv::Mat>(img0.clone()));
  } else if (is_dir) {
    std::vector<fs::path> files;
    for (auto &p : fs::directory_iterator(cfg.input_path)) {
      if (!p.is_regular_file()) continue;
      auto ext = p.path().extension().string();
      for (auto &c : ext) c = (char)std::tolower(c);
      if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
        files.push_back(p.path());
    }
    std::sort(files.begin(), files.end());
    for (auto &p : files) {
      if (stopped()) break;
      cv::Mat im = cv::imread(p.string());
      if (im.empty()) continue;
      process_one(std::make_shared<cv::Mat>(im));
    }
  } else {
    while (!stopped()) {
      cv::Mat frame;
      cap >> frame;
      if (frame.empty()) break;
      process_one(std::make_shared<cv::Mat>(frame.clone()));
    }
  }

  g_bus.set_done();
  g_state = stopped() ? ST_STOPPED : ST_DONE;
}

bool start() {
  if (g_state == ST_RUNNING) return false;
  if (g_input_path.empty() && !g_use_camera) return false;
  g_cfg.input_path = g_input_path;
  g_cfg.yolo_path = g_yolo_path;
  g_cfg.label_path = g_label_path;
  g_cfg.sam_enc_path = g_sam_enc_path;
  g_cfg.sam_dec_path = g_sam_dec_path;
  g_cfg.use_sam = g_use_sam;
  g_cfg.yolo_threads = g_yolo_threads;
  g_cfg.out_dir = "outputs";
  fs::create_directories(g_cfg.out_dir);

  g_bus.reset();
  if (g_worker.joinable()) g_worker.join();
  g_state = ST_RUNNING;
  g_worker = std::thread(worker_fn);
  return true;
}

void stop_and_join() {
  g_stop = true;
  if (g_worker.joinable()) g_worker.join();
  if (g_state == ST_RUNNING) g_state = ST_STOPPED;
}

}  // namespace pipeline
