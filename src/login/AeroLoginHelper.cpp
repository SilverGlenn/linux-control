#include "AeroLoginFiles.h"
#include "AeroLoginProtocol.h"
#include "AeroLoginChild.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QImageReader>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <climits>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
bool trusted(const QString &path)
{
    if(!QFileInfo(path).isAbsolute())return false;QString current=path;
    for(;;){struct stat s{};if(lstat(QFile::encodeName(current).constData(),&s)||S_ISLNK(s.st_mode)||s.st_uid!=0||(s.st_mode&0022))return false;if(current==QLatin1String("/"))return true;current=QFileInfo(current).absolutePath();}
}
bool transfer(int fd,QByteArray &data,qsizetype limit,bool writing,int timeout)
{
    QElapsedTimer elapsed;elapsed.start();qsizetype offset=0;
    while(elapsed.elapsed()<timeout){
        pollfd item{fd,short(writing?POLLOUT:POLLIN),0};const int ready=poll(&item,1,qMax(1,timeout-int(elapsed.elapsed())));if(ready<0&&errno==EINTR)continue;if(ready<=0)return false;
        if(writing){if(offset==data.size())return true;const auto count=::write(fd,data.constData()+offset,size_t(qMin<qsizetype>(65536,data.size()-offset)));if(count<0&&(errno==EINTR||errno==EAGAIN))continue;if(count<=0)return false;offset+=count;if(offset==data.size())return true;}
        else{char bytes[65536];const auto count=::read(fd,bytes,sizeof bytes);if(count<0&&(errno==EINTR||errno==EAGAIN))continue;if(count<0)return false;if(!count)return true;if(data.size()+count>limit)return false;data.append(bytes,count);}
    }
    return false;
}
void closeExtra()
{
#ifdef SYS_close_range
    if(syscall(SYS_close_range,3u,UINT_MAX,0)==0)return;
#endif
    const long top=sysconf(_SC_OPEN_MAX);for(int fd=3;fd<qMin<long>(top>0?top:65536,1048576);++fd)::close(fd);
}
bool validateInUnprivilegedProcess(const QByteArray &bytes,bool canonical,QString &error)
{
    QElapsedTimer lifetime;lifetime.start();
    int input[2],output[2];if(pipe2(input,O_CLOEXEC)){error=QStringLiteral("The image validation pipe could not be created.");return false;}
    if(pipe2(output,O_CLOEXEC)){close(input[0]);close(input[1]);error=QStringLiteral("The image validation pipe could not be created.");return false;}
    const auto executable=QFile::encodeName(QCoreApplication::applicationFilePath());const pid_t pid=fork();
    if(pid==0){
        if(dup2(input[0],STDIN_FILENO)<0||dup2(output[1],STDOUT_FILENO)<0)_exit(120);close(STDERR_FILENO);closeExtra();
        rlimit cpu{6,6},memory{1024ull*1024*1024,1024ull*1024*1024},core{0,0};
        if(setrlimit(RLIMIT_CPU,&cpu)||setrlimit(RLIMIT_AS,&memory)||setrlimit(RLIMIT_CORE,&core)||setgroups(0,nullptr)||setresgid(65534,65534,65534)||setresuid(65534,65534,65534)||prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))_exit(121);
        char *const args[]={const_cast<char *>(executable.constData()),const_cast<char *>(canonical?"--decode-prepared-image":"--decode-image"),nullptr};
        char *const env[]={const_cast<char *>("PATH=/usr/bin:/bin"),const_cast<char *>("LANG=C"),const_cast<char *>("LC_ALL=C"),const_cast<char *>("HOME=/nonexistent"),const_cast<char *>("QT_PLUGIN_PATH="),nullptr};execve(executable.constData(),args,env);_exit(122);
    }
    close(input[0]);close(output[1]);if(pid<0){close(input[1]);close(output[0]);error=QStringLiteral("An unprivileged image validator could not be started.");return false;}
    fcntl(input[1],F_SETFL,fcntl(input[1],F_GETFL)|O_NONBLOCK);fcntl(output[0],F_SETFL,fcntl(output[0],F_GETFL)|O_NONBLOCK);
    QByteArray send=bytes,reply;bool ok=transfer(input[1],send,AeroLoginTypes::MaximumImageBytes,true,qMax(1,10000-int(lifetime.elapsed())));close(input[1]);if(ok)ok=transfer(output[0],reply,64,false,qMax(1,10000-int(lifetime.elapsed())));close(output[0]);
    int status=0;ok=AeroLoginChild::finish(pid,qMax(0,10000-int(lifetime.elapsed())),ok,status)&&reply==QByteArray("ok\n");if(!ok)error=QStringLiteral("The picture failed independent unprivileged validation. No image decoder ran with administrative privileges.");return ok;
}
int reply(bool saved,const QString &error,bool pending,const QByteArray &revision={})
{QByteArray bytes=AeroLoginProtocol::encodeReply(saved,error,pending,revision);transfer(STDOUT_FILENO,bytes,AeroLoginProtocol::MaximumReplyBytes,true,2000);return error.isEmpty()&&saved?0:1;}
}
int main(int argc,char **argv)
{
    signal(SIGPIPE,SIG_IGN);
    const bool decoder=argc==2&&(!strcmp(argv[1],"--decode-image")||!strcmp(argv[1],"--decode-prepared-image"));
    if(decoder){
        if(geteuid()==0)return 2;
        qputenv("QT_PLUGIN_PATH","");QCoreApplication app(argc,argv);QCoreApplication::setLibraryPaths({QLibraryInfo::path(QLibraryInfo::PluginsPath)});QImageReader::setAllocationLimit(80);
        QByteArray bytes;fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK);if(!transfer(STDIN_FILENO,bytes,AeroLoginTypes::MaximumImageBytes,false,10000))return 3;
        const auto image=AeroLoginTypes::prepareImage(bytes);const bool ok=image.valid&&(strcmp(argv[1],"--decode-prepared-image")||image.bytes==bytes);const char *result=ok?"ok\n":"invalid\n";::write(STDOUT_FILENO,result,strlen(result));return ok?0:4;
    }
    if(argc!=2||(strcmp(argv[1],"--apply")&&strcmp(argv[1],"--recover")))return 2;
    if(geteuid()!=0){const char error[]="Administrative authorization is required.\n";::write(STDERR_FILENO,error,sizeof error-1);return 1;}
    qputenv("PATH","/usr/bin:/bin");qputenv("LANG","C");qputenv("LC_ALL","C");qputenv("QT_PLUGIN_PATH","");qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");qunsetenv("XDG_CONFIG_HOME");qunsetenv("XDG_CONFIG_DIRS");qunsetenv("XDG_DATA_HOME");qunsetenv("XDG_DATA_DIRS");
    QCoreApplication app(argc,argv);
    if(!trusted(QCoreApplication::applicationFilePath())||!trusted(QLibraryInfo::path(QLibraryInfo::PluginsPath)))return reply(false,QStringLiteral("The installed helper or decoder path is not a trusted root-owned system path."),false);
    QCoreApplication::setLibraryPaths({QLibraryInfo::path(QLibraryInfo::PluginsPath)});
    QByteArray input;fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK);if(!transfer(STDIN_FILENO,input,AeroLoginProtocol::MaximumRequestBytes,false,10000))return reply(false,QStringLiteral("The bounded request was incomplete or timed out."),false);
    AeroLoginFiles::Context context;QString error;const auto before=AeroLoginFiles::read(context,&error);
    if(!before.sourceValid||!before.trustedInstallation)return reply(false,error.isEmpty()?before.reason:error,before.recoveryPending,before.revision);
    AeroLoginSnapshot requested;bool recovery=false;if(!AeroLoginProtocol::decodeRequest(input,before,requested,recovery,error)||recovery!=(!strcmp(argv[1],"--recover")))return reply(false,error.isEmpty()?QStringLiteral("The helper command does not match the request mode."):error,before.recoveryPending,before.revision);
    const bool canonical=requested.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage;
    context.validateImage=[canonical](const QByteArray &bytes,QString &error){return validateInUnprivilegedProcess(bytes,canonical,error);};
    const auto result=recovery?AeroLoginFiles::recover(context,before.revision):AeroLoginFiles::apply(context,before,requested);
    return reply(result.success||result.saved,result.error,result.recoveryRequired,result.snapshot.revision);
}
