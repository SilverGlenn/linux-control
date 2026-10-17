#include "AeroLoginPage.h"
#include "Win7Ui.h"
#include "Win7FormStyle.h"
#include <QBuffer>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QImageReader>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QtConcurrent>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr auto Generation="aeroLoginImageGeneration",PendingImage="aeroLoginPendingImage",PendingName="aeroLoginPendingImageName",PendingReason="aeroLoginPendingImageReason",SourceName="aeroLoginImageSource";
// Dynamic metadata may emit stateChanged through the app-owned session. Every
// mutation is an outward call: observers may destroy the session or its view.
bool metadata(QPointer<AeroLoginSettings> session,const char *key,const QVariant &value)
{ if (!session) return false; session->setProperty(key,value); return bool(session); }
bool clearPrepared(QPointer<AeroLoginSettings> session,const QString &source)
{
    return metadata(session,SourceName,source) && metadata(session,PendingImage,QVariant())
        && metadata(session,PendingName,QString()) && metadata(session,PendingReason,QString());
}
struct PreparedSelection { AeroLoginImage image; QString path,error; };
QLabel *plain(const QString &text,const char *name=nullptr)
{ auto *label=Win7::bodyLabel(text); label->setTextFormat(Qt::PlainText); if (name) label->setObjectName(QString::fromLatin1(name)); return label; }
QString boolText(const AeroLoginField &field) { return !field.known?QObject::tr("Unavailable"):field.value?QObject::tr("On"):QObject::tr("Off"); }
QString digestText(const QByteArray &digest) { return digest.isEmpty()?QObject::tr("unavailable"):QString::fromLatin1(digest.toHex()); }
QString assetText(const AeroLoginAsset &asset)
{ if (!asset.present) return QObject::tr("No image override"); return QObject::tr("%1 bytes; SHA-256 %2%3").arg(asset.size).arg(digestText(asset.digest),asset.readable?QString():QObject::tr("; unreadable")); }
QString imageText(const AeroLoginImage &image)
{ return QObject::tr("%1 × %2 pixels; %3; %4 bytes; SHA-256 %5").arg(image.width).arg(image.height).arg(image.format).arg(image.bytes.size()).arg(digestText(image.digest)); }
QString backgroundIntentText(AeroLoginBackgroundIntent intent)
{ switch (intent) { case AeroLoginBackgroundIntent::Keep:return QObject::tr("Keep saved asset"); case AeroLoginBackgroundIntent::ReplaceImage:return QObject::tr("Replace with selected image"); case AeroLoginBackgroundIntent::RemoveOverride:return QObject::tr("Remove image override"); case AeroLoginBackgroundIntent::RestoreApprovedImage:return QObject::tr("Restore approved image"); } return {}; }
PreparedSelection readSelection(const QString &path)
{
    PreparedSelection result; result.path=path;
    if (!QFileInfo(path).isAbsolute() || path.contains(QStringLiteral("://"))) { result.error=QObject::tr("Choose a local PNG or JPEG file."); return result; }
    const int descriptor=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    if (descriptor<0) { result.error=QObject::tr("The selected image could not be opened as a regular local file. Choose the file itself rather than a symbolic link."); return result; }
    QFile file; if (!file.open(descriptor,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)) { ::close(descriptor); result.error=QObject::tr("The selected image could not be read."); return result; }
    struct stat before{},after{};
    if (::fstat(file.handle(),&before)!=0 || !S_ISREG(before.st_mode)) { result.error=QObject::tr("The selected image must be a regular local file."); return result; }
    if (before.st_size<=0 || before.st_size>AeroLoginTypes::MaximumImageBytes) { result.error=QObject::tr("Choose an image file between 1 byte and 16 MiB."); return result; }
    const QByteArray bytes=file.read(AeroLoginTypes::MaximumImageBytes+1);
    if (file.error()!=QFile::NoError || bytes.size()!=before.st_size || ::fstat(file.handle(),&after)!=0 || before.st_dev!=after.st_dev || before.st_ino!=after.st_ino || before.st_size!=after.st_size || before.st_mtim.tv_sec!=after.st_mtim.tv_sec || before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec) { result.error=QObject::tr("The image changed or could not be read completely. Choose it again."); return result; }
    result.image=AeroLoginTypes::prepareImage(bytes); if (!result.image.valid) result.error=result.image.reason; return result;
}
QPixmap thumbnail(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size()>AeroLoginTypes::MaximumImageBytes) return {};
    QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer); const QSize size=reader.size(); const auto format=reader.format().toLower();
    if ((format!="png" && format!="jpeg" && format!="jpg") || size.width()<=0 || size.height()<=0 || qint64(size.width())*size.height()>AeroLoginTypes::MaximumPixels) return {};
    reader.setScaledSize(size.scaled(340,150,Qt::KeepAspectRatio)); return QPixmap::fromImage(reader.read());
}
void renderPreview(QLabel *label,const QByteArray &bytes,bool blue,const QString &description)
{
    label->clear(); label->setAccessibleDescription(description);
    if (blue) { label->setStyleSheet(QStringLiteral("color: white; background: #1D5F7A; border: 1px solid #879CAC;")); label->setText(QObject::tr("Blue login background\n(no image override)")); }
    else { label->setStyleSheet(QStringLiteral("color: #333333; background: #F0F3F6; border: 1px solid #AAB5BF;")); const auto pixmap=thumbnail(bytes); if (!pixmap.isNull()) label->setPixmap(pixmap); else label->setText(QObject::tr("Image estimate unavailable")); }
}
QString reviewText(const AeroLoginSnapshot &saved,const AeroLoginSnapshot &draft)
{
    QStringList lines=AeroLoginTypes::reviewChanges(saved,draft);
    lines<<QObject::tr("Image before: %1").arg(assetText(saved.background));
    lines<<QObject::tr("Image action: %1").arg(backgroundIntentText(draft.backgroundIntent));
    if (draft.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage) lines<<imageText(draft.replacement);
    if (draft.backgroundIntent==AeroLoginBackgroundIntent::RestoreApprovedImage) lines<<QObject::tr("Approved image: %1").arg(assetText(draft.approvedDefault));
    lines<<QObject::tr("Estimated login: %1").arg(AeroLoginTypes::loginProjection(draft));
    lines<<QObject::tr("Estimated lock screen: %1").arg(AeroLoginTypes::lockProjection(draft));
    lines<<QObject::tr("The four options affect login only. Startup animation and sound depend on the greeter's startup state and may not play at every sign-in.");
    return lines.join('\n');
}
int reviewDialog(QWidget *parent,const QString &title,const QString &text,const QString &acceptText,const char *name)
{
    auto *dialog=new QDialog(parent); QPointer<QDialog> guard(dialog); dialog->setObjectName(QString::fromLatin1(name)); dialog->setWindowTitle(title); dialog->resize(650,430); Win7::styleSettingsDialog(dialog); Win7::styleSettingsFields(dialog); auto *layout=new QVBoxLayout(dialog); auto *scroll=new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); auto *body=new QWidget; auto *bodyLayout=new QVBoxLayout(body); auto *label=plain(text,"aeroLoginReviewText"); label->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard); bodyLayout->addWidget(label); bodyLayout->addStretch(); scroll->setWidget(body); layout->addWidget(scroll); auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText(acceptText); layout->addWidget(buttons); QObject::connect(buttons,&QDialogButtonBox::accepted,dialog,&QDialog::accept); QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject); const int result=dialog->exec(); if (guard) guard->deleteLater(); return result;
}
}

