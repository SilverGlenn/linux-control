#pragma once
#include "settings/AeroLoginSettings.h"

namespace AeroLoginProtocol {
constexpr int Version=1;
constexpr qsizetype MaximumRequestBytes=24*1024*1024;
constexpr qsizetype MaximumReplyBytes=64*1024;
QByteArray encode(const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested,QString &error);
QByteArray encodeRecovery(const AeroLoginSnapshot &expected,QString &error);
bool decodeRequest(const QByteArray &bytes,const AeroLoginSnapshot &current,AeroLoginSnapshot &requested,bool &recovery,QString &error);
QByteArray encodeReply(bool saved,const QString &error,bool recoveryPending,const QByteArray &postRevision={});
bool decodeReply(const QByteArray &bytes,bool &saved,QString &error,bool &recoveryPending,QByteArray *postRevision=nullptr);
}
