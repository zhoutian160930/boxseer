#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "camera_capture.h"
#include "can_bus.h"
#include "config.h"
#include "image_saver.h"
#include "judgment.h"
#include "log_panel.h"
#include "pipeline.h"
#include "stats_panel.h"
#include "ui_log.h"
#include "video_widget.h"

namespace fs = std::filesystem;

namespace {
std::string base_name(const std::string &p) {
  size_t s = p.find_last_of('/');
  return (s == std::string::npos) ? p : p.substr(s + 1);
}
std::string trim_path(const std::string &p) {
  std::string r = p;
  while (!r.empty()) {
    char c = r.back();
    if (c == '/' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
      r.pop_back();
    else
      break;
  }
  return r;
}
/* 配置热加载审计:对比新旧 Config,逐字段输出变更(排查外部改参问题) */
void log_config_diff(const Config &o, const Config &n) {
#define CFG_DIFF(f)                                    \
  if (o.f != n.f)                                      \
    SPDLOG_INFO("[UI][配置热加载] {}: {} -> {}", #f, o.f, n.f);
  CFG_DIFF(material_class)
  CFG_DIFF(box_class)
  CFG_DIFF(target_count)
  CFG_DIFF(line_left_frac)
  CFG_DIFF(line_right_frac)
  CFG_DIFF(yolo_model)
  CFG_DIFF(label_path)
  CFG_DIFF(sam_encoder)
  CFG_DIFF(sam_decoder)
  CFG_DIFF(detImg_root)
  CFG_DIFF(saveImg_root)
  CFG_DIFF(log_root)
  CFG_DIFF(yolo_threads)
  CFG_DIFF(use_sam)
  CFG_DIFF(sam_threads)
  CFG_DIFF(default_input)
  CFG_DIFF(fb_model_dir)
  CFG_DIFF(fb_input_dir)
  CFG_DIFF(font_path)
  CFG_DIFF(ui_width)
  CFG_DIFF(ui_height)
  CFG_DIFF(panel_width)
  CFG_DIFF(default_res)
  CFG_DIFF(capture_interval_ms)
  CFG_DIFF(can_enabled)
  CFG_DIFF(can_send_if)
  CFG_DIFF(can_recv_if)
  CFG_DIFF(can_id)
  CFG_DIFF(gpio_enabled)
  CFG_DIFF(gpio_out_ch)
  CFG_DIFF(gpio_input_enabled)
  CFG_DIFF(gpio_input_ch)
  CFG_DIFF(gpio_poll_us)
  CFG_DIFF(gpio_input_latch)
  CFG_DIFF(camera_enabled)
  CFG_DIFF(camera_iface)
  CFG_DIFF(camera_ip)
  CFG_DIFF(camera_timeout_ms)
  CFG_DIFF(camera_width)
  CFG_DIFF(camera_height)
  CFG_DIFF(camera_exposure_us)
  CFG_DIFF(camera_gain)
#undef CFG_DIFF
}
}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("推块物料智能视觉检测系统"));
  /* 右上角系统按钮:最小化(缩到任务栏)/缩放(最大化↔窗口)/关闭(退出) */
  setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint |
                 Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
  resize(1280, 720);

  video_ = new VideoWidget(this);
  setCentralWidget(video_);

  buildToolbar();
  buildDocks();
  statusBar()->showMessage(QStringLiteral("就绪"));

  /* 输入初值来自 config */
  input_path_ = config::g.default_input;
  yolo_path_ = config::g.yolo_model;
  label_path_ = config::g.label_path;
  pipeline::set_yolo(yolo_path_, label_path_);
  pipeline::set_sam(config::g.sam_encoder, config::g.sam_decoder,
                    config::g.use_sam);
  pipeline::set_yolo_threads(config::g.yolo_threads);
  pipeline::g_target_count.store(config::g.target_count);
  pipeline::g_material_class.store(config::g.material_class);
  pipeline::g_box_class.store(config::g.box_class);
  video_->setLineFracs(config::g.line_left_frac, config::g.line_right_frac);
  syncConfigToUi(true);