AeroLoginPage::AeroLoginPage(QScrollArea *sidebar,QWidget *parent):AeroLoginPage(sidebar,AeroLoginSettings::instance(),parent) {}
AeroLoginPage::AeroLoginPage(QScrollArea *sidebar,AeroLoginSettings *session,QWidget *parent):QWidget(parent),m_session(session)
{
    Q_ASSERT(session); auto *layout=Win7::pageScaffold(this,sidebar,20,800); setProperty("settingsScope",QStringLiteral("aeroLogin")); Win7::styleSettingsControls(this); Win7::styleSettingsFields(this);
    layout->addWidget(Win7::pageTitle(tr("Aero Login"))); layout->addSpacing(8); layout->addWidget(plain(tr("Customize the Aero login screen. Changes remain staged until you review and apply them. Background images are shared with the Aero lock screen; their fallback appearances differ.")));
    layout->addSpacing(10); layout->addLayout(Win7::sectionHeading(tr("Login options")));
    const QMap<AeroLoginKey,QString> captions={{AeroLoginKey::StartupAnimation,tr("Enable startup &animation")},{AeroLoginKey::StartupSound,tr("Play startup &sound")},{AeroLoginKey::ForceUserSelection,tr("Always show &user selection")},{AeroLoginKey::RdpBackground,tr("Use &blue login background")}};
    const QMap<AeroLoginKey,QString> names={{AeroLoginKey::StartupAnimation,QStringLiteral("aeroLoginStartupAnimation")},{AeroLoginKey::StartupSound,QStringLiteral("aeroLoginStartupSound")},{AeroLoginKey::ForceUserSelection,QStringLiteral("aeroLoginForceUserSelection")},{AeroLoginKey::RdpBackground,QStringLiteral("aeroLoginRdpBackground")}};
    for (auto key:AeroLoginTypes::keys()) { auto *check=new QCheckBox(captions.value(key)); check->setObjectName(names.value(key)); check->setAccessibleName(AeroLoginTypes::label(key)); m_fields[key]=check; layout->addWidget(check); auto *warning=plain({},(names.value(key)+QStringLiteral("Warning")).toLatin1().constData()); m_fieldWarnings[key]=warning; layout->addWidget(warning);
        connect(check,&QCheckBox::clicked,this,[this,key](bool checked){ if (!m_session) return; m_inputMessage.clear(); if (key==AeroLoginKey::RdpBackground && checked) m_session->removeImage(); else m_session->selectValue(key,checked); }); }
    layout->addWidget(plain(tr("Startup animation and sound depend on the login screen's startup state; enabling them does not guarantee playback at every sign-in.")));
    layout->addSpacing(10); layout->addLayout(Win7::sectionHeading(tr("Shared background image")));
    auto *previews=new QGridLayout; previews->setColumnStretch(0,1); previews->setColumnStretch(1,1); previews->addWidget(plain(tr("Estimated login appearance")),0,0); previews->addWidget(plain(tr("Estimated lock-screen fallback")),0,1);
    m_loginPreview=new QLabel; m_loginPreview->setObjectName(QStringLiteral("aeroLoginPreview")); m_lockPreview=new QLabel; m_lockPreview->setObjectName(QStringLiteral("aeroLockPreview")); for (auto *preview:{m_loginPreview,m_lockPreview}) { preview->setAlignment(Qt::AlignCenter); preview->setMinimumSize(240,150); preview->setTextFormat(Qt::PlainText); preview->setAccessibleName(preview==m_loginPreview?tr("Static estimated login background"):tr("Static estimated lock background")); } previews->addWidget(m_loginPreview,1,0); previews->addWidget(m_lockPreview,1,1); layout->addLayout(previews);
    m_imageSummary=plain({},"aeroLoginImageSummary"); layout->addWidget(m_imageSummary); m_effects=plain({},"aeroLoginEffects"); layout->addWidget(m_effects);
    auto *imageButtons=new QHBoxLayout; m_choose=new QPushButton(tr("&Choose image…")); m_choose->setObjectName(QStringLiteral("aeroLoginChooseImage")); m_remove=new QPushButton(tr("Re&move override")); m_remove->setObjectName(QStringLiteral("aeroLoginRemoveImage")); m_restore=new QPushButton(tr("Restore appr&oved image")); m_restore->setObjectName(QStringLiteral("aeroLoginRestoreImage")); imageButtons->addWidget(m_choose); imageButtons->addWidget(m_remove); imageButtons->addWidget(m_restore); imageButtons->addStretch(); layout->addLayout(imageButtons);
    m_pending=plain({},"aeroLoginPendingImage"); layout->addWidget(m_pending); m_reviewImage=new QPushButton(tr("Review selected &image…")); m_reviewImage->setObjectName(QStringLiteral("aeroLoginReviewImage")); layout->addWidget(m_reviewImage);
    auto *screenLock=new QPushButton(tr("Screen &Locking…")); screenLock->setObjectName(QStringLiteral("aeroLoginScreenLock")); auto *lockRow=new QHBoxLayout; lockRow->addWidget(screenLock); lockRow->addStretch(); layout->addLayout(lockRow);
    auto *detailsToggle=new QPushButton(tr("Show configuration &details")); detailsToggle->setObjectName(QStringLiteral("aeroLoginDetailsToggle")); detailsToggle->setCheckable(true); auto *detailsRow=new QHBoxLayout; detailsRow->addWidget(detailsToggle); detailsRow->addStretch(); layout->addLayout(detailsRow); m_details=new QWidget; m_details->setObjectName(QStringLiteral("aeroLoginDetails")); auto *detailsLayout=new QVBoxLayout(m_details); detailsLayout->setContentsMargins(0,0,0,0); m_detailsText=plain({},"aeroLoginDetailsText"); m_detailsText->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard); detailsLayout->addWidget(m_detailsText); layout->addWidget(m_details); m_details->setVisible(false); connect(detailsToggle,&QPushButton::toggled,this,[this,detailsToggle](bool open){ m_details->setVisible(open); detailsToggle->setText(open?tr("Hide configuration &details"):tr("Show configuration &details")); if (m_session) m_session->setProperty("aeroLoginDetailsExpanded",open); }); detailsToggle->setChecked(session->property("aeroLoginDetailsExpanded").toBool());
    m_changes=plain({},"aeroLoginChanges"); layout->addWidget(m_changes); m_message=plain({},"aeroLoginMessage"); layout->addWidget(m_message);
    auto *buttons=new QHBoxLayout; m_defaults=new QPushButton(tr("Restore approved defaul&ts…")); m_defaults->setObjectName(QStringLiteral("aeroLoginDefaults")); m_refresh=new QPushButton(tr("Re&fresh")); m_refresh->setObjectName(QStringLiteral("aeroLoginRefresh")); m_revert=new QPushButton(tr("Re&vert")); m_revert->setObjectName(QStringLiteral("aeroLoginRevert")); m_apply=new QPushButton(tr("A&pply…")); m_apply->setObjectName(QStringLiteral("aeroLoginApply")); m_recover=new QPushButton(tr("Review recover&y…")); m_recover->setObjectName(QStringLiteral("aeroLoginRecover")); buttons->addWidget(m_defaults); buttons->addWidget(m_refresh); buttons->addWidget(m_recover); buttons->addStretch(); buttons->addWidget(m_revert); buttons->addWidget(m_apply); layout->addLayout(buttons); layout->addStretch();
    connect(m_choose,&QPushButton::clicked,this,[this]{ if (m_chooserQueued) return; m_chooserQueued=true; QTimer::singleShot(0,this,[this]{ m_chooserQueued=false; chooseImage(); }); });
    connect(m_remove,&QPushButton::clicked,this,[this]{ m_inputMessage.clear(); if (m_session) m_session->removeImage(); }); connect(m_restore,&QPushButton::clicked,this,[this]{ m_inputMessage.clear(); if (m_session) m_session->restoreImage(); }); connect(m_reviewImage,&QPushButton::clicked,this,&AeroLoginPage::reviewPreparedImage);
    connect(screenLock,&QPushButton::clicked,this,[this]{ emit navigationRequested(PageId::ScreenLock); }); connect(m_apply,&QPushButton::clicked,this,&AeroLoginPage::reviewApply); connect(m_defaults,&QPushButton::clicked,this,&AeroLoginPage::reviewDefaults); connect(m_recover,&QPushButton::clicked,this,&AeroLoginPage::reviewRecovery); connect(m_refresh,&QPushButton::clicked,this,[this]{ if (m_session) m_session->refresh(); }); connect(m_revert,&QPushButton::clicked,this,[this]{ if (m_session) { m_waitRevert=true; m_session->revert(); } });
    connect(session,&SettingsSession::stateChanged,this,&AeroLoginPage::updateState); connect(session,&QObject::destroyed,this,&AeroLoginPage::updateState); session->installEventFilter(this); updateState();
}
SettingsSession *AeroLoginPage::settingsSession() const { return m_session; }
QList<SidebarLink> AeroLoginPage::sidebarLinks() { return {Nav::to(tr("Screen Locking"),PageId::ScreenLock)}; }
QList<SidebarLink> AeroLoginPage::sidebarSeeAlso() { return {Nav::to(tr("Personalization"),PageId::Personalization),Nav::to(tr("User Accounts"),PageId::UserAccounts)}; }
bool AeroLoginPage::eventFilter(QObject *object,QEvent *event)
{ if (object==m_session && event->type()==QEvent::DynamicPropertyChange) { const auto *change=static_cast<QDynamicPropertyChangeEvent *>(event); if (change->propertyName().startsWith("aeroLogin")) updateState(); } return QWidget::eventFilter(object,event); }
bool AeroLoginPage::currentContext(const AeroLoginSnapshot &saved,const AeroLoginSnapshot &draft) const
{ return m_session && m_session->isReady() && !m_session->isBusy() && !m_session->hasConflict() && AeroLoginTypes::sameConfiguration(saved,m_session->applied()) && AeroLoginTypes::sameDraft(draft,m_session->staged()); }

