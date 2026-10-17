#include "NightTimePreviewWidget.h"
#include <QPainter>
#include <QPainterPath>
#include <QTimeZone>
#include <algorithm>

NightTimePreviewWidget::NightTimePreviewWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("nightTimePreview")); setMinimumHeight(112); setAccessibleName(tr("Calculated day and night cycle"));
}
QSize NightTimePreviewWidget::sizeHint() const { return QSize(680,124); }
void NightTimePreviewWidget::setForecast(const NightTimeForecast::Result &result,const QDateTime &reference,const QString &description)
{ m_result=result; m_reference=reference; m_description=description; setAccessibleDescription(description); update(); }
void NightTimePreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.fillRect(rect(),Qt::white);
    if (!m_result.valid || !m_reference.isValid()) { painter.setPen(QColor("#555555")); painter.drawText(rect().adjusted(12,8,-12,-8),Qt::AlignCenter|Qt::TextWordWrap,tr("A calculated cycle will appear when the selected inputs are known.")); return; }
    const auto midnight=QDateTime(m_reference.date(),QTime(0,0),m_reference.timeZone()); const auto end=midnight.addDays(1);
    const double seconds=midnight.secsTo(end); if (seconds<=0) return;
    const QRectF band(14,height()-48,width()-28,22); auto x=[&](const QDateTime &time) { return band.left()+midnight.msecsTo(time)/(seconds*1000)*band.width(); };
    painter.fillRect(band,QColor("#D6E2F0"));
    auto fillInterval=[&](const QDateTime &start,const QDateTime &finish,const QColor &color) {
        if (!start.isValid() || !finish.isValid() || start>=finish) return;
        const double left=qBound(band.left(),x(start),band.right()),right=qBound(band.left(),x(finish),band.right());
        if (right>left) painter.fillRect(QRectF(left,band.top(),right-left,band.height()),color);
    };
    for (const auto &cycle:m_result.cycles) {
        fillInterval(cycle.morning.end,cycle.evening.start,QColor("#FBE3A0"));
        fillInterval(cycle.morning.start,cycle.morning.end,QColor("#EAC99A")); fillInterval(cycle.evening.start,cycle.evening.end,QColor("#EAC99A"));
        if (cycle.noon.date()!=m_reference.date()) continue;
        const double left=qBound(band.left(),x(cycle.morning.end),band.right()),right=qBound(band.left(),x(cycle.evening.start),band.right());
        if (right>left) {
            QPainterPath path; path.moveTo(left,band.top()-7); path.cubicTo(left+(right-left)/4,5,right-(right-left)/4,5,right,band.top()-7);
            painter.setPen(QPen(QColor("#BB811C"),2)); painter.drawPath(path);
            if (m_reference>=cycle.morning.end && m_reference<=cycle.evening.start) {
                const double span=cycle.morning.end.msecsTo(cycle.evening.start); const double progress=span>0?cycle.morning.end.msecsTo(m_reference)/span:0;
                const auto point=path.pointAtPercent(qBound(0.0,progress,1.0)); painter.setBrush(QColor("#E8AB36")); painter.setPen(Qt::NoPen); painter.drawEllipse(point,5,5);
            }
        }
    }
    painter.setBrush(Qt::NoBrush); painter.setPen(QColor("#9AA9B9")); painter.drawRect(band);
    if (m_result.fallback!=NightTimeForecast::Fallback::None) { painter.setPen(QPen(QColor("#A36B22"),1,Qt::DashLine)); painter.drawRect(band.adjusted(-3,-3,3,3)); }
    painter.setPen(QColor("#222222")); const QString startText=midnight.time().toString(QStringLiteral("HH:mm"));
    painter.drawText(QRectF(band.left(),band.bottom()+5,100,18),Qt::AlignLeft,startText);
    painter.drawText(QRectF(band.center().x()-50,band.bottom()+5,100,18),Qt::AlignCenter,tr("Local day"));
    painter.drawText(QRectF(band.right()-100,band.bottom()+5,100,18),Qt::AlignRight,tr("Next midnight"));
}
