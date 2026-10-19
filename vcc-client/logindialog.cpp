#include "logindialog.h"
#include "ui_logindialog.h"
#include <QDebug>

LoginDialog::LoginDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::LoginDialog)
{
    ui->setupUi(this);
    this->setWindowTitle("注册&登录");
    ui->tw_page->setCurrentIndex( 0 );//第一时间显示注册
}

LoginDialog::~LoginDialog()
{
    delete ui;
}

void LoginDialog::closeEvent(QCloseEvent *event)
{
    event->ignore();
    Q_EMIT SIG_close();
}


#include <QRegExp>
#include <QMessageBox>
//提交登录信息
void LoginDialog::on_pb_commit_clicked()
{
    QString strTel      = ui->le_tel->text();
    QString strPassword = ui->le_password->text();
    //校验
    //非空校验 清掉空格后，不能是空字符串
    QString tmpTel = strTel, tmpPass = strPassword;
    if(tmpTel.remove(" ").isEmpty() || tmpPass.remove(" ").isEmpty())
    {
        QMessageBox::about(this, "提示", "手机号密码不能为空格");
        return;
    }
    //根据下拉框选择的登录方式进行不同的校验
    //原代码仅校验手机号格式：^1{9}$
    //修改后：用户通过 cb_login_type 下拉框选择登录方式
    //  - 索引0：手机号登录，校验 ^1{9}$
    //  - 索引1：用户名（昵称）登录，校验长度1-10
    //  - 索引2：邮箱登录，校验邮箱格式
    int loginType = ui->cb_login_type->currentIndex();
    if( loginType == 0 )
    {
        //手机号登录：校验手机号格式
        QRegExp reg("^1[3-9][0-9]{9}$");
        if(!reg.exactMatch(strTel))
        {
            QMessageBox::about(this, "提示", "手机号格式错误，应为11位手机号");
            return;
        }
    }
    else if( loginType == 1 )
    {
        //用户名（昵称）登录：校验长度
        if( strTel.length() > 10 || strTel.length() < 1 )
        {
            QMessageBox::about(this, "提示", "用户名长度需在1-10之间");
            return;
        }
    }
    else if( loginType == 2 )
    {
        //邮箱登录：校验邮箱格式
        QRegExp regEmail("^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}$");
        if(!regEmail.exactMatch(strTel))
        {
            QMessageBox::about(this, "提示", "邮箱格式错误");
            return;
        }
    }

    //2.校验密码 长度不能超过20
    if( strPassword.length() > 20)
    {
        QMessageBox::about(this, "提示", "密码过长，长度不超过20");
        return;
    }
    Q_EMIT SIG_loginCommit( strTel , strPassword );
    qDebug()<<__func__;
}

//清空注册信息
void LoginDialog::on_pb_clear_register_clicked()
{
    ui->le_confirm_register->setText("");
    ui->le_name_register->setText("");
    ui->le_password_register->setText("");
    ui->le_tel_register->setText("");
    ui->le_email_register->setText("");

}

//注册提交
void LoginDialog::on_pb_commit_register_clicked()
{

    QString strTel      = ui->le_tel_register->text();
    QString strPassword = ui->le_password_register->text();
    QString strConfirm  = ui->le_confirm_register->text();
    QString strName     = ui->le_name_register->text();
    QString strEmail    = ui->le_email_register->text();
    //校验
    //非空校验 清掉空格后，不能是空字符串
    QString tmpTel = strTel, tmpPass = strPassword, tmpName = strName, tmpEmail = strEmail;
    if(tmpTel.remove(" ").isEmpty() || tmpPass.remove(" ").isEmpty() || tmpName.remove(" ").isEmpty() || tmpEmail.remove(" ").isEmpty())
    {
        QMessageBox::about(this, "提示", "手机号密码昵称邮箱不能为空格");
        return;
    }
    //1.校验手机号 正则表达式
    QRegExp reg("^1[3-8][0-9]\{6,9\}$");
    bool res = reg.exactMatch(strTel);
    if(!res)
    {
        QMessageBox::about(this, "提示", "手机号格式错误，8-11位手机号");
        return;
    }

    //2.校验密码 长度不能超过20
    if( strPassword.length() > 20)
    {
        QMessageBox::about(this, "提示", "密码过长，长度不超过20");
        return;
    }
    //3.两次输入是否一致
    if( strPassword != strConfirm)
    {
        QMessageBox::about(this, "提示", "两次输入的密码不一致");
        return;
    }
    //4.name长度不超过10
    if(strName.length() > 10)
    {
        QMessageBox::about(this, "提示", "昵称长度不能超过10");
        return;
    }
    //5.校验邮箱格式
    QRegExp regEmail("^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}$");
    if(!regEmail.exactMatch(strEmail))
    {
        QMessageBox::about(this, "提示", "邮箱格式错误");
        return;
    }
    //注册信号新增 email 参数
    Q_EMIT SIG_registerCommit( strTel , strPassword , strName , strEmail );
    qDebug()<<__func__;

}

//清空登录信息
void LoginDialog::on_pb_clear_clicked()
{
    ui->le_password->setText("");
    ui->le_tel->setText("");

}

//登录方式下拉框切换槽函数
//功能：当下拉框选择不同登录方式时，动态修改 lb_tel 标签的提示文字
//  - 索引0：手机号 → lb_tel 显示"手机号"
//  - 索引1：用户名 → lb_tel 显示"用户名"
//  - 索引2：邮箱 → lb_tel 显示"邮箱"
void LoginDialog::on_cb_login_type_currentIndexChanged(int index)
{
    switch(index)
    {
    case 0:
        ui->lb_tel->setText("手机号");
        break;
    case 1:
        ui->lb_tel->setText("用户名");
        break;
    case 2:
        ui->lb_tel->setText("邮箱");
        break;
    default:
        break;
    }
}

