#include <QtTest>
#include <QProcess>
#include <QBuffer>
#include <QImage>
#include <QElapsedTimer>
#include <cerrno>
#include "login/AeroLoginChild.h"
#include <unistd.h>
#include "aero_login_fixture.h"

class AeroLoginHelperTest final:public QObject {
    Q_OBJECT
    QPair<int,QByteArray> run(const QString &mode,const QByteArray &input={}){
        QProcess p;p.setProgram(QString::fromUtf8(CONTROL_AERO_LOGIN_HELPER_TEST_PATH));p.setArguments({mode});p.start();
        if(!p.waitForStarted(5000))return {-100,{}};p.write(input);p.closeWriteChannel();
        if(!p.waitForFinished(15000)){p.kill();p.waitForFinished();return {-101,{}};}
        return {p.exitStatus()==QProcess::NormalExit?p.exitCode():-102,p.readAllStandardOutput()+p.readAllStandardError()};
    }
private slots:
    void initTestCase(){if(geteuid()==0)QSKIP("Helper fixtures must run as an unprivileged user; no root helper execution is permitted.");}
    void administrativeModesRefuseBeforeReadingNativeFiles(){
        for(const auto *mode:{"--apply","--recover"}){const auto result=run(QString::fromLatin1(mode),"{}");QCOMPARE(result.first,1);QVERIFY(result.second.contains("Administrative authorization is required"));}
        QCOMPARE(run("--fixture=/tmp/alternate-root").first,2);QCOMPARE(run("--theme=other").first,2);
    }
    void decoderRunsUnprivilegedAndRequiresCanonicalPreparedBytes(){
        const auto image=aeroLoginImageFixture(Qt::green);QVERIFY(image.valid);auto result=run("--decode-prepared-image",image.bytes);QCOMPARE(result.first,0);QCOMPARE(result.second,QByteArray("ok\n"));
        QImage raw(12,9,QImage::Format_RGB32);raw.fill(Qt::blue);raw.setText("foreign metadata","must be stripped");QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);QVERIFY(raw.save(&buffer,"PNG"));
        result=run("--decode-image",bytes);QCOMPARE(result.first,0);result=run("--decode-prepared-image",bytes);QCOMPARE(result.first,4);
        result=run("--decode-image","not a picture");QCOMPARE(result.first,4);
    }
    void decoderEofWithoutExitCannotHoldTheHelperIndefinitely(){
        int pipe[2];QVERIFY(::pipe(pipe)==0);const auto child=fork();QVERIFY(child>=0);
        if(child==0){::close(pipe[0]);::write(pipe[1],"ok\n",3);::close(pipe[1]);::sleep(30);_exit(0);}
        ::close(pipe[1]);char bytes[4]{};QCOMPARE(::read(pipe[0],bytes,4),ssize_t(3));QCOMPARE(::read(pipe[0],bytes,4),ssize_t(0));::close(pipe[0]);
        QElapsedTimer timer;timer.start();int status=0;QVERIFY(!AeroLoginChild::finish(child,120,true,status));QVERIFY(timer.elapsed()<2000);QVERIFY(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
        errno=0;QCOMPARE(::waitpid(child,&status,WNOHANG),pid_t(-1));QCOMPARE(errno,ECHILD);
        const auto normal=fork();QVERIFY(normal>=0);if(normal==0)_exit(0);QVERIFY(AeroLoginChild::finish(normal,1000,true,status));
    }
};
QTEST_GUILESS_MAIN(AeroLoginHelperTest)
#include "aero_login_helper_test.moc"
