#ifndef GROUPADMINISTRATION_H
#define GROUPADMINISTRATION_H

#include <cstdint>

#include <QObject>
#include <QWidget>
#include <QtWidgets/QtWidgets>

#include <QPushButton>


#define GROUP_MAX_MEMBERS 5 //until i know howto efficiently use arrays with a non-fixed size.
#define MAX_MEMBERS 5



struct Signature
{
    uint32_t signature;
};

struct PublicKey
{
    uint32_t key;
};

struct PrivateKey
{
    uint32_t key;
};

struct Member
{
    uint32_t userId; //assiged by users who can add users //2^32 users is 4 billion, way more than a single group should ever have
    PublicKey publicKey;
};

struct MemberRights
{
    bool canAddUsers; //updates the group activity time
    bool canRemoveUsers; //updates the group activity time
    bool canScheduleGame; //updates the group activity time
    bool canJoinGame; //this can be used to ban people how have not payed, while keeping him in the group
};

struct GroupMember
{
    Member member;
    MemberRights rights;
};

struct Group
{
    QList<GroupMember> members;
    QDateTime expireDateTime;
    uint32_t  version;
    Signature sigGroup;
};

struct MemberNickNameAndId
{
    uint32_t userId; //assiged by users who can add users. //2^32 users is 4 billion, way more than a single group should ever have
    QString nickName; //only needed when drawing the UI, so, make a separate id to username? this avoids the dynamic size puzzle
};
struct PlayingMember
{
    // flags like: playing, have ball, have bibs, coming with friends, have payed,
    uint32_t userId;
    uint32_t  version;
    Signature sigUser;
};

struct MemberPrivate //this is the only private data of a member, and has to stay on the device of the user (or be encrypted.)
{
    MemberNickNameAndId nameAndId;
    PrivateKey privateKey; //to sign
    PublicKey publicKey; //to request access
};

struct Game
{
    //dateandtime
    //location
    //forget date and time
    PlayingMember playingMembers[GROUP_MAX_MEMBERS];
    uint32_t  version;
    Signature sigGame; //signature of everybody who is allowed to forward the game, so everybody in the group.
};

class GroupAdministration : public QWidget
{
    Q_OBJECT
public:
    GroupAdministration(QWidget *parent=nullptr);
public slots:
    void slotCreateGroup(bool clicked);
    void slotCreateUser(bool clicked);
    void slotJoinGroup(bool clicked);
signals:
private:
    QWidget *myWindow;
    QGridLayout *myGridLayout;
    QPushButton *myCreateGroupBtn;
    QPushButton *myCreateUserBtn;
    QPushButton *myJoinGroupBtn;

    MemberPrivate* myMembersPrivate[MAX_MEMBERS];
    void updatePlayingStatus(PrivateKey privatekey, PublicKey publickey, bool playing);
};

/*
class User : public QWidget
{
    Q_OBJECT
public:
    User(QWidget *parent=nullptr);
public slots:
    void slotCreateUser(bool clicked);
signals:
private:
    QWidget *myWindow;
    QGridLayout *myGridLayout;
    QPushButton *myCreateUserBtn;
};
*/
#endif // GROUPADMINISTRATION_H


