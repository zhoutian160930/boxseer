#pragma once
/* 视频显示控件:cv::Mat 显示(letterbox 适配)+ 两条可拖竖线(检测区边界)。
 * 竖线位置以比例存储(config line_*_frac),像素坐标实时换算;
 * 拖动时同步 pipeline::g_line_*_x(像素,基于当前视图宽)供 worker 实时读取。 */

#include <QWidget>
#include <atomic>

#include <opencv2/core.hpp>

class VideoWidget : public QWidget {
  Q_OBJECT
 public:
  explicit VideoWidget(QWidget *parent = nullptr);

  /* 显示一帧 BGR(内部做尺寸/格式转换,线程安全:仅 GUI 线程调用) */
  void setFrame(const cv::Mat &bgr);

  /* 设置/获取竖线比例 [0,1](持久化到 config 用) */
  void setLineFracs(double left, double right);
  double leftFrac() const { return left_frac_; }
  double rightFrac() const { return right_frac_; }

  /* 同步 pipeline atomic 竖线像素(视图尺寸变化后调用) */
  void syncLinesToPipeline();

 signals:
  /* 竖线被拖动(比例已 clamp),主窗口写 config + mark_dirty */
  void linesChanged(double left, double right);
  /* 拖动松手(一次拖拽结束),用于审计日志 */
  void dragFinished();

 protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void resizeEvent(QResizeEvent *) override;

 private:
  enum DragTarget { DRAG_NONE = 0, DRAG_LEFT, DRAG_RIGHT };

  /* 帧图像(始终持有 BGR→RGB888 副本) */
  cv::Mat disp_;          /* RGB888 */
  QPixmap cached_pix_;    /* 上次缩放结果(尺寸不变时复用) */
  QSize last_scale_;      /* cached_pix_ 对应的目标尺寸 */
  double left_frac_ = 0.25, right_frac_ = 0.75;
  DragTarget dragging_ = DRAG_NONE;

  /* letterbox 几何:图像映射到 widget 的绘制区 */
  QRect drawn_rect_;
  QRect imageDisplayRect(const QSize &img, const QRect &viewport) const;
};
