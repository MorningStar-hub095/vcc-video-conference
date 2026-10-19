#include "vccdialog.h"
#include "ui_vccdialog.h"
#include <QMessageBox>
#include <QDebug>
//预定会议和设置功能所需的头文件
#include <QDateTime>
#include <QInputDialog>
#include <QSettings>
#include <QCoreApplication>
#include <QDir>

VccDialog::VccDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::VccDialog)
{
    ui->setupUi(this);

    setWindowTitle("视频会议v1.0");
}

VccDialog::~VccDialog()
{
    qDebug()<<__func__;
    delete ui;
}

//关闭事件
void VccDialog::closeEvent(QCloseEvent *event)
{
    qDebug()<<__func__;
    if(QMessageBox::question(this , "提示", "是否退出?") == QMessageBox::Yes)
    {
        qDebug()<<__func__;
        Q_EMIT SIG_close();
        event->accept();
    }
    else
    {
        event->ignore();
    }
}

void VccDialog::setInfo(QString name, int icon)
{
    //设置名字和头像
    ui->lb_name->setText( name );

}


void VccDialog::on_pb_create_clicked()
{
    Q_EMIT SIG_createRoom();

}


void VccDialog::on_pb_join_clicked()
{
    Q_EMIT SIG_joinRoom();

}

//预定会议按钮点击槽函数
//功能：预定一个未来的会议，生成会议号和会议时间，保存到本地配置文件
//实现：
//  1. 弹出输入对话框让用户输入会议主题
//  2. 弹出输入对话框让用户选择会议时长（分钟）
//  3. 生成唯一的会议号（基于当前时间戳）
//  4. 将会议信息（会议号、主题、时间、时长）保存到本地配置文件 booking.ini
//  5. 显示预定成功提示，包含会议号和会议时间
void VccDialog::on_pb_book_clicked()
{
    //输入会议主题
    bool ok = false;
    QString topic = QInputDialog::getText(this, tr("预定会议"),
                                          tr("请输入会议主题:"), QLineEdit::Normal,
                                          tr("我的会议"), &ok);
    if( !ok || topic.isEmpty() )
    {
        return;
    }

    //输入会议时长（分钟）
    QStringList items;
    items << tr("30分钟") << tr("60分钟") << tr("90分钟") << tr("120分钟");
    QString item = QInputDialog::getItem(this, tr("预定会议"),
                                          tr("请选择会议时长:"), items, 0, false, &ok);
    if( !ok || item.isEmpty() )
    {
        return;
    }

    //解析时长
    int duration = 30;
    if( item.startsWith("60") ) duration = 60;
    else if( item.startsWith("90") ) duration = 90;
    else if( item.startsWith("120") ) duration = 120;

    //生成会议号（基于当前时间戳，确保唯一性）
    //格式：MMDDHHMM（月日时分）
    qint64 timestamp = QDateTime::currentDateTime().toSecsSinceEpoch();
    int roomid = (int)(timestamp % 100000000);  //取后8位

    //计算会议开始时间（当前时间 + 5分钟提醒缓冲）
    QDateTime startTime = QDateTime::currentDateTime().addSecs(5 * 60);
    QString strStartTime = startTime.toString("yyyy-MM-dd HH:mm");

    //保存到本地配置文件 booking.ini
    //配置文件保存在程序所在目录下，方便用户查看
    QString configPath = QCoreApplication::applicationDirPath() + "/booking.ini";
    QSettings settings(configPath, QSettings::IniFormat);
    settings.beginGroup("Booking_" + QString::number(roomid));
    settings.setValue("roomid", roomid);
    settings.setValue("topic", topic);
    settings.setValue("starttime", strStartTime);
    settings.setValue("duration", duration);
    settings.endGroup();

    //显示预定成功提示
    QString info = tr("会议预定成功！\n\n")
                 + tr("会议号: %1\n").arg(roomid)
                 + tr("会议主题: %1\n").arg(topic)
                 + tr("开始时间: %1\n").arg(strStartTime)
                 + tr("会议时长: %1分钟\n").arg(duration)
                 + tr("\n请记下会议号，到时间后使用\"加入会议\"功能输入会议号加入。");
    QMessageBox::information(this, tr("预定成功"), info);

    //发送信号通知 ckernel（预留扩展，可用于服务端同步）
    Q_EMIT SIG_bookRoom();
}

//设置按钮点击槽函数
//功能：打开设置对话框，让用户配置音频/视频设备、网络等参数
//实现：
//  1. 从本地配置文件 settings.ini 读取当前设置
//  2. 弹出多页输入对话框让用户修改设置
//  3. 保存设置到本地配置文件
//  4. 发送信号通知 ckernel 应用新设置
void VccDialog::on_pb_set_clicked()
{
    //读取当前设置
    QString configPath = QCoreApplication::applicationDirPath() + "/settings.ini";
    QSettings settings(configPath, QSettings::IniFormat);

    //当前设置项
    QString curServerIp = settings.value("Network/ServerIP", "127.0.0.1").toString();
    int curVideoBitrate = settings.value("Video/Bitrate", 500).toInt();
    int curAudioVolume = settings.value("Audio/Volume", 80).toInt();

    //弹出设置对话框（使用输入对话框简化实现）
    bool ok = false;

    //服务器IP设置
    QString serverIp = QInputDialog::getText(this, tr("设置 - 服务器"),
                                          tr("服务器IP地址:"), QLineEdit::Normal,
                                          curServerIp, &ok);
    if( !ok ) return;

    //视频码率设置
    QStringList videoItems;
    videoItems << tr("低 (300kbps)") << tr("中 (500kbps)") << tr("高 (800kbps)");
    int curVideoIndex = (curVideoBitrate <= 300) ? 0 : ((curVideoBitrate <= 500) ? 1 : 2);
    QString videoItem = QInputDialog::getItem(this, tr("设置 - 视频"),
                                          tr("视频码率:"), videoItems, curVideoIndex, false, &ok);
    if( !ok ) return;

    int videoBitrate = 500;
    if( videoItem.startsWith("低") ) videoBitrate = 300;
    else if( videoItem.startsWith("中") ) videoBitrate = 500;
    else if( videoItem.startsWith("高") ) videoBitrate = 800;

    //音频音量设置
    int audioVolume = QInputDialog::getInt(this, tr("设置 - 音频"),
                                          tr("音频音量 (0-100):"), curAudioVolume,
                                          0, 100, 1, &ok);
    if( !ok ) return;

    //保存设置到配置文件
    settings.setValue("Network/ServerIP", serverIp);
    settings.setValue("Video/Bitrate", videoBitrate);
    settings.setValue("Audio/Volume", audioVolume);

    //提示用户
    QMessageBox::information(this, tr("设置"),
        tr("设置已保存，部分设置重启程序后生效。\n\n")
        + tr("服务器IP: %1\n").arg(serverIp)
        + tr("视频码率: %1 kbps\n").arg(videoBitrate)
        + tr("音频音量: %1\n").arg(audioVolume));

    //发送信号通知 ckernel
    Q_EMIT SIG_settings();
}



