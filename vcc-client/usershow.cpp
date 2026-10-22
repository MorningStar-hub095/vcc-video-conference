#include "usershow.h"
#include "ui_usershow.h"
#include <QPainter>
#include <QCoreApplication>

// 引入FFmpeg AVFrame完整定义用于帧尺寸日志输出
// usershow.h仅有前向声明struct AVFrame，访问frame->width/height需完整定义
extern "C"
{
#include "libavutil/frame.h"
}

// 引入YuvGLWidget头文件
#include "yuvglwidget.h"

UserShow::UserShow(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::UserShow) , m_id(0)
{
    // 初始化GL控件指针
    m_glWidget = nullptr;

    ui->setupUi(this);
    connect( &m_timer , SIGNAL(timeout()) , this , SLOT(slot_checkTimer() ));

    m_lastTime = QTime::currentTime();

    //【修复-P3：默认头像相对路径】"./images/tuer.jpg" 依赖进程工作目录，
    // 从其他目录启动 exe 时加载失败。改为按可执行文件目录定位。
    // 原代码: m_defaultImg("./images/tuer.jpg")（相对工作目录）
    m_defaultImg = QImage( QCoreApplication::applicationDirPath() + "/images/tuer.jpg" );

    m_timer.start( 1000 );
}

UserShow::~UserShow()
{
    // 释放GL控件
    if (m_glWidget) {
        delete m_glWidget;
        m_glWidget = nullptr;
    }
    delete ui;
}

//设置
void UserShow::slot_setInfo(int id, QString name)
{
    m_id = id;
    m_UserName = name;

    QString text = QString("用户名:%1").arg(m_UserName);
    ui->lb_name->setText( text );
}


//重绘事件
void UserShow::paintEvent(QPaintEvent *event)
{
    QPainter painter( this );
    //画黑色背景
    painter.setBrush( QBrush( Qt::black ) );
    painter.drawRect( 0, 0, this->width(), this->height() );

    // 当 YuvGLWidget 可见时（H264远程视频模式），跳过 QImage 绘制
    if (m_glWidget && m_glWidget->isVisible()) {
        painter.end();
        return;
    }

    if( m_img.size().height()<=0 ) return;

    //画视频帧
    //等比例缩放
    //添加 ui->lb_name 空指针检查和目标尺寸有效性检查
    int labelH = (ui->lb_name) ? ui->lb_name->height() : 20;
    int targetW = this->width();
    int targetH = this->height() - labelH;
    if (targetW <= 0 || targetH <= 0) {
        painter.end();
        return;
    }
    m_img = m_img.scaled(targetW, targetH, Qt::KeepAspectRatio);

    int x = this->width() - m_img.width();
    x = x/2;
    int y = this->height() - m_img.height() - labelH;
    y = labelH + y/2;
    QPixmap pix = QPixmap::fromImage( m_img );
    painter.drawPixmap( x , y , pix );
    painter.end();

}

void UserShow::slot_setImage(QImage &img)
{
    // 使用深拷贝而非浅拷贝，防止 paintEvent 中 scaled() 访问已释放的共享像素数据
    // 原Bug: m_img = img 是浅拷贝（Qt隐式共享），m_img 与 img 共享同一块像素数据。
    //        slot_sendVideoFrame 返回后 img 被销毁，引用计数减少。
    //        paintEvent 中 m_img.scaled() 读取像素数据时，如果引用计数管理出现
    //        并发问题（video线程可能正在创建新的QImage影响共享数据），会导致读取
    //        已释放内存 → 崩溃。
    // 修复: m_img = img.copy() 创建深拷贝，m_img 拥有完全独立的像素数据缓冲区，
    //       不受其他 QImage 生命周期影响。
    m_img = img.copy();
    //每次获得画面 更新时间
    m_lastTime = QTime::currentTime();

    // 切换到 QImage 模式时隐藏 YuvGLWidget，防止 GL 渲染层覆盖 QPainter 绘制的画面
    // 原Bug: slot_setImage 设置 m_img 并触发 paintEvent 绘制 QImage，
    //        但 YuvGLWidget 作为子控件仍在顶层渲染上一帧 AVFrame，
    //        导致 paintEvent 绘制的 QImage（本地预览/默认图）被 GL 控件遮挡，画面不显示。
    // 修复: 当切换到 QImage 模式时隐藏 YuvGLWidget，让 QPainter 的绘制可见。
    //       当 slot_setFrame 被调用时会重新 show() YuvGLWidget。
    if (m_glWidget) {
        m_glWidget->hide();
    }

    this->update();
    //实现视频加速渲染 openGL

}

