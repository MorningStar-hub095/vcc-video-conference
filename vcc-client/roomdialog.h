#ifndef ROOMDIALOG_H
#define ROOMDIALOG_H

#include <QDialog>

#include <QVBoxLayout>
#include "usershow.h"

//extern "C"
//{
//#include "libavcodec/avcodec.h"
//#include "libavformat/avformat.h"
//#include "libswscale/swscale.h"
//#include "libavdevice/avdevice.h"
//#include "libavutil/time.h"
//}


namespace Ui {
class RoomDialog;
}

class RoomDialog : public QDialog
{
    Q_OBJECT
signals:
    void SIG_close();
    void SIG_AudioPause();
    void SIG_AudioStart();
    void SIG_VideoStart();
    void SIG_VideoPause();
    void SIG_ScreenStart();
    void SIG_ScreenPause();

public:
    explicit RoomDialog(QWidget *parent = nullptr);
    ~RoomDialog();
public slots:
    void slot_setInfo(QString roomid);

    void slot_addUserShow( UserShow * user);

    void slot_refreshUser( int id , QImage & img );

    // slot_refreshUserYuv接口接收H264解码后的AVFrame
    // 阶段二+三：H264解码路径的UI刷新接口
    // 与原slot_refreshUser(QImage)共存，由ckernel按编码模式选择调用
    // 接收AVFrame(YUV420P)并转发到对应UserShow的YuvGLWidget进行GPU渲染
    void slot_refreshUserYuv( int id , AVFrame * frame );

    void slot_removeUserShow( UserShow * user);

    void slot_removeUserShow( int id);

    void slot_setAudioCheck( bool check );

    // 更新麦克风静音状态 UI 反馈（由 ckernel::slot_muteStateChanged 调用）
    // isMute=true: VAD 检测到静音，cb_audio 显示"音频(无声)"并置灰
    // isMute=false: 检测到语音活动，cb_audio 恢复"音频"
    void updateMuteStatus(bool isMute);

    void slot_clearUserShow();

    void slot_setVideoCheck( bool check );

    void slot_setBigImgId(int id , QString name);

    void slot_setScreenCheck(bool check);
    //检查指定用户是否已存在于成员列表中
    //用于防止服务端重复推送同一成员时创建多个 UserShow 控件
    bool slot_hasUser( int id );
private slots:
    void on_pb_close_clicked();

    void on_pb_quit_clicked();

    void closeEvent(QCloseEvent* event);

    void on_cb_audio_clicked();

    void on_cb_video_clicked();

    void on_cb_desk_clicked();

    //最大化/还原按钮的槽函数
    //点击 pb_max 按钮时触发，在"窗口最大化"和"还原原始大小"两种状态间切换
    void on_pb_max_clicked();

    //最小化按钮的槽函数
    //点击 pb_min 按钮时触发，将窗口最小化到任务栏
    void on_pb_min_clicked();

private:
    Ui::RoomDialog *ui;
    QVBoxLayout * m_mainLayout;
    std::map<int , UserShow*>m_mapIDToUserShow;

    //保存窗口还原时的几何信息
    //用于在最大化与还原之间切换时恢复窗口原始位置和大小
    QRect m_normalGeometry;
    //标记当前是否处于最大化状态
    bool m_isMaximized;
};

#endif // ROOMDIALOG_H


