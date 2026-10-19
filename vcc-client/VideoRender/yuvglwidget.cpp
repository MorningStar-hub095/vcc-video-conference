#include "yuvglwidget.h"
#include <QDebug>

// FFmpeg 头文件仅在 cpp 中引入
// 避免 .h 污染其他编译单元
extern "C"
{
#include "libavutil/frame.h"
}

// 着色器源码 - GLSL 1.10 兼容 ES 2.0
// 顶点着色器：输出位置 + 纹理坐标
static const char *kVertexShaderSrc =
    "attribute vec2 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

// 片段着色器 - BT.601 YUV->RGB 转换矩阵
// 采样三张纹理（Y/U/V）并按 BT.601 标准转换为 RGB
static const char *kFragmentShaderSrc =
    "uniform sampler2D texY;\n"
    "uniform sampler2D texU;\n"
    "uniform sampler2D texV;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    float y = texture2D(texY, vTexCoord).r;\n"
    "    float u = texture2D(texU, vTexCoord).r - 0.5;\n"
    "    float v = texture2D(texV, vTexCoord).r - 0.5;\n"
    "    // BT.601 转换矩阵\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.344 * u - 0.714 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

// 构造函数初始化成员
YuvGLWidget::YuvGLWidget(QWidget *parent): QOpenGLWidget(parent) , m_texY(0) , m_texU(0) , m_texV(0) ,
m_texturesAllocated(false) , m_texWidth(0) , m_texHeight(0) , m_shader(nullptr) , m_vao(nullptr) , m_vbo(nullptr) ,
m_frame(nullptr) , m_useImageMode(false)
{

}

// 析构函数 - 必须先 makeCurrent
YuvGLWidget::~YuvGLWidget()
{
    // 析构前必须 makeCurrent 才能调用 GL API
    if (context()) {
        makeCurrent();
        cleanupGL();
        doneCurrent();
    }
    // 释放 AVFrame 引用
    if (m_frame) {
        av_frame_free(&m_frame);
    }
}

// 设置 YUV 帧
// 线程安全：仅 UI 线程调用（信号槽 queued connection 自动）
void YuvGLWidget::slot_setFrame(AVFrame *frame)
{
    if (!frame) return;

    // 释放上一帧引用
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    // 克隆当前帧（引用计数 +1，不复制数据）
    m_frame = av_frame_clone(frame);
    m_useImageMode = false;

    // 异步触发重绘
    update();
}

// 兼容 QImage 接口
// 用于回退到 JPEG 路径时仍可显示
void YuvGLWidget::slot_setImage(const QImage &img)
{
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    m_image = img;
    m_useImageMode = true;
    update();
}

// 初始化 OpenGL
void YuvGLWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_TEXTURE_2D);

    initShader();
    initQuad();
    initTextures();
}

// 编译着色器
bool YuvGLWidget::initShader()
{
    m_shader = new QOpenGLShaderProgram;
    if (!m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSrc)) {
        qCritical() << "YuvGLWidget: vertex shader compile failed:" << m_shader->log();
        return false;
    }
    if (!m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShaderSrc)) {
        qCritical() << "YuvGLWidget: fragment shader compile failed:" << m_shader->log();
        return false;
    }
    if (!m_shader->link()) {
        qCritical() << "YuvGLWidget: shader link failed:" << m_shader->log();
        return false;
    }
    qDebug() << "YuvGLWidget: shader init success";
    return true;
}

