#include "ckernel.h"

#include <QApplication>



int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    //VccDialog w;
    //w.show();
    ckernel::GetInstance();
    return a.exec();
}