// slot_setFrame接收YUV帧
// 接收 H.264 解码后的 AVFrame(YUV420P)，转发到 YuvGLWidget 进行 GPU 加速渲染
// 与原 slot_setImage 共存，由 ckernel 按编码模式选择调用
void UserShow::slot_setFrame(AVFrame *frame)
{
    if (!frame) {
        qWarning() << "error:slot_setFrame收到空帧";
        return;
    }

    // 清理死代码，合并两个 m_glWidget 检查块为一个
    // 原Bug: 原代码有两段 if(!m_glWidget) 块：
    //        第一段(lines 95-99)将 m_glWidget 设为 nullptr（本身已是 nullptr，完全无效）
    //        第二段(lines 123-136)才真正创建 YuvGLWidget。
    //        中间还有一段 if(!frame) 死代码（第117行，永远不可达因为第92行已return）。
    //        这种结构极易导致维护者误修改，且第一段 if 块可能被误认为是创建逻辑。
    // 修复: 合并为单一的 m_glWidget 创建逻辑，移除死代码。
    // 原代码保留为注释:
    /*
    if (!m_glWidget) {
        m_glWidget = nullptr;  // 原第98行：无效赋值
    }
    if (!frame) {              // 原第117行：死代码
        qWarning() << "error:slot_setFrame收到空帧";
        return;
    }
    if (!m_glWidget) {
        m_glWidget = new YuvGLWidget(this);
        int labelHeight = ui->lb_name ? ui->lb_name->height() : 20;
        int vidHeight = this->height() - labelHeight;
        m_glWidget->setGeometry(0, labelHeight, this->width(), vidHeight);
        m_glWidget->show();
    }
    */

    // 更新心跳时间（用于超时检测）
    m_lastTime = QTime::currentTime();

    // 延迟创建YuvGLWidget（首次调用时）
    if (!m_glWidget) {
        qDebug() << "首次创建YuvGLWidget实例";
        m_glWidget = new YuvGLWidget(this);

        // 设置YuvGLWidget覆盖UserShow的视频显示区域
        // 预留底部标签栏高度（lb_name），视频显示在标签上方
        int labelHeight = ui->lb_name ? ui->lb_name->height() : 20;
        int vidHeight = this->height() - labelHeight;
        m_glWidget->setGeometry(0, labelHeight, this->width(), vidHeight);
        m_glWidget->show();
        qDebug() << "YuvGLWidget已创建并显示"
                 << "位置: 0," << labelHeight
                 << "尺寸:" << this->width() << "x" << vidHeight;
    } else {
        // 确保 YuvGLWidget 可见（可能被 slot_setImage 隐藏过）
        // 原Bug: slot_setImage 会 hide() YuvGLWidget（切换到 QImage 模式），
        //        当远程视频恢复时 slot_setFrame 被调用，但未 show() YuvGLWidget，
        //        导致 GL 控件保持隐藏，远程视频画面不显示。
        // 修复: 每次收到 AVFrame 时确保 YuvGLWidget 可见。
        if (!m_glWidget->isVisible()) {
            m_glWidget->show();
        }
    }

    // 转发AVFrame到YuvGLWidget进行GPU渲染
    // YuvGLWidget内部通过av_frame_clone持有引用，无需额外管理
    m_glWidget->slot_setFrame(frame);
}

void UserShow::mousePressEvent(QMouseEvent *event)
{
    event->accept();
    Q_EMIT SIG_itemClicked( m_id , m_UserName );

}

// 添加resizeEvent，让YuvGLWidget跟随父控件大小变化
// 原Bug: YuvGLWidget的几何尺寸仅在首次创建时通过setGeometry设置一次。
//        当UserShow控件被resize时（如窗口最大化、拖拽调整大小），
//        YuvGLWidget不会跟随调整，导致视频画面错位或被裁剪。
// 修复: 重写resizeEvent，在父控件大小变化时同步更新YuvGLWidget的几何尺寸。
void UserShow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    if (m_glWidget) {
        int labelHeight = ui->lb_name ? ui->lb_name->height() : 20;
        int vidHeight = this->height() - labelHeight;
        m_glWidget->setGeometry(0, labelHeight, this->width(), vidHeight);
    }
}

void UserShow::slot_checkTimer()
{
    //定时检测 接收界面是否超时
    if(m_lastTime.secsTo(QTime::currentTime()) > 5 )
    {
        //设为默认 画面
        slot_setImage( m_defaultImg );
    }

}