void AeroLoginPage::chooseImage()
{
    if (!m_session || !m_session->canEdit()) return;
    auto *dialog=new QFileDialog(this,tr("Choose a local login background")); QPointer<QFileDialog> guard(dialog); QPointer<AeroLoginPage> page(this); dialog->setObjectName(QStringLiteral("aeroLoginImageFileDialog")); dialog->setOption(QFileDialog::DontUseNativeDialog,true); dialog->setFileMode(QFileDialog::ExistingFile); dialog->setAcceptMode(QFileDialog::AcceptOpen); dialog->setNameFilters({tr("PNG and JPEG images (*.png *.jpg *.jpeg)"),tr("PNG images (*.png)"),tr("JPEG images (*.jpg *.jpeg)")}); dialog->setSupportedSchemes({QStringLiteral("file")}); Win7::styleSettingsDialog(dialog); Win7::styleSettingsFields(dialog); dialog->setStyleSheet(dialog->styleSheet()+QStringLiteral(" QLineEdit { border-image: none; color: #000000; background: #FFFFFF; border: 1px solid #ABADB3; padding: 3px; } QAbstractItemView { border-image: none; color: #000000; background: #FFFFFF; selection-background-color: #CDE6F7; selection-color: #000000; }"));
    const int result=dialog->exec(); if (!page) return; const QString path=guard && !guard->selectedFiles().isEmpty()?guard->selectedFiles().first():QString(); if (guard) guard->deleteLater(); if (result==QDialog::Accepted && !path.isEmpty()) prepareSelection(path);
}
void AeroLoginPage::prepareSelection(const QString &path)
{
    if (!m_session || !m_session->canEdit()) return;
    QPointer<AeroLoginSettings> session=m_session; const auto baseline=session->applied(),draft=session->staged(); const quint64 generation=session->property(Generation).toULongLong()+1;
    if (!metadata(session,Generation,QVariant::fromValue(generation)) || !metadata(session,"aeroLoginImagePreparingName",path) || !metadata(session,"aeroLoginImageError",QString())) return;
    if (!session->beginImagePreparation() || !session) return;
    auto *watcher=new QFutureWatcher<PreparedSelection>(session);
    QObject::connect(watcher,&QFutureWatcher<PreparedSelection>::finished,session,[watcher,session,baseline,draft,generation]{
        const auto result=watcher->result(); watcher->deleteLater(); if (!session) return;
        if (session->property(Generation).toULongLong()!=generation) { session->endImagePreparation(); return; }
        if (result.image.valid) { if (!metadata(session,PendingImage,QVariant::fromValue(result.image)) || !metadata(session,PendingName,result.path) || !metadata(session,PendingReason,QString())) return; }
        else if (!metadata(session,"aeroLoginImageError",result.error.isEmpty()?QObject::tr("The selected image is not a supported PNG or JPEG."):result.error)) return;
        if (!metadata(session,"aeroLoginImagePreparingName",QString())) return; session->endImagePreparation(); if (!session || !result.image.valid) return;
        auto desired=session->staged(); QString error;
        if (session->canEdit() && AeroLoginTypes::sameConfiguration(baseline,session->applied()) && AeroLoginTypes::sameDraft(draft,session->staged()) && AeroLoginTypes::selectImage(desired,session->applied(),result.image,error)) {
            session->selectImage(result.image); if (!session) return;
            if (AeroLoginTypes::sameDraft(desired,session->staged())) { clearPrepared(session,result.path); return; }
        }
        metadata(session,PendingReason,QObject::tr("The image is prepared but was not staged because the saved settings, draft or editing availability changed. Review the selected image against the current values."));
    });
    watcher->setFuture(QtConcurrent::run([path]{ return readSelection(path); }));
}
void AeroLoginPage::reviewPreparedImage()
{
    if (!m_session || !m_session->canEdit()) return; const auto image=m_session->property(PendingImage).value<AeroLoginImage>(); if (!image.valid) return;
    const auto baseline=m_session->applied(),draft=m_session->staged(); auto desired=draft; QString error;
    if (!AeroLoginTypes::selectImage(desired,baseline,image,error)) { m_inputMessage=error; updateState(); return; }
    const QString name=m_session->property(PendingName).toString(); QPointer<AeroLoginPage> page(this);
    const int result=reviewDialog(this,tr("Review selected image"),tr("Selected file: %1\n%2\n\n%3\n\nThis stages the image only. Apply remains a separate action.").arg(name,imageText(image),reviewText(baseline,desired)),tr("Stage image"),"aeroLoginImageReviewDialog"); if (!page) return;
    if (result==QDialog::Accepted) {
        if (!currentContext(baseline,draft) || !m_session->canEdit() || !(m_session->property(PendingImage).value<AeroLoginImage>()==image)) m_inputMessage=tr("The selected-image review is stale. The prepared image is retained for another review.");
        else { m_inputMessage.clear(); QPointer<AeroLoginSettings> session=m_session; session->selectImage(image); if (session && AeroLoginTypes::sameDraft(desired,session->staged())) clearPrepared(session,name); }
    }
    if (page) updateState();
}
void AeroLoginPage::reviewApply()
{
    if (!m_session || !m_session->canApply() || !m_session->isDirty()) return; const auto baseline=m_session->applied(),draft=m_session->staged(); QPointer<AeroLoginPage> page(this);
    const int result=reviewDialog(this,tr("Review Aero login changes"),reviewText(baseline,draft)+tr("\n\nContinuing may request administrator authorization. Saved files will be checked; existing login and lock screens are not restarted or tested. Future display adoption is unverified."),tr("Apply reviewed changes"),"aeroLoginApplyReviewDialog"); if (!page) return;
    if (result==QDialog::Accepted) { if (!currentContext(baseline,draft) || !m_session->canApply()) m_inputMessage=tr("The Apply review is stale. Your draft and selected image are retained; review them again."); else { m_inputMessage.clear(); m_session->apply(); } }
    if (page) updateState();
}
void AeroLoginPage::reviewDefaults()
{
    if (!m_session || !m_session->canEdit()) return; const auto baseline=m_session->applied(),draft=m_session->staged(); auto candidate=draft; QString error;
    for (auto key:AeroLoginTypes::keys()) if (!AeroLoginTypes::restore(candidate,baseline,key,error)) { m_inputMessage=error; updateState(); return; }
    if (!AeroLoginTypes::restoreImage(candidate,baseline,error)) { m_inputMessage=error; updateState(); return; }
    QPointer<AeroLoginPage> page(this); const int result=reviewDialog(this,tr("Review approved Aero defaults"),tr("Restore the four approved login options and the approved background image. Other settings and the selected SDDM theme are preserved. These changes will be staged only.\n\n%1").arg(reviewText(baseline,candidate)),tr("Stage approved defaults"),"aeroLoginDefaultsReviewDialog"); if (!page) return;
    if (result==QDialog::Accepted) { if (!currentContext(baseline,draft) || !m_session->canEdit()) m_inputMessage=tr("The approved-defaults review is stale. Review the current settings again."); else { m_inputMessage.clear(); m_session->restoreDefaults(); } }
    if (page) updateState();
}
void AeroLoginPage::reviewRecovery()
{
    if (!m_session || !m_session->canRecover()) return; const auto baseline=m_session->observed(),draft=m_session->staged(); QPointer<AeroLoginPage> page(this);
    const int result=reviewDialog(this,tr("Review Aero login recovery"),tr("An unfinished background/settings transaction needs recovery.\n\n%1\n\nRecovery checks the saved transaction and current files before making any repair. It may request administrator authorization. Your draft and selected image will be retained; foreign edits must not be overwritten.").arg(baseline.recoverySummary),tr("Recover reviewed transaction"),"aeroLoginRecoveryReviewDialog"); if (!page) return;
    if (result==QDialog::Accepted) { if (!m_session || m_session->isBusy() || !m_session->canRecover() || !AeroLoginTypes::sameConfiguration(baseline,m_session->observed()) || !AeroLoginTypes::sameDraft(draft,m_session->staged())) m_inputMessage=tr("The recovery review is stale. Refresh and review the current recovery state."); else { m_inputMessage.clear(); m_session->recover(); } }
    if (page) updateState();
}

