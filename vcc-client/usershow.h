#ifndef USERSHOW_H
#define USERSHOW_H

#include <QWidget>
#include <QPaintEvent>
#include <QImage>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QTime>

// 前向声明（避免头文件循环依赖）
class YuvGLWidget;
struct AVFrame;

namespace Ui {
class UserShow;
}

class UserShow : public QWidget
{
    Q_OBJECT

public:
    explicit UserShow(QWidget *parent = nullptr);
    ~UserShow();

    //提供 m_id 的 public 访问器
    //原 m_id 为 private，RoomDialog::slot_addUserShow 需要读取 m_id 作为 map 的 key
    //添加 getter 方法，遵循封装原则，不暴露成员变量
    int getId() const { return m_id; }
signals:
    void SIG_itemClicked( int id , QString name );
public slots:
    void slot_setInfo( int id , QString name);
    void paintEvent(QPaintEvent *event);
    void slot_setImage( QImage& img);

    // YUV帧设置接口
    // 接收 H.264 解码后的 AVFrame(YUV420P)，转发到 YuvGLWidget 渲染
    // 与原 slot_setImage(QImage) 共存，按编码模式选择
    void slot_setFrame( AVFrame *frame );

    void mousePressEvent(QMouseEvent *event);

    // resizeEvent，让YuvGLWidget跟随父控件大小变化
    void resizeEvent(QResizeEvent *event);

    void slot_checkTimer();

private:
    Ui::UserShow *ui;

    int m_id;
    QString m_UserName;
    QImage m_img;

    QImage m_defaultImg;
    QTimer m_timer;
    QTime m_lastTime;

    // YuvGLWidget指针成员
    // 持有 YUV 渲染控件（在构造函数中创建，析构函数中释放）
    // 用于 H.264 解码后的 GPU 加速渲染
    YuvGLWidget *m_glWidget;

    friend class RoomDialog;
};

#endif // USERSHOW_H
