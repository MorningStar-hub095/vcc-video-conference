#ifndef VCCDIALOG_H
#define VCCDIALOG_H

#include <QDialog>
#include <QCloseEvent>

QT_BEGIN_NAMESPACE
namespace Ui { class VccDialog; }
QT_END_NAMESPACE

class VccDialog : public QDialog
{
    Q_OBJECT
signals:
    void SIG_close();
    void SIG_createRoom();
    void SIG_joinRoom();
    //预定会议信号
    //点击"预定会议"按钮时触发，由 ckernel 接收处理
    void SIG_bookRoom();
    //设置信号
    //点击"设置"按钮时触发，由 ckernel 接收处理
    void SIG_settings();


public:
    VccDialog(QWidget *parent = nullptr);
    ~VccDialog();

    void closeEvent(QCloseEvent *event);

    void setInfo(QString  name, int icon = 1);

private slots:
    void on_pb_create_clicked();

    void on_pb_join_clicked();

    //预定会议按钮点击槽函数
    void on_pb_book_clicked();

    //设置按钮点击槽函数
    void on_pb_set_clicked();



private:
    Ui::VccDialog *ui;
};
#endif // VCCDIALOG_H
