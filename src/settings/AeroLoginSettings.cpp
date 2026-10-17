#include "AeroLoginSettings.h"
#include "platform/kde/AeroLoginBackend.h"
#include <QCoreApplication>
#include <QDynamicPropertyChangeEvent>
#include <QPointer>
#include <QTimer>
namespace {
bool usable(const AeroLoginSnapshot &s){return s.sourceValid&&s.trustedInstallation&&s.writable&&s.helperAvailable&&!s.recoveryPending;}
}
AeroLoginSettings *AeroLoginSettings::instance()
{static QPointer<AeroLoginSettings> session;if(!session)session=new AeroLoginSettings(new KdeAeroLoginBackend,QCoreApplication::instance());return session;}
AeroLoginSettings::AeroLoginSettings(AeroLoginBackend *backend,QObject *parent):SettingsSession(parent),m_backend(backend)
{
    Q_ASSERT(backend);backend->setParent(this);
    connect(backend,&AeroLoginBackend::readFinished,this,&AeroLoginSettings::onRead);
    connect(backend,&AeroLoginBackend::writeFinished,this,&AeroLoginSettings::onWritten);
    connect(backend,&AeroLoginBackend::externalChange,this,&AeroLoginSettings::refresh);
    connect(backend,&AeroLoginBackend::operationPending,this,[this](quint64 token,const QString &message){if(token==m_token&&m_phase==Phase::Write){m_error=message;emit stateChanged();}});
    m_busy=true;QTimer::singleShot(0,this,[this]{beginRead(Phase::Refresh);});
}
bool AeroLoginSettings::canEdit() const{return m_ready&&!m_busy&&!m_conflict&&!m_uncertain&&usable(m_observed);}
bool AeroLoginSettings::canEdit(AeroLoginKey key) const{return canEdit()&&AeroLoginTypes::keys().contains(key)&&m_applied.fields.value(key).writable;}
bool AeroLoginSettings::canRecover() const{return m_ready&&!m_busy&&m_observed.sourceValid&&m_observed.trustedInstallation&&m_observed.helperAvailable&&m_observed.recoveryPending&&m_observed.canRecover;}
bool AeroLoginSettings::hasImageReview() const{return property("aeroLoginPendingImage").value<AeroLoginImage>().valid;}
bool AeroLoginSettings::event(QEvent *event)
{
    const bool result=SettingsSession::event(event);
    if(event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()==QByteArray("aeroLoginPendingImage")){
        const bool dirty=AeroLoginTypes::dirty(m_staged)||hasImageReview();if(dirty!=m_dirty){m_dirty=dirty;emit stateChanged();}
    }
    return result;
}
QString AeroLoginSettings::applySupportText() const
{
    if(m_busy)return m_imagePreparing?tr("Preparing the selected picture…"):tr("Checking preferences or waiting for the authorized change to finish…");
    if(m_conflict||m_uncertain)return tr("The source changed or the result is uncertain. Your draft is retained. Recover an interrupted change if offered, then Revert successfully before applying.");
    if(!m_ready)return tr("Aero login preferences have not been read successfully.");
    if(m_observed.recoveryPending)return tr("An interrupted appearance change needs reviewed recovery before another Apply.");
    if(!usable(m_observed))return !m_observed.reason.isEmpty()?m_observed.reason:!m_observed.helperReason.isEmpty()?m_observed.helperReason:tr("The approved Aero installation and administrative helper are not ready for changes.");
    if(hasImageReview())return tr("Review the prepared picture before applying, or Revert to discard it.");
    QString error;if(!AeroLoginTypes::requestValid(m_applied,m_staged,error))return error;return {};
}
bool AeroLoginSettings::canApply() const{return applySupportText().isEmpty();}
void AeroLoginSettings::edited(){m_dirty=AeroLoginTypes::dirty(m_staged)||hasImageReview();m_error.clear();m_result.clear();emit stateChanged();}
void AeroLoginSettings::selectValue(AeroLoginKey k,bool v)
{if(!canEdit(k))return;QString error;if(!AeroLoginTypes::stage(m_staged,m_applied,k,v,error)){m_error=error;emit stateChanged();return;}edited();}
void AeroLoginSettings::restoreDefault(AeroLoginKey k)
{if(!canEdit(k))return;QString error;if(!AeroLoginTypes::restore(m_staged,m_applied,k,error)){m_error=error;emit stateChanged();return;}edited();}
void AeroLoginSettings::restoreDefaults()
{
    if(!canEdit())return;auto next=m_staged;QString error;
    for(auto key:AeroLoginTypes::keys())if(!AeroLoginTypes::restore(next,m_applied,key,error)){m_error=error;emit stateChanged();return;}
    if(!AeroLoginTypes::restoreImage(next,m_applied,error)){m_error=error;emit stateChanged();return;}m_staged=next;edited();
}
void AeroLoginSettings::selectImage(const AeroLoginImage &i)
{if(!canEdit())return;QString error;if(!AeroLoginTypes::selectImage(m_staged,m_applied,i,error)){m_error=error;emit stateChanged();return;}edited();}
void AeroLoginSettings::removeImage()
{if(!canEdit())return;QString error;if(!AeroLoginTypes::removeImage(m_staged,m_applied,error)){m_error=error;emit stateChanged();return;}edited();}
void AeroLoginSettings::restoreImage()
{if(!canEdit())return;QString error;if(!AeroLoginTypes::restoreImage(m_staged,m_applied,error)){m_error=error;emit stateChanged();return;}edited();}
bool AeroLoginSettings::beginImagePreparation()
{if(!canEdit())return false;m_imagePreparing=m_busy=true;emit stateChanged();return true;}
void AeroLoginSettings::endImagePreparation()
{
    if(!m_imagePreparing)return;m_imagePreparing=false;m_busy=m_phase!=Phase::Idle;QPointer<AeroLoginSettings> guard(this);emit stateChanged();
    if(guard&&m_pendingRefresh&&!m_busy)beginRead(Phase::Refresh);
}
void AeroLoginSettings::beginRead(Phase phase)
{m_phase=phase;m_busy=true;m_pendingRefresh=false;QPointer<AeroLoginSettings> guard(this);emit stateChanged();if(guard)m_backend->readCurrent(++m_token);}
void AeroLoginSettings::refresh(){if(m_busy){m_pendingRefresh=true;return;}beginRead(Phase::Refresh);}
void AeroLoginSettings::revert(){if(!m_busy)beginRead(Phase::Revert);}
void AeroLoginSettings::apply()
{
    if(m_busy)return;const auto reason=applySupportText();if(!reason.isEmpty()){m_error=reason;QPointer<AeroLoginSettings> guard(this);emit stateChanged();if(guard)emit applyFinished(false);return;}
    if(!m_dirty){emit applyFinished(true);return;}m_recovery=false;m_expected=m_applied;m_requested=m_staged;m_error.clear();m_result.clear();beginRead(Phase::Preflight);
}
void AeroLoginSettings::recover()
{
    if(!canRecover())return;m_recovery=true;m_expected=m_observed;m_requested=m_staged;m_error.clear();m_result.clear();beginRead(Phase::RecoveryPreflight);
}
void AeroLoginSettings::onRead(quint64 token,bool success,const AeroLoginSnapshot &snapshot,const QString &error)
{
    if(token!=m_token||(m_phase!=Phase::Refresh&&m_phase!=Phase::Revert&&m_phase!=Phase::Preflight&&m_phase!=Phase::Readback&&m_phase!=Phase::RecoveryPreflight&&m_phase!=Phase::RecoveryReadback))return;
    const auto phase=m_phase;if(success){m_observed=snapshot;m_ready=true;}else m_ready=false;
    if(phase==Phase::Preflight||phase==Phase::RecoveryPreflight){
        QString validation;const bool matches=success&&AeroLoginTypes::sameConfiguration(m_expected,snapshot);
        const bool valid=phase==Phase::RecoveryPreflight?(snapshot.recoveryPending&&snapshot.canRecover&&snapshot.helperAvailable&&snapshot.trustedInstallation&&snapshot.sourceValid):AeroLoginTypes::requestValid(snapshot,m_requested,validation);
        if(matches&&valid){m_phase=Phase::Write;if(m_recovery)m_backend->recover(++m_token,m_expected);else m_backend->writeChanges(++m_token,m_expected,m_requested);return;}
        m_conflict=true;m_error=!success?error:!validation.isEmpty()?validation:tr("Aero files, defaults, installation or recovery state changed before authorization. Your draft is retained.");finish(true,false);return;
    }
    if(phase==Phase::Readback||phase==Phase::RecoveryReadback){
        QString verification;const bool matches=success&&(phase==Phase::RecoveryReadback?!snapshot.recoveryPending&&snapshot.sourceValid&&snapshot.trustedInstallation:AeroLoginTypes::matchesRequest(m_expected,m_requested,snapshot,verification));
        const bool complete=m_saved&&!m_savedRevision.isEmpty()&&snapshot.revision==m_savedRevision&&m_writeError.isEmpty()&&matches;
        if(phase==Phase::Readback&&!m_saved&&success&&!snapshot.recoveryPending&&AeroLoginTypes::sameConfiguration(m_expected,snapshot)){
            // The backend reports terminal outcomes only. An unchanged baseline
            // after denied/cancelled authorization retains a retryable draft.
            m_conflict=m_uncertain=false;m_result.clear();m_error=m_writeError.isEmpty()?tr("The appearance change was not saved. Your draft is retained."):m_writeError;finish(true,false);return;
        }
        if(complete){
            m_uncertain=false;m_error.clear();
            if(phase==Phase::RecoveryReadback&&m_dirty){m_conflict=true;m_result=tr("Interrupted files recovered. Your draft is retained; Revert to review the recovered baseline before applying.");}
            else{m_applied=m_staged=snapshot;m_dirty=m_conflict=false;m_result=phase==Phase::RecoveryReadback?tr("Interrupted appearance files recovered and read back. Login and lock-screen adoption remain unverified."):tr("Saved Aero login preferences and background files verified. No greeter was restarted; rendered behavior and future login/lock adoption remain unverified.");}
        }else{m_conflict=m_uncertain=true;m_result.clear();m_error=!m_writeError.isEmpty()?m_writeError:!success?error:!verification.isEmpty()?verification:tr("The authorized appearance result could not be verified. Recover if offered, then Revert before retrying.");}
        finish(true,complete);return;
    }
    if(success&&(phase==Phase::Revert||!AeroLoginTypes::dirty(m_staged)&&!m_conflict&&!m_uncertain)){
        const bool unchanged=m_ready&&AeroLoginTypes::sameConfiguration(m_applied,snapshot);m_applied=m_staged=snapshot;m_dirty=phase!=Phase::Revert&&hasImageReview();m_conflict=m_uncertain=false;m_error.clear();if(phase==Phase::Revert||!unchanged)m_result.clear();
        if(phase==Phase::Revert){QPointer<AeroLoginSettings> alive(this);for(const auto *key:{"aeroLoginPendingImage","aeroLoginPendingImageName","aeroLoginPendingImageReason","aeroLoginImageSource","aeroLoginImageError"}){setProperty(key,QVariant());if(!alive)return;}}
    }else if(success){if(!AeroLoginTypes::sameConfiguration(m_applied,snapshot)){m_conflict=true;m_error=tr("Aero files, policy or installation changed outside Control Panel. Your draft and selected picture are retained; Revert before applying.");}}
    else{if(m_dirty||m_uncertain||phase==Phase::Revert)m_conflict=true;m_error=error.isEmpty()?tr("Aero login preferences could not be read. Your draft and picture are retained."):error;}
    finish();
}
void AeroLoginSettings::onWritten(quint64 token,bool saved,const QString &error,const QByteArray &postRevision)
{if(token!=m_token||m_phase!=Phase::Write)return;m_saved=saved;m_savedRevision=postRevision;m_writeError=error;beginRead(m_recovery?Phase::RecoveryReadback:Phase::Readback);}
void AeroLoginSettings::finish(bool applying,bool success)
{
    m_phase=Phase::Idle;m_busy=m_imagePreparing;QPointer<AeroLoginSettings> guard(this);emit stateChanged();if(!guard)return;if(applying)emit applyFinished(success);
    if(guard&&m_pendingRefresh&&!m_busy)QTimer::singleShot(0,this,[this]{if(!m_busy&&m_pendingRefresh)beginRead(Phase::Refresh);});
}