  connect(video_, &VideoWidget::linesChanged, this,
          [this](double l, double r) {
            config::g.line_left_frac = (float)l;
            config::g.line_right_frac = (float)r;
            config::mark_dirty();
          });
  /* 拖线松手才记审计日志(拖动过程中 linesChanged 高频触发,不逐条记) */
  connect(video_, &VideoWidget::dragFinished, this, [this] {
    SPDLOG_INFO("[UI] 拖动检测线: left={:.4f} right={:.4f}",
                config::g.line_left_frac, config::g.line_right_frac);
  });
  connect(stats_, &StatsPanel::targetChanged, this, [](int v) {
            pipeline::g_target_count.store(v);
            config::g.target_count = v;
            config::mark_dirty();
            SPDLOG_INFO("[UI] 修改 目标物料数: {}", v);
          });

  /* 启动时自动加载摄像头(config 启用时) */
  if (config::g.camera_enabled) {
    use_camera_ = true;
    pipeline::set_use_camera(true);
    camera_capture::init(0);
    stats_->setInputFile(QStringLiteral("摄像头"));
  }

  image_saver::init(16);

  /* QTimer 桥 */
  frame_timer_ = new QTimer(this);
  connect(frame_timer_, &QTimer::timeout, this, &MainWindow::pollFrame);
  frame_timer_->start(5);

  aux_timer_ = new QTimer(this);
  connect(aux_timer_, &QTimer::timeout, this, &MainWindow::pollAux);
  aux_timer_->start(150);

  config_timer_ = new QTimer(this);
  connect(config_timer_, &QTimer::timeout, this, &MainWindow::pollConfig);
  config_timer_->start(500);

  camera_timer_ = new QTimer(this);
  connect(camera_timer_, &QTimer::timeout, this, &MainWindow::pollCamera);
  camera_timer_->start(60);

  /* Esc: 最大化/窗口化切换(与标题栏缩放按钮一致) */
  auto *esc = new QAction(this);
  esc->setShortcut(Qt::Key_Escape);
  addAction(esc);
  connect(esc, &QAction::triggered, this, [this] {
    isMaximized() ? showNormal() : showMaximized();
  });
}

MainWindow::~MainWindow() = default;

void MainWindow::buildToolbar() {
  auto *tb = addToolBar(QStringLiteral("主工具栏"));
  tb->setMovable(false);
  tb->setToolButtonStyle(Qt::ToolButtonTextOnly);

  auto *m_yolo = tb->addAction(QStringLiteral("YOLO模型"));
  connect(m_yolo, &QAction::triggered, this, &MainWindow::onYoloModel);
  auto *m_label = tb->addAction(QStringLiteral("标签文件"));
  connect(m_label, &QAction::triggered, this, &MainWindow::onLabelFile);
  auto *m_src = tb->addAction(QStringLiteral("输入源"));
  connect(m_src, &QAction::triggered, this, &MainWindow::onInputSource);

  act_start_ = tb->addAction(QStringLiteral("开始"));
  connect(act_start_, &QAction::triggered, this, &MainWindow::onStart);
  act_stop_ = tb->addAction(QStringLiteral("停止"));
  connect(act_stop_, &QAction::triggered, this, &MainWindow::onStop);
  act_stop_->setEnabled(false);

  act_capture_ = tb->addAction(QStringLiteral("采集图像"));
  act_capture_->setCheckable(true);
  connect(act_capture_, &QAction::triggered, this, &MainWindow::onCapture);

  auto *m_quit = tb->addAction(QStringLiteral("退出"));
  connect(m_quit, &QAction::triggered, this, &MainWindow::onQuit);
}

void MainWindow::buildDocks() {
  /* 右侧统计 Dock */
  auto *stats_dock = new QDockWidget(QStringLiteral("生产统计"), this);
  stats_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  stats_ = new StatsPanel(this);
  stats_dock->setWidget(stats_);
  addDockWidget(Qt::RightDockWidgetArea, stats_dock);
  stats_dock->setMinimumWidth(300);

  /* 底部日志 Dock(物料信息/运行日志/警告报警) */
  auto *log_dock = new QDockWidget(QStringLiteral("信息输出"), this);
  log_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  logs_ = new LogPanel(this);
  log_dock->setWidget(logs_);
  addDockWidget(Qt::BottomDockWidgetArea, log_dock);
  log_dock->setMinimumHeight(140);
}

