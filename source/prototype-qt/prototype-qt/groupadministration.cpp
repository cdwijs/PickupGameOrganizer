#include "groupadministration.h"
#include <QDebug>

//android only
/*
#include <QJniObject>
#include <QJniEnvironment>

bool hasStrongBoxFeature()
{
    // StrongBox is API 28+; the feature string doesn't exist before that.
    if (QNativeInterface::QAndroidApplication::sdkVersion() < 28)
        return false;

    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return false;

    const QJniObject pm = context.callObjectMethod(
        "getPackageManager", "()Landroid/content/pm/PackageManager;");
    if (!pm.isValid())
        return false;

    const QJniObject feature =
        QJniObject::fromString(QStringLiteral("android.hardware.strongbox_keystore"));
    const bool supported = pm.callMethod<jboolean>(
        "hasSystemFeature", "(Ljava/lang/String;)Z", feature.object<jstring>());

    QJniEnvironment().checkAndClearExceptions();
    return supported;
}
*/


GroupAdministration::GroupAdministration(QWidget *parent) : QWidget(parent)
{
    qDebug()<<Q_FUNC_INFO;
    myWindow = new QWidget;

    myGridLayout = new QGridLayout(myWindow);
    myCreateGroupBtn = new QPushButton();
    myCreateGroupBtn->setText("create group");

    myCreateUserBtn = new QPushButton();
    myCreateUserBtn->setText("create user");

    myGridLayout->addWidget(myCreateGroupBtn);
    myGridLayout->addWidget(myCreateUserBtn);
    QSize qsize;
    qsize.setHeight(600);
    qsize.setWidth(800);

    connect(myCreateGroupBtn,&QPushButton::clicked,this,&GroupAdministration::slotCreateGroup);
    connect(myCreateUserBtn,&QPushButton::clicked,this,&GroupAdministration::slotCreateUser);

    myWindow->setParent(parent);
    myWindow->setMinimumSize(800,600);
    myWindow->show();
    myWindow->setMinimumSize(0,0);
}

void GroupAdministration::slotCreateGroup(bool clicked)
{
    qDebug()<<Q_FUNC_INFO;
    Q_UNUSED(clicked);
    //hasStrongBoxFeature(); //android only
}

void GroupAdministration::slotCreateUser(bool clicked)
{
    qDebug()<<Q_FUNC_INFO;
    Q_UNUSED(clicked);
    //hasStrongBoxFeature(); //android only

    QString myString = "Hello";
    // This will print 15 (or another number) on some systems, not 5.
    // It's the capacity, not the length.
    qDebug() << "String capacity:" << myString.capacity();
    qDebug() << "String size:" << myString.size(); // This will print 5.

}

void GroupAdministration::updatePlayingStatus(PrivateKey privatekey, PublicKey publickey, bool playing) //also needs to have a pointer to struct Game
{
    //in the function above this one: Check if your public key is in keyCanJoinGame[]

    //find your nickname. If it is there, try if it's actually you by checking the signature. This uses the public key
    //if it is you, remove yourself if playing is false.
    //if you nick is't in the list, add yourself.
    //then sign your record. This uses the private key.
    //if structGame is altered, increase the version and sign it.
}