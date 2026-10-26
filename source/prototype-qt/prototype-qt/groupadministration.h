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
    bool canAddUsers :1; //updates the group activity time
    bool canRemoveUsers:1; //updates the group activity time
    bool canScheduleGame:1; //updates the group activity time
    bool canJoinGame:1; //this can be used to ban people how have not payed, while keeping him in the group
};

/*
What I'd do instead: reverse it. Store the uint8_t and define the bits yourself. That's well-defined everywhere, the same size, and needs no union.
  The Qt way to do this is QFlags:

  enum class Right : uint8_t
  {
      AddUsers     = 1u << 0,   // updates the group activity time
      RemoveUsers  = 1u << 1,   // updates the group activity time
      ScheduleGame = 1u << 2,   // updates the group activity time
      JoinGame     = 1u << 3,   // can be used to ban people who have not paid, while keeping them in the group
      // bits 4-7 reserved, must be 0
  };
  Q_DECLARE_FLAGS(MemberRights, Right)
  Q_DECLARE_OPERATORS_FOR_FLAGS(MemberRights)

  Using it:

  member.rights = Right::AddUsers | Right::RemoveUsers | Right::JoinGame | Right::ScheduleGame;
  if (member.rights.testFlag(Right::JoinGame)) { ... }
  member.rights.setFlag(Right::JoinGame, false);   // ban

  ds << quint8(member.rights.toInt());             // for signing / the wire: same byte on every target

  The bit positions are written in the source, so the byte is the same on every compiler and CPU, and there's no padding or undefined behaviour. This
  also replaces the packRights() helper from earlier.
*/



struct GroupMember
{
    Member member;
    MemberRights rights;
};

//adding a member to the group:
//take the group datastream with the most recent version
//unpack the datastream into a group object
//edit it there
//pack it into a datasteam and sign it.

struct GroupPacked
{
    QByteArray groupPacked;
    Signature sigGroup; //add he userid that has signed the message, so we only need to check one public key instead of all
};


struct Group
{
    QList<GroupMember> members;
    qint64 expireSecsSinceEpoch; //change into qint64 seconds since epoch via  DateTime.toUTC().toSecsSinceEpoch())
    uint32_t  version;
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
    void packGroup(Group* group, GroupPacked* grouppacked);
    void unpackGroup(Group* group, GroupPacked* grouppacked);
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


