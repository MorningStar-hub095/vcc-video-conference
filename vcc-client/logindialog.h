#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>
#include "QCloseEvent"

namespace Ui {
class LoginDialog;
}

class LoginDialog : public QDialog
{
    Q_OBJECT
signals:
    void SIG_loginCommit(QString tel , QString password);
    //注册信号新增 email 参数
    //原信号：SIG_registerCommit(QString tel, QString password, QString name)
    //修改后：SIG_registerCommit(QString tel, QString password, QString name, QString email)
    void SIG_registerCommit(QString tel , QString password , QString name , QString email );
    void SIG_close();

public:
    explicit LoginDialog(QWidget *parent = nullptr);
    ~LoginDialog();
    void closeEvent(QCloseEvent * event);

private slots:


    void on_pb_commit_clicked();

    void on_pb_clear_register_clicked();

    void on_pb_commit_register_clicked();

    void on_pb_clear_clicked();

    //登录方式下拉框切换槽函数
    //当下拉框选择不同登录方式时，动态修改 lb_tel 标签的提示文字
    void on_cb_login_type_currentIndexChanged(int index);

private:
    Ui::LoginDialog *ui;
};

#endif // LOGINDIALOG_H
