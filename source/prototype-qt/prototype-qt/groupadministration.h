#ifndef GROUPADMINISTRATION_H
#define GROUPADMINISTRATION_H

#include <cstdint>

#include <QObject>
#include <QWidget>
#include <QtWidgets/QtWidgets>

#include <QPushButton>


#define GROUP_MAX_USERS 5 //until i know howto efficiently use arrays with a non-fixed size.
#define MAX_USERS 5


struct Signature
{
    uint32_t signature;
};

struct PublicKey
{
    uint32_t publickey;
};

struct PrivateKey
{
    uint32_t privatekey;
};

struct Group
{
    PublicKey keyCanAddUsers[GROUP_MAX_USERS];
    PublicKey keyCanRemoveUsers[GROUP_MAX_USERS];
    PublicKey keyCanScheduleGame[GROUP_MAX_USERS];
    PublicKey keyCanJoinGame[GROUP_MAX_USERS];
    uint32_t  version;
    Signature sigGroup;
};

struct Game
{
    PublicKey keyIsPlaying[GROUP_MAX_USERS];
    uint32_t  version;
    Signature sigGame;
};

struct User
{
    PublicKey publicKey;
    PrivateKey privatekey;
};

class GroupAdministration : public QWidget
{
    Q_OBJECT
public:
    GroupAdministration(QWidget *parent=nullptr);
public slots:
    void slotCreateGroup(bool clicked);
    void slotCreateUser(bool clicked);
signals:
private:
    QWidget *myWindow;
    QGridLayout *myGridLayout;
    QPushButton *myCreateGroupBtn;
    QPushButton *myCreateUserBtn;
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


