#include "DefaultAppsSettings.h"
#include "platform/apps/DefaultAppsBackend.h"
#include <QCoreApplication>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>

const DefaultApplication *DefaultAppsSnapshot::application(const QString &id) const
{
    for (const auto &app : applications) if (app.id == id) return &app;
    return nullptr;
}
const QList<DefaultAppRoleInfo> &DefaultAppsTypes::roles()
{
    // These MIME lists and recommended IDs are the audited KCM's lists, not
    // all types an application happens to advertise. Unsupported types are skipped.
    static const QList<DefaultAppRoleInfo> values{
        {DefaultAppRole::Browser, QStringLiteral("browser"), QStringLiteral("Web browser"), QStringLiteral("x-scheme-handler/http"), QStringLiteral(""), QStringLiteral("org.kde.falkon.desktop"), {QStringLiteral("x-scheme-handler/http"), QStringLiteral("x-scheme-handler/https")}},
        {DefaultAppRole::Email, QStringLiteral("email"), QStringLiteral("E-mail client"), QStringLiteral("x-scheme-handler/mailto"), QStringLiteral("Email"), QStringLiteral("org.kde.kmail2.desktop"), {QStringLiteral("x-scheme-handler/mailto")}},
        {DefaultAppRole::FileManager, QStringLiteral("filemanager"), QStringLiteral("File manager"), QStringLiteral("inode/directory"), QStringLiteral("FileManager"), QStringLiteral("org.kde.dolphin.desktop"), {QStringLiteral("inode/directory")}},
        {DefaultAppRole::Terminal, QStringLiteral("terminal"), QStringLiteral("Terminal emulator"), QStringLiteral(""), QStringLiteral("TerminalEmulator"), QStringLiteral("org.kde.konsole.desktop"), {}},
        {DefaultAppRole::Map, QStringLiteral("geo"), QStringLiteral("Map"), QStringLiteral("x-scheme-handler/geo"), QStringLiteral(""), QStringLiteral("marble_geo.desktop"), {QStringLiteral("x-scheme-handler/geo")}},
        {DefaultAppRole::Dialer, QStringLiteral("tel"), QStringLiteral("Dialer"), QStringLiteral("x-scheme-handler/tel"), QStringLiteral("Dialer"), QStringLiteral("org.kde.kdeconnect.handler.desktop"), {QStringLiteral("x-scheme-handler/tel")}},
        {DefaultAppRole::TextEditor, QStringLiteral("texteditor"), QStringLiteral("Text editor"), QStringLiteral("text/plain"), QStringLiteral("TextEditor"), QStringLiteral("org.kde.kate.desktop"), {QStringLiteral("text/plain"), QStringLiteral("text/x-cmake"), QStringLiteral("text/markdown"), QStringLiteral("application/x-docbook+xml"), QStringLiteral("application/json"), QStringLiteral("application/x-yaml")}},
        {DefaultAppRole::ImageViewer, QStringLiteral("imageviewer"), QStringLiteral("Image viewer"), QStringLiteral("image/png"), QStringLiteral("Viewer"), QStringLiteral("org.kde.gwenview.desktop"), {QStringLiteral("image/png"), QStringLiteral("image/jpeg"), QStringLiteral("image/webp"), QStringLiteral("image/avif"), QStringLiteral("image/heif"), QStringLiteral("image/bmp"), QStringLiteral("image/x-icns")}},
        {DefaultAppRole::MusicPlayer, QStringLiteral("musicplayer"), QStringLiteral("Music player"), QStringLiteral("audio/mpeg"), QStringLiteral("Player"), QStringLiteral("org.kde.elisa.desktop"), {QStringLiteral("audio/aac"), QStringLiteral("audio/mp4"), QStringLiteral("audio/mpeg"), QStringLiteral("audio/mpegurl"), QStringLiteral("audio/ogg"), QStringLiteral("audio/vnd.rn-realaudio"), QStringLiteral("audio/vorbis"), QStringLiteral("audio/x-flac"), QStringLiteral("audio/x-mp3"), QStringLiteral("audio/x-mpegurl"), QStringLiteral("audio/x-ms-wma"), QStringLiteral("audio/x-musepack"), QStringLiteral("audio/x-oggflac"), QStringLiteral("audio/x-pn-realaudio"), QStringLiteral("audio/x-scpls"), QStringLiteral("audio/x-speex"), QStringLiteral("audio/x-vorbis"), QStringLiteral("audio/x-vorbis+ogg"), QStringLiteral("audio/x-wav")}},
        {DefaultAppRole::VideoPlayer, QStringLiteral("videoplayer"), QStringLiteral("Video player"), QStringLiteral("video/mp4"), QStringLiteral("Video"), QStringLiteral("org.kde.haruna.desktop"), {QStringLiteral("video/3gp"), QStringLiteral("video/3gpp"), QStringLiteral("video/3gpp2"), QStringLiteral("video/avi"), QStringLiteral("video/divx"), QStringLiteral("video/dv"), QStringLiteral("video/fli"), QStringLiteral("video/flv"), QStringLiteral("video/mp2t"), QStringLiteral("video/mp4"), QStringLiteral("video/mp4v-es"), QStringLiteral("video/mpeg"), QStringLiteral("video/msvideo"), QStringLiteral("video/ogg"), QStringLiteral("video/quicktime"), QStringLiteral("video/vnd.divx"), QStringLiteral("video/vnd.mpegurl"), QStringLiteral("video/vnd.rn-realvideo"), QStringLiteral("video/webm"), QStringLiteral("video/x-avi"), QStringLiteral("video/x-flv"), QStringLiteral("video/x-m4v"), QStringLiteral("video/x-matroska"), QStringLiteral("video/x-mpeg2"), QStringLiteral("video/x-ms-asf"), QStringLiteral("video/x-msvideo"), QStringLiteral("video/x-ms-wmv"), QStringLiteral("video/x-ms-wmx"), QStringLiteral("video/x-ogm"), QStringLiteral("video/x-ogm+ogg"), QStringLiteral("video/x-theora"), QStringLiteral("video/x-theora+ogg"), QStringLiteral("application/x-matroska")}},
        {DefaultAppRole::PdfViewer, QStringLiteral("pdfviewer"), QStringLiteral("PDF viewer"), QStringLiteral("application/pdf"), QStringLiteral(""), QStringLiteral("okularApplication_pdf.desktop"), {QStringLiteral("application/pdf")}},
        {DefaultAppRole::ArchiveManager, QStringLiteral("archivemanager"), QStringLiteral("Archive manager"), QStringLiteral("application/zip"), QStringLiteral("Archiving"), QStringLiteral("org.kde.ark.desktop"), {QStringLiteral("application/x-tar"), QStringLiteral("application/x-compressed-tar"), QStringLiteral("application/x-bzip-compressed-tar"), QStringLiteral("application/x-tarz"), QStringLiteral("application/x-xz-compressed-tar"), QStringLiteral("application/x-lzma-compressed-tar"), QStringLiteral("application/x-lzip-compressed-tar"), QStringLiteral("application/x-tzo"), QStringLiteral("application/x-lrzip-compressed-tar"), QStringLiteral("application/x-lz4-compressed-tar"), QStringLiteral("application/x-zstd-compressed-tar"), QStringLiteral("application/x-cd-image"), QStringLiteral("application/x-bcpio"), QStringLiteral("application/x-cpio"), QStringLiteral("application/x-cpio-compressed"), QStringLiteral("application/x-sv4cpio"), QStringLiteral("application/x-sv4crc"), QStringLiteral("application/x-source-rpm"), QStringLiteral("application/vnd.ms-cab-compressed"), QStringLiteral("application/x-xar"), QStringLiteral("application/x-iso9660-appimage"), QStringLiteral("application/x-archive"), QStringLiteral("application/vnd.rar"), QStringLiteral("application/x-rar"), QStringLiteral("application/x-7z-compressed"), QStringLiteral("application/zip"), QStringLiteral("application/x-compress"), QStringLiteral("application/gzip"), QStringLiteral("application/x-bzip"), QStringLiteral("application/x-lzma"), QStringLiteral("application/x-xz"), QStringLiteral("application/zstd"), QStringLiteral("application/x-lha")}},
    };
    return values;
}
const DefaultAppRoleInfo *DefaultAppsTypes::role(DefaultAppRole value)
{
    for (const auto &r : roles()) if (r.role == value) return &r;
    return nullptr;
}
bool DefaultAppsTypes::validMime(const QString &mime)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9!#$&^_.+\\-]+/[A-Za-z0-9!#$&^_.+\\-]+$"));
    if (!pattern.match(mime).hasMatch()) return false;
    return mime.startsWith(QLatin1String("x-scheme-handler/")) || QMimeDatabase().mimeTypeForName(mime).isValid();
}
QString DefaultAppsTypes::canonicalMime(const QString &mime)
{
    if (mime.startsWith(QLatin1String("x-scheme-handler/"))) return mime;
    return QMimeDatabase().mimeTypeForName(mime).name();
}
bool DefaultAppsTypes::supports(const DefaultApplication &app, const QString &mime)
{
    if (app.mimeTypes.contains(mime)) return true;
    const auto type = QMimeDatabase().mimeTypeForName(mime);
    for (const auto &supported : app.mimeTypes)
        if (type.isValid() && type.inherits(supported)) return true;
    return false;
}
bool DefaultAppsTypes::eligible(const DefaultApplication &app, const DefaultAppRoleInfo &r)
{
    return app.showInDesktop && !app.exec.isEmpty() && (r.category.isEmpty() || app.categories.contains(r.category))
        && (r.anchorMime.isEmpty() || supports(app, r.anchorMime));
}
QString DefaultAppsTypes::selected(const DefaultAppsPreferences &p, const DefaultAppRoleInfo &r)
{ return r.role == DefaultAppRole::Terminal ? p.terminalService : p.mimeDefaults.value(canonicalMime(r.anchorMime)); }