/* ---- 工具栏动作 ---- */

void MainWindow::onYoloModel() {
  SPDLOG_INFO("[UI] 点击 YOLO模型");
  QString f = QFileDialog::getOpenFileName(
      this, QStringLiteral("选择 YOLO 模型"),
      QString::fromStdString(config::g.fb_model_dir),
      QStringLiteral("RKNN 模型 (*.rknn);;所有文件 (*)"));
  if (f.isEmpty()) {
    SPDLOG_INFO("[UI] YOLO模型选择取消");
    return;
  }
  yolo_path_ = trim_path(f.toStdString());
  config::g.yolo_model = yolo_path_;
  config::mark_dirty();
  pipeline::set_yolo(yolo_path_, label_path_);
  stats_->setYoloFile(QString::fromStdString(base_name(yolo_path_)));
  SPDLOG_INFO("YOLO 模型已切换: {}", yolo_path_);

  /* 修复:停止态释放旧 RknnPool,下次开始用新模型;运行中提示重启 */
  if (pipeline::g_state.load() == pipeline::ST_RUNNING) {
    QMessageBox::information(this, QStringLiteral("提示"),
                             QStringLiteral("模型将在本次运行结束后,下次\"开始\"时生效。"));
  } else if (pipeline::release_yolo_pool()) {
    SPDLOG_INFO("模型池已释放,下次开始加载新模型");
  }
}

void MainWindow::onLabelFile() {
  SPDLOG_INFO("[UI] 点击 标签文件");
  QString f = QFileDialog::getOpenFileName(
      this, QStringLiteral("选择标签文件"),
      QString::fromStdString(config::g.fb_model_dir),
      QStringLiteral("文本 (*.txt);;所有文件 (*)"));
  if (f.isEmpty()) {
    SPDLOG_INFO("[UI] 标签文件选择取消");
    return;
  }
  label_path_ = trim_path(f.toStdString());
  config::g.label_path = label_path_;
  config::mark_dirty();
  pipeline::set_yolo(yolo_path_, label_path_);
  stats_->setLabelFile(QString::fromStdString(base_name(label_path_)));
  SPDLOG_INFO("标签文件已切换: {}", label_path_);
}

