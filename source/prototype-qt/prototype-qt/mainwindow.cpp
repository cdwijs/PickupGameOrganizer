#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "groupadministration.h"

GroupAdministration *myGroupAdministration2;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{

    ui->setupUi(this);
    myGroupAdministration2 = new GroupAdministration(parent);
}

MainWindow::~MainWindow()
{
    delete ui;
}
