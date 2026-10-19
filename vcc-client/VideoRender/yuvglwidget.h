#ifndef YUVGLWIDGET_H
#define YUVGLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QImage>

// FFmpeg 前向声明
// 避免 FFmpeg 头文件污染其他编译单元
struct AVFrame;

// YUV420P OpenGL 渲染控件
// 设计要点:
//   1. 接收 AVFrame(YUV420P)，分别上传 Y/U/V 三张纹理
//   2. Fragment Shader 中按 BT.601 矩阵完成 YUV->RGB 转换
//   3. 首帧 glTexImage2D 分配纹理，后续 glTexSubImage2D 更新
//   4. 析构时 makeCurrent + glDeleteTextures 释放资源
//   5. 使用 VAO/VBO + Shader（现代 OpenGL，兼容 Core Profile）
class YuvGLWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit YuvGLWidget(QWidget *parent = nullptr);
    ~YuvGLWidget() override;

    // 设置 YUV 帧（线程安全：UI 线程调用）
    // frame: AVFrame(YUV420P)，内部 av_frame_clone 增加引用计数
    //        调用方仍可继续使用 frame，无需额外管理
    void slot_setFrame(AVFrame *frame);

    // 兼容接口：接收 QImage（用于回退到 JPEG 路径时）
    void slot_setImage(const QImage &img);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    // 初始化 Shader 程序
    bool initShader();

    // 初始化 VAO/VBO（四顶点 quad）
    void initQuad();

    // 初始化 Y/U/V 三张纹理
    void initTextures();

    // 上传 YUV 数据到纹理
    void uploadYuvTextures(AVFrame *frame);

    // 释放 OpenGL 资源
    void cleanupGL();

    // 计算 OpenGL 顶点坐标（保持宽高比）
    void computeVertexCoords(float &halfW, float &halfH);

    // 纹理对象
    GLuint m_texY;
    GLuint m_texU;
    GLuint m_texV;
    bool   m_texturesAllocated;  // 纹理是否已分配
    int    m_texWidth;           // 已分配纹理宽度
    int    m_texHeight;          // 已分配纹理高度

    // Shader 与缓冲
    QOpenGLShaderProgram   *m_shader;
    QOpenGLVertexArrayObject *m_vao;
    QOpenGLBuffer          *m_vbo;

    // 当前帧数据
    AVFrame *m_frame;  // 引用计数式持有，paintGL 后释放

    // 兼容 QImage 模式
    QImage   m_image;
    bool     m_useImageMode;  // true=QImage 模式， false=YUV 模式
};

#endif // YUVGLWIDGET_H