void MainWindow::onInputSource() {
  SPDLOG_INFO("[UI] 点击 输入源");
  QMessageBox box(this);
  box.setWindowTitle(QStringLiteral("选择输入源"));
  box.setText(QStringLiteral("请选择输入源类型:"));
  QPushButton *folder =
      box.addButton(QStringLiteral("本地文件夹"), QMessageBox::ActionRole);
  QPushButton *camera =
      box.addButton(QStringLiteral("摄像头"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Cancel);
  box.exec();
  if ((void)box; box.clickedButton() == (QAbstractButton *)folder) {
    use_camera_ = false;
    pipeline::set_use_camera(false);
    camera_capture::shutdown(); /* 切回文件夹时关闭相机 */
    /* 目录/文件二选一菜单 */
    QMessageBox box2(this);
    box2.setWindowTitle(QStringLiteral("输入类型"));
    box2.setText(QStringLiteral("选择 文件 或 整个目录:"));
    QPushButton *b_file =
        box2.addButton(QStringLiteral("图片/视频文件"), QMessageBox::ActionRole);
    QPushButton *b_dir =
        box2.addButton(QStringLiteral("图片目录"), QMessageBox::ActionRole);
    box2.addButton(QMessageBox::Cancel);
    box2.exec();
    QString sel;
    if (box2.clickedButton() == (QAbstractButton *)b_file) {
      sel = QFileDialog::getOpenFileName(
          this, QStringLiteral("选择输入文件"),
          QString::fromStdString(config::g.fb_input_dir),
          QStringLiteral("媒体 (*.jpg *.jpeg *.png *.bmp *.mp4 *.avi *.mkv);;所有文件 (*)"));
    } else if (box2.clickedButton() == (QAbstractButton *)b_dir) {
      sel = QFileDialog::getExistingDirectory(
          this, QStringLiteral("选择图片目录"),
          QString::fromStdString(config::g.fb_input_dir));
    } else {
      return;
    }
    if (sel.isEmpty()) return;
    input_path_ = trim_path(sel.toStdString());
    config::g.default_input = input_path_;
    config::mark_dirty();
    pipeline::set_input_source(input_path_);
    stats_->setInputFile(QString::fromStdString(base_name(input_path_)));
    SPDLOG_INFO("输入源已选择: {}", input_path_);
  } else if (box.clickedButton() == (QAbstractButton *)camera) {
    use_camera_ = true;
    pipeline::set_use_camera(true);
    camera_capture::init(0); /* 幂等 */
    SPDLOG_INFO("输入源:摄像头");
    stats_->setInputFile(QStringLiteral("摄像头"));
  }
}

void MainWindow::onStart() {
  SPDLOG_INFO("[UI] 点击 开始 (输入={}, 模型={}, 标签={})", input_path_,
              base_name(yolo_path_), base_name(label_path_));
  if (pipeline::g_state.load() == pipeline::ST_RUNNING) return;
  if (!use_camera_ && input_path_.empty()) {
    stats_->setStatus(QStringLiteral("请先选择 输入/YOLO模型/标签"));
    return;
  }
  if (yolo_path_.empty() || label_path_.empty()) {
    stats_->setStatus(QStringLiteral("请先选择 输入/YOLO模型/标签"));
    return;
  }
  pipeline::set_input_source(input_path_);
  pipeline::set_yolo(yolo_path_, label_path_);
  pipeline::set_sam(config::g.sam_encoder, config::g.sam_decoder,
                    config::g.use_sam);
  pipeline::set_yolo_threads(config::g.yolo_threads);

  if (!pipeline::start()) {
    stats_->setStatus(QStringLiteral("启动失败(输入为空)"));
    return;
  }
  act_start_->setEnabled(false);
  act_stop_->setEnabled(true);
  updateStatusText();
}

void MainWindow::onStop() {
  SPDLOG_INFO("[UI] 点击 停止");
  if (pipeline::g_state.load() != pipeline::ST_RUNNING) return;
  pipeline::stop_and_join();
  act_start_->setEnabled(true);
  act_stop_->setEnabled(false);
  updateStatusText();
}

void MainWindow::onCapture() {
  SPDLOG_INFO("[UI] 点击 采集图像");
  if (pipeline::g_state.load() == pipeline::ST_RUNNING) {
    act_capture_->setChecked(false);
    return;
  }
  bool on = act_capture_->isChecked();
  pipeline::g_capture_mode.store(on);
  if (on) {
    if (!camera_capture::is_ready()) {
      use_camera_ = true;
      pipeline::set_use_camera(true);
      camera_capture::init(0);
    }
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&t));
    capture_dir_ = config::g.saveImg_root + "/" + ts;
    fs::create_directories(capture_dir_);
    capture_idx_ = 0;
    SPDLOG_INFO("capture: 开始采集, 保存目录={} 间隔={}ms", capture_dir_,
                config::g.capture_interval_ms);
  } else {
    SPDLOG_INFO("capture: 停止采集, 共 {} 张", capture_idx_);
  }
}