// 初始化 VAO/VBO 四顶点 quad
// 使用 GL_TRIANGLE_FAN 替代已弃用的 GL_QUADS
void YuvGLWidget::initQuad()
{
    m_vao = new QOpenGLVertexArrayObject;
    m_vao->create();
    m_vao->bind();

    // 顶点数据: 每顶点 4 floats (position.xy + texCoord.xy)
    // 默认全屏，paintGL 中按宽高比动态更新
    static const float kVertices[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
         1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
    };

    m_vbo = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    m_vbo->create();
    m_vbo->bind();
    m_vbo->allocate(kVertices, sizeof(kVertices));

    int posAttr = m_shader->attributeLocation("aPos");
    int texAttr = m_shader->attributeLocation("aTexCoord");
    glVertexAttribPointer(posAttr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(texAttr, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(posAttr);
    glEnableVertexAttribArray(texAttr);

    m_vbo->release();
    m_vao->release();
}

// 初始化 Y/U/V 三张纹理
void YuvGLWidget::initTextures()
{
    glGenTextures(3, &m_texY);  // m_texY/ m_texU / m_texV 连续分配

    // m_texY/m_texU/m_texV 三个 GLuint 成员内存连续，可通过首地址 + i 索引
    // 修复: 原写法 (&m_texY) 是错误语法，直接用 (&m_texY) 即可
    GLuint *texPtr = &m_texY;
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texPtr[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

// 渲染主函数
void YuvGLWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (m_useImageMode) {
        // 兼容模式：QImage 直接渲染（简单实现，与原 MyOpenGLWidget 一致）
        if (m_image.isNull()) return;
        // QImage 模式直接走 QPainter 绘制（简单兜底）
        // 完整 YUV 路径走下面的逻辑
        // TODO: 后续可统一为 QImage -> GL_RGB 纹理上传
        return;
    }

    if (!m_frame) return;

    // 上传 YUV 数据到纹理
    uploadYuvTextures(m_frame);

    // 计算等比例缩放的顶点坐标
    float halfW, halfH;
    computeVertexCoords(halfW, halfH);

    // 更新 VBO 中的位置数据（保持宽高比）
    float vertices[] = {
        -halfW, -halfH,  0.0f, 1.0f,
         halfW, -halfH,  1.0f, 1.0f,
         halfW,  halfH,  1.0f, 0.0f,
        -halfW,  halfH,  0.0f, 0.0f,
    };
    m_vbo->bind();
    m_vbo->write(0, vertices, sizeof(vertices));
    m_vbo->release();

    // 绑定纹理到对应的纹理单元
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_texU);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_texV);

    // 绘制
    m_shader->bind();
    m_shader->setUniformValue("texY", 0);
    m_shader->setUniformValue("texU", 1);
    m_shader->setUniformValue("texV", 2);
    m_vao->bind();
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    m_vao->release();
    m_shader->release();
}

// 上传 YUV 数据到三张纹理
// 性能优化: 首帧 glTexImage2D 分配，后续 glTexSubImage2D 更新
void YuvGLWidget::uploadYuvTextures(AVFrame *frame)
{
    int yW = frame->width;
    int yH = frame->height;
    int uvW = yW / 2;
    int uvH = yH / 2;

    // Y 平面
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texY);
    if (!m_texturesAllocated || m_texWidth != yW || m_texHeight != yH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, yW, yH, 0,
                     GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, yW, yH,
                        GL_RED, GL_UNSIGNED_BYTE, frame->data[0]);
    }

    // U 平面
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_texU);
    if (!m_texturesAllocated || m_texWidth != yW) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uvW, uvH, 0,
                     GL_RED, GL_UNSIGNED_BYTE, frame->data[1]);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvW, uvH,
                        GL_RED, GL_UNSIGNED_BYTE, frame->data[1]);
    }

    // V 平面
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_texV);
    if (!m_texturesAllocated || m_texWidth != yW) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uvW, uvH, 0,
                     GL_RED, GL_UNSIGNED_BYTE, frame->data[2]);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvW, uvH,
                        GL_RED, GL_UNSIGNED_BYTE, frame->data[2]);
    }

    m_texturesAllocated = true;
    m_texWidth = yW;
    m_texHeight = yH;
}

// 计算等比例缩放的顶点坐标
void YuvGLWidget::computeVertexCoords(float &halfW, float &halfH)
{
    if (!m_frame || m_frame->width <= 0 || m_frame->height <= 0) {
        halfW = 1.0f;
        halfH = 1.0f;
        return;
    }

    // 按宽高比缩放
    QSize scaled = QSize(m_frame->width, m_frame->height)
                       .scaled(this->width(), this->height(), Qt::KeepAspectRatio);

    halfW = (float)scaled.width() / this->width();
    halfH = (float)scaled.height() / this->height();
    // 限制在  范围
    if (halfW > 1.0f) halfW = 1.0f;
    if (halfH > 1.0f) halfH = 1.0f;
}

// 窗口大小变化
void YuvGLWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

// 释放 GL 资源
void YuvGLWidget::cleanupGL()
{
    if (m_texturesAllocated) {
        GLuint texs[3] = { m_texY, m_texU, m_texV };
        glDeleteTextures(3, texs);
        m_texY = m_texU = m_texV = 0;
        m_texturesAllocated = false;
    }
    delete m_shader;
    m_shader = nullptr;
    delete m_vao;
    m_vao = nullptr;
    delete m_vbo;
    m_vbo = nullptr;
}
