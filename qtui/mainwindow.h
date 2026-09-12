#pragma once
/* Qt 主窗口:工具栏(7 按钮) + 中央视频区 + 右侧统计 Dock + 底部日志 Dock。
 * QTimer 桥:帧显示 5ms / 日志+统计 150ms / config 热加载+保存 500ms /
 * 相机空闲预览+采集 60ms / worker 回收 100ms。 */

#include <QMainWindow>

#include <atomic>
#include <string>

#include "frame_bus.h"

class QTimer;
class VideoWidget;
class StatsPanel;
class LogPanel;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent *e) override;

 private slots:
  /* 工具栏动作 */
  void onYoloModel();
  void onLabelFile();
  void onInputSource();
  void onStart();
  void onStop();
  void onCapture();
  void onSaveRecipe();
  void onLoadRecipe(const QString &name);
  void onQuit();

  /* 桥 */
  void pollFrame();
  void pollAux();      /* 日志+统计+worker 回收 */
  void pollConfig();   /* config 热加载+防抖保存 */
  void pollCamera();   /* 空闲预览+采集存图 */

 private:
  void buildToolbar();
  void buildDocks();
  void syncConfigToUi(bool force);
  void updateStatusText();
  void runJudgment(const FramePayload &p);
  bool confirmQuit();

  /* 工具栏动作(供禁用/恢复) */
  class QAction *act_start_ = nullptr;
  class QAction *act_stop_ = nullptr;
  class QAction *act_capture_ = nullptr;

  VideoWidget *video_ = nullptr;
  StatsPanel *stats_ = nullptr;
  LogPanel *logs_ = nullptr;

  QTimer *frame_timer_ = nullptr;
  QTimer *aux_timer_ = nullptr;
  QTimer *config_timer_ = nullptr;
  QTimer *camera_timer_ = nullptr;

  /* 输入源状态 */
  std::string input_path_;
  std::string yolo_path_;
  std::string label_path_;
  bool use_camera_ = false;

  /* 采集模式 */
  std::string capture_dir_;
  int capture_idx_ = 0;

  /* worker 自然结束检测 */
  int last_state_ = 0;
};
