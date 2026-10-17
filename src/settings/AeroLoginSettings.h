#pragma once
#include "SettingsSession.h"
#include <QMap>
#include <QVariant>

enum class AeroLoginKey { StartupAnimation, StartupSound, ForceUserSelection, RdpBackground };
enum class AeroLoginIntent { Set, RestoreApprovedDefault };
enum class AeroLoginBackgroundIntent { Keep, ReplaceImage, RemoveOverride, RestoreApprovedImage };
struct AeroLoginField {
    bool value=false, defaultValue=false, known=false, defaultKnown=false;
    bool localPresent=false, basePresent=false, userEmpty=false, canonical=true;
    bool editorKnown=false, editorValue=false, writable=false;
    QString raw, baseRaw, reason;
    bool operator==(const AeroLoginField &other) const;
};
struct AeroLoginImage {
    QByteArray bytes, digest;
    int width=0, height=0;
    QString format, reason;
    bool valid=false;
    bool operator==(const AeroLoginImage &other) const;
};
struct AeroLoginAsset {
    bool present=false, readable=false;
    qint64 size=0;
    QByteArray bytes, digest;
    QString reason;
    bool operator==(const AeroLoginAsset &other) const;
};
struct AeroLoginSnapshot {
    QMap<AeroLoginKey,AeroLoginField> fields;
    AeroLoginAsset background, approvedDefault;
    QByteArray revision, helperRevision;
    bool sourceValid=false, trustedInstallation=false, writable=false, helperAvailable=false;
    bool recoveryPending=false, canRecover=false;
    QString reason, helperReason, recoverySummary;
    QStringList issues;
    QMap<AeroLoginKey,AeroLoginIntent> intents;
    AeroLoginBackgroundIntent backgroundIntent=AeroLoginBackgroundIntent::Keep;
    AeroLoginImage replacement;
};
Q_DECLARE_METATYPE(AeroLoginSnapshot)
Q_DECLARE_METATYPE(AeroLoginImage)
namespace AeroLoginTypes {
constexpr qint64 MaximumImageBytes=16*1024*1024;
constexpr qint64 MaximumPixels=16*1024*1024;
QList<AeroLoginKey> keys();
QString keyName(AeroLoginKey key);
QString label(AeroLoginKey key);
bool builtinEditorDefault(AeroLoginKey key);
bool sameConfiguration(const AeroLoginSnapshot &a,const AeroLoginSnapshot &b);
bool sameDraft(const AeroLoginSnapshot &a,const AeroLoginSnapshot &b);
bool dirty(const AeroLoginSnapshot &snapshot);
// Preparation is unprivileged. The helper independently validates the supplied
// bytes in a child which drops privileges before loading an image decoder.
AeroLoginImage prepareImage(const QByteArray &bytes);
bool stage(AeroLoginSnapshot &draft,const AeroLoginSnapshot &baseline,AeroLoginKey key,bool value,QString &error);
bool restore(AeroLoginSnapshot &draft,const AeroLoginSnapshot &baseline,AeroLoginKey key,QString &error);
bool selectImage(AeroLoginSnapshot &draft,const AeroLoginSnapshot &baseline,const AeroLoginImage &image,QString &error);
bool removeImage(AeroLoginSnapshot &draft,const AeroLoginSnapshot &baseline,QString &error);
bool restoreImage(AeroLoginSnapshot &draft,const AeroLoginSnapshot &baseline,QString &error);
bool requestValid(const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested,QString &error);
bool matchesRequest(const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested,const AeroLoginSnapshot &actual,QString &error);
QString loginProjection(const AeroLoginSnapshot &draft);
QString lockProjection(const AeroLoginSnapshot &draft);
QStringList reviewChanges(const AeroLoginSnapshot &baseline,const AeroLoginSnapshot &draft);
}
class AeroLoginBackend:public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void readCurrent(quint64 token)=0;
    virtual void writeChanges(quint64 token,const AeroLoginSnapshot &expected,const AeroLoginSnapshot &requested)=0;
    virtual void recover(quint64 token,const AeroLoginSnapshot &expected)=0;
signals:
    void readFinished(quint64 token,bool success,const AeroLoginSnapshot &snapshot,const QString &error);
    void writeFinished(quint64 token,bool saved,const QString &error,const QByteArray &postRevision={});
    void operationPending(quint64 token,const QString &message);
    void externalChange();
};
class AeroLoginSettings final:public SettingsSession {
    Q_OBJECT
public:
    static AeroLoginSettings *instance();
    explicit AeroLoginSettings(AeroLoginBackend *backend,QObject *parent=nullptr);
    const AeroLoginSnapshot &applied() const{return m_applied;}
    const AeroLoginSnapshot &staged() const{return m_staged;}
    const AeroLoginSnapshot &observed() const{return m_observed;}
    bool isReady() const{return m_ready;}
    bool isPreparingImage() const{return m_imagePreparing;}
    bool beginImagePreparation();
    void endImagePreparation();
    bool canEdit() const;
    bool canEdit(AeroLoginKey key) const;
    bool canApply() const;
    bool canRecover() const;
    bool hasImageReview() const;
    QString applySupportText() const;
public slots:
    void selectValue(AeroLoginKey key,bool value);
    void restoreDefault(AeroLoginKey key);
    void restoreDefaults();
    void selectImage(const AeroLoginImage &image);
    void removeImage();
    void restoreImage();
    void recover();
    void apply() override;
    void revert() override;
    void refresh();
protected:
    bool event(QEvent *event) override;
private:
    enum class Phase{Idle,Refresh,Revert,Preflight,Write,Readback,RecoveryPreflight,RecoveryReadback};
    void beginRead(Phase phase);
    void onRead(quint64 token,bool success,const AeroLoginSnapshot &snapshot,const QString &error);
    void onWritten(quint64 token,bool saved,const QString &error,const QByteArray &postRevision);
    void edited();
    void finish(bool applying=false,bool success=false);
    AeroLoginBackend *m_backend;
    AeroLoginSnapshot m_applied,m_staged,m_expected,m_requested,m_observed;
    Phase m_phase=Phase::Idle;
    quint64 m_token=0;
    bool m_ready=false,m_pendingRefresh=false,m_uncertain=false,m_saved=false,m_recovery=false,m_imagePreparing=false;
    QString m_writeError;
    QByteArray m_savedRevision;
};