bool MainWindow::confirmQuit() {
  auto btn = QMessageBox::question(
      this, QStringLiteral("退出"),
      QStringLiteral("确定退出程序?"),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  return btn == QMessageBox::Yes;
}

void MainWindow::onQuit() {
  SPDLOG_INFO("[UI] 点击 退出");
  if (!confirmQuit()) {
    SPDLOG_INFO("[UI] 退出已取消");
    return;
  }
  close();
}

void MainWindow::closeEvent(QCloseEvent *e) {
  pipeline::stop_and_join();
  camera_capture::shutdown();
  can_bus::shutdown();
  image_saver::shutdown();
  QMainWindow::closeEvent(e);
  qApp->quit();
}

/* ---- QTimer 桥 ---- */

void MainWindow::pollFrame() {
  FramePayload payload;
  if (pipeline::g_bus.pop(payload)) {
    pipeline::g_last_payload = payload;
    pipeline::g_has_last = true;
    /* 最小化时跳过渲染(bus 必须继续 pop,防 worker 背压等待) */
    if (!isMinimized()) video_->setFrame(payload.frame);

    /* 物料信息一行 */
    float vw1 = pipeline::g_view_width > 0 ? pipeline::g_view_width : 1;
    float llf = std::clamp(pipeline::g_line_left_x.load() / vw1, 0.0f, 1.0f);
    float rrf = std::clamp(pipeline::g_line_right_x.load() / vw1, 0.0f, 1.0f);
    int bc = pipeline::g_box_class.load(), mc = pipeline::g_material_class.load();
    int tgt = pipeline::g_target_count.load();
    auto boxes =
        judge_all_boxes(payload.results, payload.orig_w, llf, rrf, tgt, bc, mc);
    static int info_idx = 0;
    info_idx++;
    QString line = QString(QStringLiteral("图%1: ")).arg(info_idx);
    int bi = 1;
    for (auto &b : boxes) {
      const char *st = !b.revealed ? "未漏出" : b.full ? "满" : "未满";
      line += QString(QStringLiteral("盒%1(%2/%3)%4 "))
                  .arg(bi++)
                  .arg(b.revealed ? b.material_count : 0)
                  .arg(tgt)
                  .arg(QString::fromUtf8(st));
    }
    if (boxes.empty()) line += QStringLiteral("(无盒子)");
    logs_->appendMaterialInfo(line);
  }
}

void MainWindow::runJudgment(const FramePayload &p) {
  if (p.orig_w <= 0 || p.frame.empty()) {
    stats_->setJudgeWaiting();
    return;
  }
  float vw = pipeline::g_view_width > 0 ? pipeline::g_view_width : 1;
  float llf = std::clamp(pipeline::g_line_left_x.load() / vw, 0.0f, 1.0f);
  float rrf = std::clamp(pipeline::g_line_right_x.load() / vw, 0.0f, 1.0f);
  int target = pipeline::g_target_count.load();
  auto boxes =
      judge_all_boxes(p.results, p.orig_w, llf, rrf, target,
                      pipeline::g_box_class.load(),
                      pipeline::g_material_class.load());
  JudgeSummary s = summarize(boxes);
  if (s.total == 0) {
    stats_->setJudge(-1, 0, 0, 0);
    return;
  }
  stats_->setJudge(s.total, s.full, s.not_full, s.not_revealed);
}

void MainWindow::pollAux() {
  /* 日志双缓冲 → 对应标签页 */
  QStringList info_lines, dbgerr_lines;
  for (auto &l : ui_log::take_info()) info_lines << QString::fromStdString(l);
  for (auto &l : ui_log::take_dbgerr())
    dbgerr_lines << QString::fromStdString(l);
  if (!info_lines.isEmpty()) logs_->appendLogInfo(info_lines);
  if (!dbgerr_lines.isEmpty()) logs_->appendLogDbgErr(dbgerr_lines);

  /* 计数 */
  stats_->setCounts(pipeline::g_correct_count.load(),
                    pipeline::g_wrong_count.load());

  /* 拖线实时重判 */
  if (pipeline::g_has_last) runJudgment(pipeline::g_last_payload);

  /* worker 自然结束:回收线程+恢复按钮 */
  int s = pipeline::g_state.load();
  if (last_state_ == pipeline::ST_RUNNING &&
      (s == pipeline::ST_DONE || s == pipeline::ST_STOPPED)) {
    pipeline::stop_and_join(); /* 幂等回收 */
    act_start_->setEnabled(true);
    act_stop_->setEnabled(false);
    updateStatusText();
  }
  last_state_ = s;
}

void MainWindow::pollConfig() {
  if (pipeline::g_state.load() != pipeline::ST_RUNNING) {
    Config snapshot = config::g;  /* 热加载前快照,加载后 diff 出审计日志 */
    if (config::poll_hot_reload()) {
      log_config_diff(snapshot, config::g);
      syncConfigToUi(false);
    }
  }
  /* 曝光/增益热加载:配置变化即下发相机,运行中同样生效 */
  static int last_exp = -1;
  static double last_gain = -2;
  int cur_exp = config::g.camera_exposure_us;
  double cur_gain = config::g.camera_gain;
  if (cur_exp != last_exp || cur_gain != last_gain) {
    camera_capture::apply_exposure();
    last_exp = cur_exp;
    last_gain = cur_gain;
  }
  if (config::poll_save_due()) config::save();
}

void MainWindow::pollCamera() {
  /* 相机模式空闲时:实时预览(无需点开始) + 采集存图 */
  if (!use_camera_ || pipeline::g_state.load() == pipeline::ST_RUNNING ||
      !camera_capture::is_ready())
    return;
  cv::Mat frame;
  if (camera_capture::grab(frame)) {
    pipeline::g_last_payload = FramePayload{frame, {}, frame.cols, frame.rows};
    pipeline::g_has_last = true;
    if (!isMinimized()) video_->setFrame(frame);  /* 最小化时跳过渲染 */
    if (pipeline::g_capture_mode.load() && !capture_dir_.empty()) {
      /* 按设定间隔存图(capture_interval_ms, 0=每帧都存);
       * 相机定时器 60ms 一拍,实际分辨率约 60ms */
      static auto last_save = std::chrono::steady_clock::now();
      int interval_ms = std::max(0, config::g.capture_interval_ms);
      auto now = std::chrono::steady_clock::now();
      if (interval_ms == 0 ||
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_save)
                  .count() >= interval_ms) {
        last_save = now;
        char sp[512];
        snprintf(sp, sizeof(sp), "%s/%06d.jpg", capture_dir_.c_str(),
                 capture_idx_++);
        cv::imwrite(sp, frame);
      }
    }
  }
}

