#include "roomdialog.h"
#include "ui_roomdialog.h"
#include <QMessageBox>
#include <QDebug>
//最大化功能所需的头文件
#include <QApplication>
#include <QScreen>
#include <QWindow>

RoomDialog::RoomDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::RoomDialog)
{
    ui->setupUi(this);

    m_mainLayout = new QVBoxLayout;

    m_mainLayout->setContentsMargins(0,0,0,0);
    m_mainLayout->setSpacing(5);

    //设置一个竖直布局的画布,可以向这里添加控件
    ui->wdg_list->setLayout( m_mainLayout );

    //初始化最大化状态标记为 false（未最大化）
    m_isMaximized = false;
    //保存窗口初始几何信息，用于还原
    m_normalGeometry = this->geometry();

//    //测试
//    for(int i=0 ; i<6 ; ++i)
//    {
//        UserShow* user = new UserShow;
//        user->slot_setInfo(i+1 , QString("测试%1").arg(i+1));
//        slot_addUserShow( user );
//    }
}

RoomDialog::~RoomDialog()
{
    delete ui;
}

void RoomDialog::slot_setInfo(QString roomid)
{
    QString title = QString("房间号:%1").arg(roomid);
    setWindowTitle(title);
    ui->lb_title->setText( title );
}

void RoomDialog::slot_addUserShow(UserShow *user)
{
    //原代码仅将 UserShow 添加到布局，未添加到 m_mapIDToUserShow。
    //这导致 slot_refreshUser(id, img) 查找 m_mapIDToUserShow 时找不到对应条目，视频帧被直接丢弃，无法显示。这是视频功能打不开的根本原因。
    //修复：将 UserShow 同时添加到布局和 map，以 user->getId() 为键。
    //注意：调用此函数前必须先调用 user->slot_setInfo(id, name) 设置 m_id，否则 m_id 为默认值(0)，可能导致多个用户映射到同一个 key。
    m_mainLayout->addWidget( user );
    m_mapIDToUserShow[ user->getId() ] = user;

    //如果大预览控件 ui->wdg_UserShow 的 id 还未设置（默认为0），
    //则将第一个添加的用户设为大预览，这样进入房间后默认显示第一个用户的画面
    if( ui->wdg_UserShow->getId() == 0 )
    {
        ui->wdg_UserShow->slot_setInfo( user->getId() , user->m_UserName );
    }
}

void RoomDialog::slot_refreshUser(int id, QImage &img )
{
    qDebug() << " slot_refreshUser called, id:" << id << ", map size:" << m_mapIDToUserShow.size() << ", count:" << m_mapIDToUserShow.count(id);
    //endregion
    //恢复并完善预览图刷新逻辑
    //ui->wdg_UserShow 是 UI 设计器放置的本地大预览控件（左侧大区域），不在 m_mapIDToUserShow 中。当用户点击右侧列表中的某个 UserShow 时，slot_setBigImgId 会被调用，将选中用户的 id 设置到 ui->wdg_UserShow。
    //因此当视频帧的 id 与 ui->wdg_UserShow 的 id 匹配时，需要同步刷新大预览。
    //原代码被注释掉后，大预览区域永远不显示画面。
    if( ui->wdg_UserShow->getId() == id)
    {
        ui->wdg_UserShow->slot_setImage( img );
    }
    //同时刷新右侧列表中对应的小预览
    if( m_mapIDToUserShow.count( id ) > 0)
    {
        UserShow* user = m_mapIDToUserShow[id];
        user->slot_setImage( img );
    }
}

// 实现slot_refreshUserYuv转发AVFrame到UserShow
// H264解码路径的UI刷新实现
// 与slot_refreshUser(QImage)逻辑对称，区别在于转发AVFrame到YuvGLWidget进行GPU渲染
void RoomDialog::slot_refreshUserYuv(int id, AVFrame *frame)
{
    if (!frame) {
        qWarning() << "error:slot_refreshUserYuv收到空帧, id:" << id;
        return;
    }

    qDebug() << " slot_refreshUserYuv called, id:" << id
             << ", map size:" << m_mapIDToUserShow.size()
             << ", count:" << m_mapIDToUserShow.count(id);

    // 刷新大预览控件（ui->wdg_UserShow，UI设计器放置的本地大预览）
    // 当视频帧的id与ui->wdg_UserShow的id匹配时，转发AVFrame
    if (ui->wdg_UserShow && ui->wdg_UserShow->getId() == id) {
        ui->wdg_UserShow->slot_setFrame(frame);
        qDebug() << " 大预览已刷新AVFrame, id:" << id;
    }

    // 刷新右侧列表中对应的小预览
    if (m_mapIDToUserShow.count(id) > 0) {
        UserShow *user = m_mapIDToUserShow[id];
        if (user) {
            user->slot_setFrame(frame);
            qDebug() << " 小预览已刷新AVFrame, id:" << id;
        }
    }
}

void RoomDialog::slot_removeUserShow(UserShow *user)
{
    user->hide();
    m_mainLayout->removeWidget(user);
}

void RoomDialog::slot_removeUserShow(int id)
{
    //原代码仅从布局移除控件，未从 map 删除条目，也未 delete 对象。这会导致 map 残留悬空指针和内存泄漏。
    //修复：从 map 删除条目并 delete 对象
    auto ite = m_mapIDToUserShow.find( id );
    if( ite != m_mapIDToUserShow.end() )
    {
        UserShow * user = ite->second;
        m_mapIDToUserShow.erase( ite );
        slot_removeUserShow( user );
        delete user;
    }
}

void RoomDialog::slot_setAudioCheck(bool check)
{
    ui->cb_audio->setChecked( check );
}