DefaultAppsSettings *DefaultAppsSettings::instance()
{
    static QPointer<DefaultAppsSettings> session;
    if (!session) session = new DefaultAppsSettings(new KdeDefaultAppsBackend, QCoreApplication::instance());
    return session;
}
DefaultAppsSettings::DefaultAppsSettings(DefaultAppsBackend *backend, QObject *parent)
    : SettingsSession(parent), m_backend(backend)
{
    Q_ASSERT(backend); backend->setParent(this);
    connect(backend, &DefaultAppsBackend::readFinished, this, &DefaultAppsSettings::onRead);
    connect(backend, &DefaultAppsBackend::writeFinished, this, &DefaultAppsSettings::onWritten);
    connect(backend, &DefaultAppsBackend::externalChange, this, &DefaultAppsSettings::refresh);
    connect(backend,&DefaultAppsBackend::operationPending,this,[this](const QString &message) {
        if (!m_busy) return;
        m_operationPending=true; m_result=message; emit stateChanged();
    });
    m_busy = true;
    QTimer::singleShot(0, this, &DefaultAppsSettings::read);
}
void DefaultAppsSettings::changed()
{
    m_dirty = m_staged != m_applied.preferences;
    m_error.clear(); m_result.clear();
    emit stateChanged();
}
void DefaultAppsSettings::selectRole(DefaultAppRole value, const QString &id)
{
    const auto *r = DefaultAppsTypes::role(value);
    const auto *app = m_applied.application(id);
    if (m_busy || !canApply() || !r || !app || m_applied.unavailableRoles.contains(value) || !DefaultAppsTypes::eligible(*app, *r)) return;
    if (value == DefaultAppRole::Terminal) m_staged.terminalService = id;
    else for (const auto &mime : r->mimeTypes)
        if (m_applied.canChangeMime(DefaultAppsTypes::canonicalMime(mime)) && DefaultAppsTypes::supports(*app, mime))
            m_staged.mimeDefaults[DefaultAppsTypes::canonicalMime(mime)] = id;
    changed();
}
void DefaultAppsSettings::selectMime(const QString &mime, const QString &id)
{
    const auto *app = m_applied.application(id);
    if (m_busy || !canApply() || !app || !m_applied.mimeTypes.contains(mime)
        || !m_applied.canChangeMime(mime) || !DefaultAppsTypes::validMime(mime) || !DefaultAppsTypes::supports(*app, mime)) return;
    m_staged.mimeDefaults[mime] = id;
    changed();
}
void DefaultAppsSettings::restoreDefaults()
{
    if (m_busy || !canApply()) return;
    int available = 0;
    QPointer<DefaultAppsSettings> guard(this);
    for (const auto &r : DefaultAppsTypes::roles()) {
        const auto *app = m_applied.application(r.recommended);
        if (app && !m_applied.unavailableRoles.contains(r.role) && DefaultAppsTypes::eligible(*app, r)) { selectRole(r.role, app->id); if (!guard) return; ++available; }
    }
    m_result = tr("Staged KDE's recommended applications for %1 roles with an installed compatible application. Other roles and file associations are unchanged.").arg(available);
    emit stateChanged();
}
void DefaultAppsSettings::read()
{
    m_pendingRefresh = false; m_busy = true; m_writing = false;
    const auto token=++m_token; QPointer<DefaultAppsSettings> guard(this);
    emit stateChanged(); if (guard) m_backend->readCurrent(token);
}
void DefaultAppsSettings::refresh()
{
    if (m_busy) { m_pendingRefresh = true; return; }
    read();
}
void DefaultAppsSettings::onRead(quint64 token, bool ok, const DefaultAppsSnapshot &state, const QString &error)
{
    if (token != m_token || !m_busy || m_writing) return;
    if (m_operationPending) { m_result.clear(); m_operationPending=false; }
    const bool changedOutside = m_ready && ok && state.revision != m_applied.revision;
    const bool wasDirty = m_dirty;
    if (ok) {
        m_applied = state; m_ready = true;
        if (!wasDirty || m_reverting) m_staged = state.preferences;
        if (m_reverting) { m_conflict=false; m_result.clear(); }
        m_dirty = m_staged != state.preferences;
        if (changedOutside && wasDirty && !m_reverting) m_conflict = true;
        if (!m_conflict) m_error.clear();
    } else { m_ready = false; m_error = error; if (wasDirty) m_conflict = true; }
    m_reverting=false; m_busy = false; QPointer<DefaultAppsSettings> guard(this); emit stateChanged();
    if (!guard) return;
    if (m_pendingRefresh) QTimer::singleShot(0, this, &DefaultAppsSettings::refresh);
}
void DefaultAppsSettings::apply()
{
    if (m_busy) return;
    if (!canApply()) {
        m_error = m_conflict ? tr("Default programs changed outside Control Panel. Revert to reload before applying.")
            : !m_ready ? tr("Default programs could not be read. Revert to retry.") : m_applied.reason;
        QPointer<DefaultAppsSettings> guard(this); emit stateChanged(); if (guard) emit applyFinished(false); return;
    }
    if (!m_dirty) { emit applyFinished(true); return; }
    m_requested = m_staged; m_busy = true; m_writing = true;
    m_error.clear(); m_result.clear(); const auto token=++m_token; QPointer<DefaultAppsSettings> guard(this);
    emit stateChanged(); if (guard) m_backend->writeChanges(token, m_applied, m_requested);
}
void DefaultAppsSettings::onWritten(quint64 token, DefaultAppsOutcome outcome, const DefaultAppsSnapshot &state, const QString &error)
{
    if (token != m_token || !m_writing) return;
    m_operationPending=false;
    m_outcome = outcome; m_busy = false; m_writing = false;
    const bool verified = outcome == DefaultAppsOutcome::Success && state.preferences == m_requested && !state.revision.isEmpty();
    if (!state.revision.isEmpty()) { m_applied = state; m_ready = true; }
    else m_ready = false;
    m_dirty = m_staged != m_applied.preferences;
    if (verified) {
        m_conflict = false; m_error.clear();
        m_result = tr("Default programs saved and verified in KDE's association files. Running applications and the service cache may adopt these changes later; their adoption has not been verified.");
    } else {
        m_conflict = true;
        m_result.clear();
        m_error = error.isEmpty() ? tr("The saved result could not be verified. Your selection is preserved. Revert before applying again.") : error;
    }
    QPointer<DefaultAppsSettings> guard(this); emit stateChanged(); if (!guard) return;
    emit applyFinished(verified); if (!guard) return;
    if (m_pendingRefresh) QTimer::singleShot(0, this, &DefaultAppsSettings::refresh);
}
void DefaultAppsSettings::revert()
{
    if (m_busy) return;
    // Discard only after a successful asynchronous reload; a failed read keeps
    // the draft available for review and the navigation guard on this page.
    m_reverting=true; read();
}