void AeroLoginPage::updateState()
{
    if (m_updating) return; m_updating=true;
    if (!m_session) { setEnabled(false); m_message->setText(tr("The settings session is no longer available.")); m_updating=false; return; }
    if (m_waitRevert && !m_session->isBusy()) {
        m_waitRevert=false;
        if (m_session->isReady() && !m_session->isDirty() && !m_session->hasConflict() && m_session->errorText().isEmpty()) {
            m_inputMessage.clear();
        }
    }
    const auto &draft=m_session->staged(),&saved=m_session->applied(),&observed=m_session->observed();
    for (auto key:AeroLoginTypes::keys()) {
        auto *check=m_fields.value(key); const auto field=draft.fields.value(key); const QSignalBlocker block(check); check->setTristate(!field.known); check->setCheckState(!field.known?Qt::PartiallyChecked:field.value?Qt::Checked:Qt::Unchecked); check->setEnabled(m_session->canEdit(key));
        QStringList warning; if (!field.known) warning<<tr("The saved value for %1 is unavailable. An explicit choice is needed before it can be changed.").arg(AeroLoginTypes::label(key));
        if (field.known && !field.canonical && !draft.intents.contains(key)) warning<<tr("Saved spelling '%1' is read by the login screen as %2; it is preserved until you choose a replacement.").arg(field.raw,boolText(field));
        if (field.known && field.editorKnown && field.editorValue!=field.value && !draft.intents.contains(key)) warning<<tr("The native editor reads this value differently (%1). The login screen's effective value is shown here.").arg(field.editorValue?tr("On"):tr("Off"));
        auto *label=m_fieldWarnings.value(key); label->setText(warning.join(' ')); label->setVisible(!warning.isEmpty()); check->setToolTip(field.reason);
    }
    const bool removed=draft.backgroundIntent==AeroLoginBackgroundIntent::RemoveOverride;
    const bool replacement=draft.backgroundIntent==AeroLoginBackgroundIntent::ReplaceImage;
    const bool restored=draft.backgroundIntent==AeroLoginBackgroundIntent::RestoreApprovedImage;
    const bool absent=removed || (!replacement && !restored && !draft.background.present);
    const QByteArray loginBytes=replacement?draft.replacement.bytes:restored?draft.approvedDefault.bytes:draft.background.bytes;
    const QByteArray lockBytes=absent?draft.approvedDefault.bytes:loginBytes;
    renderPreview(m_loginPreview,loginBytes,absent,AeroLoginTypes::loginProjection(draft)); renderPreview(m_lockPreview,lockBytes,false,AeroLoginTypes::lockProjection(draft));
    QString summary;
    if (replacement) summary=tr("Selected image: %1\n%2").arg(m_session->property(SourceName).toString().isEmpty()?tr("prepared image retained in this draft"):m_session->property(SourceName).toString(),imageText(draft.replacement));
    else if (restored) summary=tr("Approved background image staged: %1").arg(assetText(draft.approvedDefault));
    else summary=removed?tr("Image override will be removed. Saved image: %1").arg(assetText(saved.background)):tr("Saved image: %1").arg(assetText(draft.background));
    const QByteArray shownBytes=restored?draft.approvedDefault.bytes:draft.background.bytes;
    if (!replacement && !absent && shownBytes.size()<=AeroLoginTypes::MaximumImageBytes) { QBuffer buffer; buffer.setData(shownBytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer); const QSize dimensions=reader.size(); if (dimensions.isValid() && qint64(dimensions.width())*dimensions.height()<=AeroLoginTypes::MaximumPixels) summary+=tr("\nImage dimensions: %1 × %2 pixels.").arg(dimensions.width()).arg(dimensions.height()); }
    m_imageSummary->setText(summary);
    QString effects=tr("Estimated login: %1.\nEstimated lock screen: %2.").arg(absent?tr("blue background"):tr("shared background image"),absent?(draft.approvedDefault.present?tr("approved-image fallback"):tr("no approved-image fallback available")):tr("shared background image"));
    effects+=tr("\nThese are static estimates. Existing login and lock screens are not refreshed or tested here.");
    if (draft.fields.value(AeroLoginKey::RdpBackground).known && draft.fields.value(AeroLoginKey::RdpBackground).value && draft.background.present && draft.backgroundIntent==AeroLoginBackgroundIntent::Keep) effects+=tr("\nThe saved blue-background option is On but an image override still exists. Selecting blue login background removes that override explicitly.");
    if (absent && draft.fields.value(AeroLoginKey::RdpBackground).known && !draft.fields.value(AeroLoginKey::RdpBackground).value) effects+=tr("\nTurning the blue-background option Off does not recreate an image. Choose an image or restore the approved image explicitly.");
    m_effects->setText(effects);
    const auto pending=m_session->property(PendingImage).value<AeroLoginImage>(); QStringList pendingText;
    if (m_session->isPreparingImage()) pendingText<<tr("Preparing local image: %1. No image has been staged yet.").arg(m_session->property("aeroLoginImagePreparingName").toString());
    if (pending.valid) pendingText<<tr("Prepared selection retained: %1\n%2\n%3").arg(m_session->property(PendingName).toString(),imageText(pending),m_session->property(PendingReason).toString());
    if (!m_session->property("aeroLoginImageError").toString().isEmpty()) pendingText<<m_session->property("aeroLoginImageError").toString();
    m_pending->setText(pendingText.join('\n')); m_pending->setVisible(!pendingText.isEmpty()); m_reviewImage->setVisible(pending.valid); m_reviewImage->setEnabled(pending.valid && m_session->canEdit());
    auto removeCandidate=draft,restoreCandidate=draft,defaultCandidate=draft; QString error;
    const bool editable=m_session->canEdit(); const bool canRemove=editable && AeroLoginTypes::removeImage(removeCandidate,saved,error); const bool canRestore=editable && AeroLoginTypes::restoreImage(restoreCandidate,saved,error);
    bool canDefault=editable; for (auto key:AeroLoginTypes::keys()) if (canDefault) canDefault=AeroLoginTypes::restore(defaultCandidate,saved,key,error); if (canDefault) canDefault=AeroLoginTypes::restoreImage(defaultCandidate,saved,error);
    m_choose->setEnabled(editable); m_remove->setEnabled(canRemove); m_restore->setEnabled(canRestore); m_defaults->setEnabled(canDefault); m_refresh->setEnabled(!m_session->isBusy()); m_revert->setEnabled(!m_session->isBusy() && (m_session->isDirty() || m_session->hasConflict() || pending.valid || !m_session->isReady() || !m_session->property("aeroLoginImageError").toString().isEmpty())); m_apply->setEnabled(m_session->isDirty() && m_session->canApply()); m_recover->setVisible(observed.recoveryPending); m_recover->setEnabled(m_session->canRecover());
    QStringList details; details<<tr("Fixed configuration target: sddm-theme-mod. The active SDDM theme selection is preserved.");
    for (auto key:AeroLoginTypes::keys()) { const auto field=saved.fields.value(key); details<<tr("%1: effective %2; user entry %3; base entry %4; raw '%5'; base raw '%6'; native editor %7.").arg(AeroLoginTypes::label(key),boolText(field),field.localPresent?tr("present"):tr("absent"),field.basePresent?tr("present"):tr("absent"),field.raw,field.baseRaw,!field.editorKnown?tr("unavailable"):field.editorValue?tr("On"):tr("Off")); }
    details<<tr("Saved override: %1\nApproved image: %2").arg(assetText(saved.background),assetText(saved.approvedDefault)); details<<AeroLoginTypes::loginProjection(draft)<<AeroLoginTypes::lockProjection(draft); details<<tr("Saved file verification does not acknowledge adoption by a running or future greeter. Startup animation/sound are also gated by the greeter's startup marker; the four booleans do not configure the lock screen."); details<<tr("Latest source revision: %1; helper revision: %2.").arg(QString::fromLatin1(observed.revision.toHex()),QString::fromLatin1(observed.helperRevision.toHex())); details+=observed.issues; m_detailsText->setText(details.join('\n'));
    const QStringList changes=AeroLoginTypes::reviewChanges(saved,draft); m_changes->setText(tr("Staged changes:\n%1").arg(changes.join('\n'))); m_changes->setVisible(AeroLoginTypes::dirty(draft));
    QStringList messages; if (!m_inputMessage.isEmpty()) messages<<m_inputMessage; if (!m_session->errorText().isEmpty()) messages<<m_session->errorText(); if (!m_session->resultText().isEmpty()) messages<<m_session->resultText(); if (!m_session->applySupportText().isEmpty()) messages<<m_session->applySupportText(); if (!editable && !observed.reason.isEmpty()) messages<<observed.reason; if (!editable && !observed.helperReason.isEmpty()) messages<<observed.helperReason; if (observed.recoveryPending) messages<<observed.recoverySummary; if (m_session->isBusy() && !m_session->isPreparingImage()) messages<<tr("Checking or saving Aero login settings…"); messages.removeDuplicates(); m_message->setText(messages.join('\n')); m_message->setVisible(!messages.isEmpty()); m_updating=false;
}