// 更新麦克风静音状态 UI 反馈
// 由 ckernel::slot_muteStateChanged 调用，根据 VAD 检测结果切换 cb_audio 显示
// 注意：仅修改文本/样式/tooltip，不改 checked 状态，避免干扰 on_cb_audio_clicked 的开关逻辑
// isMute=true : 检测到静音，文本改为"音频(无声)"，置灰，设 tooltip 提示
// isMute=false: 检测到语音，恢复文本"音频"，清除样式与 tooltip
void RoomDialog::updateMuteStatus(bool isMute)
{
    if (isMute) {
        // 静音：文本加"(无声)"后缀，灰色样式，tooltip 说明原因
        ui->cb_audio->setText(QStringLiteral("音频(无声)"));
        ui->cb_audio->setStyleSheet("color: gray;");
        ui->cb_audio->setToolTip(QStringLiteral("未检测到语音活动(VAD 静音)"));
    } else {
        // 有声：恢复默认显示
        ui->cb_audio->setText(QStringLiteral("音频"));
        ui->cb_audio->setStyleSheet("");
        ui->cb_audio->setToolTip("");
    }
}

void RoomDialog::slot_setVideoCheck(bool check)
{
    ui->cb_video->setChecked( check );
}

void RoomDialog::slot_setScreenCheck(bool check)
{
    ui->cb_desk->setChecked( check );
}

//检查指定用户是否已存在于成员列表中
bool RoomDialog::slot_hasUser(int id)
{
    return m_mapIDToUserShow.count( id ) > 0;
}


void RoomDialog::slot_setBigImgId(int id , QString name)
{
    ui->wdg_UserShow->slot_setInfo( id , name);
}

void RoomDialog::slot_clearUserShow()
{
    //原代码遍历 map 调用 slot_removeUserShow(UserShow*)，但该函数仅从布局移除控件并 hide，未 delete 对象，也未清空 map。这会导致退出房间后 map 残留悬空指针，再次进入房间时 key 冲突。
    //修复：遍历时 delete 对象，最后清空 map
    for( auto ite = m_mapIDToUserShow.begin(); ite != m_mapIDToUserShow.end();++ite)
    {
        slot_removeUserShow( ite->second );
        delete ite->second;
    }
    m_mapIDToUserShow.clear();

    //重置大预览控件的 id 为 0，这样下次进入房间时 slot_addUserShow 能正确设置默认大预览
    ui->wdg_UserShow->slot_setInfo( 0 , QString("") );
}

//退出房间
void RoomDialog::on_pb_close_clicked()
{
    this->close();
}


void RoomDialog::on_pb_quit_clicked()
{
    this->close();
}

//最小化按钮点击槽函数
//功能：将房间窗口最小化到任务栏
//实现：调用 showMinimized() 让窗口最小化
void RoomDialog::on_pb_min_clicked()
{
    this->showMinimized();
}

//最大化/还原按钮点击槽函数
//功能：在"窗口最大化"和"还原原始大小"两种状态间切换
//实现：
//  - 若当前未最大化(m_isMaximized == false)：
//      1. 保存当前窗口几何信息到 m_normalGeometry（用于后续还原）
//      2. 获取当前屏幕的可用几何区域（排除任务栏）
//      3. 将窗口设置为该屏幕的可用区域大小，实现最大化效果
//      4. 标记 m_isMaximized = true
//  - 若当前已最大化(m_isMaximized == true)：
//      1. 从 m_normalGeometry 恢复窗口原始位置和大小
//      2. 标记 m_isMaximized = false
void RoomDialog::on_pb_max_clicked()
{
    if( !m_isMaximized )
    {
        //保存当前窗口几何信息，用于还原
        m_normalGeometry = this->geometry();
        //获取窗口所在屏幕的可用几何区域（排除任务栏）
        QScreen * screen = QApplication::primaryScreen();
        if( screen )
        {
            QRect deskRect = screen->availableGeometry();
            this->setGeometry( deskRect );
        }
        m_isMaximized = true;
    }else{
        //还原窗口到原始位置和大小
        this->setGeometry( m_normalGeometry );
        m_isMaximized = false;
    }

//        if( !this->isMaximized() ) {
//            this->showMaximized();
//        } else {
//            this->showNormal();
//        }

}

//关闭事件
void RoomDialog::closeEvent(QCloseEvent *event)
{
    if(QMessageBox::question(this ,  "退出提示" , "是否退出会议？") == QMessageBox::Yes)
    {
        //发送退出房间信号
        Q_EMIT SIG_close();

        event->accept();
        return;
    }
    event->ignore();

}

//开启和关闭音频
void RoomDialog::on_cb_audio_clicked()
{
    if( ui->cb_audio->isChecked())
    {
        //勾选 = 用户想开启音频
        Q_EMIT SIG_AudioStart();
    }else{
        //取消勾选 = 用户想关闭音频
        Q_EMIT SIG_AudioPause();
    }
}

//开启和关闭视频
void RoomDialog::on_cb_video_clicked()
{
    if( ui->cb_video->isChecked())
    {
        //勾选 = 用户想开启视频
        //视频与桌面共享互斥，开启视频时自动关闭桌面共享
        ui->cb_desk->setChecked( false );
        Q_EMIT SIG_ScreenPause();
        Q_EMIT SIG_VideoStart();
    }else{
        //取消勾选 = 用户想关闭视频
        Q_EMIT SIG_VideoPause();
    }
}


void RoomDialog::on_cb_desk_clicked()
{
    if( ui->cb_desk->isChecked())
    {
        Q_EMIT SIG_ScreenStart();
    }else{
        Q_EMIT SIG_ScreenPause();
    }
}

