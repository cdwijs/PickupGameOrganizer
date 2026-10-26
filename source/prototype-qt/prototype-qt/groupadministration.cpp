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

QList<Group> myGroups;


GroupAdministration::GroupAdministration(QWidget *parent) : QWidget(parent)
{
    qDebug()<<Q_FUNC_INFO;
    myWindow = new QWidget;

    myGridLayout = new QGridLayout(myWindow);
    myCreateGroupBtn = new QPushButton();
    myCreateGroupBtn->setText("create group");

    myCreateUserBtn = new QPushButton();
    myCreateUserBtn->setText("create user");

    myJoinGroupBtn = new QPushButton();
    myJoinGroupBtn->setText("join group");

    myGridLayout->addWidget(myCreateGroupBtn);
    myGridLayout->addWidget(myCreateUserBtn);
    myGridLayout->addWidget(myJoinGroupBtn);
    QSize qsize;
    qsize.setHeight(600);
    qsize.setWidth(800);

    connect(myCreateGroupBtn,&QPushButton::clicked,this,&GroupAdministration::slotCreateGroup);
    connect(myCreateUserBtn,&QPushButton::clicked,this,&GroupAdministration::slotCreateUser);
    connect(myJoinGroupBtn,&QPushButton::clicked,this,&GroupAdministration::slotJoinGroup);

    myMembersPrivate[0] = new MemberPrivate;
    myMembersPrivate[0]->privateKey.key = 0xcafe0124;
    myMembersPrivate[0]->publicKey.key = 0xbeef0124;
    myMembersPrivate[0]->nameAndId.nickName = "Mr Player";
    myMembersPrivate[0]->nameAndId.userId = 0;


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


    Group group;
    group.version = 0;
    group.members.clear();

    qsizetype membersize = group.members.size();
    uint32_t highestusedid = 0; //memberid=0 denotes not a user.
    for (qsizetype member = 0; member < membersize; ++member)
    {
        uint32_t id = group.members.at(member).member.userId;
        if (id > highestusedid)
        {
            highestusedid = id; //find the highest user ID, to ensure we don't re-use userID's of people who have left the group.
        }
    }



    GroupMember member;
    member.member.publicKey = myMembersPrivate[0]->publicKey;
    member.member.userId = highestusedid + 1; //memberid=0 denotes not a user.
    member.rights.canAddUsers = true; //when creating the group, the user creating the group gets all the rights.
    member.rights.canRemoveUsers = true;
    member.rights.canJoinGame = true;
    member.rights.canScheduleGame = true;
    group.members.append(member);
    QDateTime now = QDateTime::currentDateTime();
    group.expireSecsSinceEpoch = now.addYears(1).toUTC().toSecsSinceEpoch();
    group.version += 1;
    //pack it, then sign it.
//    group.sigGroup.signature = 1122334455; //replace by a call to a real signature maker, using the private key of userId 1

    myGroups.append(group); //this is not good, this implies all groups have no encryption or all the same encryption key.
}

void GroupAdministration::slotCreateUser(bool clicked) //should be called addMember()
{
    /*
    PublicKey publickey;
    publickey.key = myMembersPrivate[0]->publicKey.key; //this has to be offered by the user who wants to join the group

    qDebug()<<Q_FUNC_INFO;
    Q_UNUSED(clicked);

    uint32_t maxusedid = 0;
    uint32_t newuserid = 0;
    for (int i = 0; i < MAX_MEMBERS; ++i)
    {
        if (myMembers[i])
        {
            if (myMembers[i]->userId >= maxusedid)
            {
                maxusedid = myMembers[i]->userId;
            }
        }
    }
    newuserid = maxusedid+1;

    bool membercreatedok = false;
    for (int i = 0; i < MAX_MEMBERS; ++i)
    {
        if (myMembers[i] == nullptr)
        {
            myMembers[i] = new Member;
            myMembers[i]->userId = newuserid; //this userID has to be delivered back to the user. Will this be brittle?
            myMembers[i]->publicKey.key = publickey.key;
            membercreatedok = true;
            break;
        }
    }

    //new members can join games
    for (int i = 0; i < MAX_MEMBERS; ++i)
    {
        if (myGroup->memberCanJoinGame[i].publicKey.key == 0) //no key: good, we can fit a new one.
        {
            myGroup->memberCanJoinGame[i].publicKey.key = publickey.key;
            myGroup->memberCanJoinGame[i].userId = newuserid;
            break;
        }
    }
    //now sign the group again
    myGroup->version += 1;
    myGroup->sigGroup.signature = 1234567890;


    //hasStrongBoxFeature(); //android only

    QString myString = "Hello";
    // This will print 15 (or another number) on some systems, not 5.
    // It's the capacity, not the length.
    qDebug() << "String capacity:" << myString.capacity();
    qDebug() << "String size:" << myString.size(); // This will print 5.

    //return newuserid back to the user
*/
}

void GroupAdministration::slotJoinGroup(bool clicked) //should be called addMember()
{
    qDebug()<<Q_FUNC_INFO;
    Q_UNUSED(clicked);
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