void MainWindow::updateStatusText() {
  const char *st = "待机";
  switch (pipeline::g_state.load()) {
    case pipeline::ST_RUNNING: st = "运行中"; break;
    case pipeline::ST_DONE: st = "完成"; break;
    case pipeline::ST_STOPPED: st = "已停止"; break;
    default: st = "待机"; break;
  }
  stats_->setStatus(QString(QStringLiteral("状态: %1  | 已处理: %2"))
                        .arg(QString::fromUtf8(st))
                        .arg(pipeline::g_proc_count.load()));
}

void MainWindow::syncConfigToUi(bool force) {
  Q_UNUSED(force);
  input_path_ = config::g.default_input;
  if (yolo_path_ != config::g.yolo_model) {
    yolo_path_ = config::g.yolo_model;
    pipeline::set_yolo(yolo_path_, label_path_);
    pipeline::release_yolo_pool(); /* 外部改配置 → 释放模型池 */
  }
  if (label_path_ != config::g.label_path) {
    label_path_ = config::g.label_path;
    pipeline::set_yolo(yolo_path_, label_path_);
  }
  pipeline::set_sam(config::g.sam_encoder, config::g.sam_decoder,
                    config::g.use_sam);
  pipeline::set_yolo_threads(config::g.yolo_threads);
  pipeline::g_target_count.store(config::g.target_count);
  pipeline::g_material_class.store(config::g.material_class);
  pipeline::g_box_class.store(config::g.box_class);
  stats_->setTargetCount(config::g.target_count);
  video_->setLineFracs(config::g.line_left_frac, config::g.line_right_frac);
  stats_->setYoloFile(QString::fromStdString(base_name(yolo_path_)));
  stats_->setLabelFile(QString::fromStdString(base_name(label_path_)));
  stats_->setInputFile(use_camera_ ? QStringLiteral("摄像头")
                                   : QString::fromStdString(
                                         base_name(input_path_)));
  updateStatusText();
  if (pipeline::g_has_last) runJudgment(pipeline::g_last_payload);
}
