#include "NightTimeMapWidget.h"
#include "Win7Ui.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QWheelEvent>
#include <QVariant>
#include <cmath>

NightTimeMapWidget::NightTimeMapWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("nightTimeMap")); setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Offline manual location map"));
    setAccessibleDescription(tr("Click or use arrow keys to stage manual coordinates. Shift moves ten degrees; Alt moves one tenth of a degree. Plus and minus zoom. Home resets the view. Drag to pan."));
    setMinimumSize(360,190);
}
QSize NightTimeMapWidget::sizeHint() const { return QSize(640,320); }
void NightTimeMapWidget::setCoordinate(bool known,double latitude,double longitude)
{
    m_known=known && std::isfinite(latitude) && std::isfinite(longitude) && latitude>=-90 && latitude<=90 && longitude>=-180 && longitude<=180;
    m_latitude=latitude; m_longitude=longitude;
    setProperty("coordinateKnown",m_known); setProperty("latitude",m_known?QVariant(latitude):QVariant()); setProperty("longitude",m_known?QVariant(longitude):QVariant()); update();
}
void NightTimeMapWidget::setSelectionEnabled(bool enabled) { m_selectionEnabled=enabled; setCursor(enabled?Qt::CrossCursor:Qt::ArrowCursor); update(); }
QRectF NightTimeMapWidget::mapRect() const
{
    const QRectF available=QRectF(rect()).adjusted(5,5,-5,-5); const double width=qMin(available.width(),available.height()*2);
    return QRectF(available.center().x()-width/2,available.center().y()-width/4,width,width/2);
}
QPointF NightTimeMapWidget::coordinateAt(const QPointF &point) const
{
    const auto area=mapRect(); return QPointF(qBound(-180.0,m_center.x()+(point.x()-area.center().x())*360/(area.width()*m_zoom),180.0),
        qBound(-90.0,m_center.y()-(point.y()-area.center().y())*180/(area.height()*m_zoom),90.0));
}
QPointF NightTimeMapWidget::pointAt(double latitude,double longitude) const
{
    const auto area=mapRect(); return QPointF(area.center().x()+(longitude-m_center.x())*area.width()*m_zoom/360,
        area.center().y()-(latitude-m_center.y())*area.height()*m_zoom/180);
}
void NightTimeMapWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); const auto area=mapRect(); painter.fillRect(rect(),Qt::white); painter.save(); painter.setClipRect(area);
    painter.fillRect(area,QColor("#DDEBF4")); const QPixmap map(QStringLiteral(":/ControlPanel/nighttime/worldmap.png"));
    const auto topLeft=pointAt(90,-180); const QRectF target(topLeft,QSizeF(area.width()*m_zoom,area.height()*m_zoom));
    if (!map.isNull()) painter.drawPixmap(target,map,QRectF(map.rect()));
    else { painter.setPen(QColor("#555555")); painter.drawText(area,Qt::AlignCenter,tr("Offline map image unavailable")); }
    painter.setPen(QPen(QColor(130,158,178,110),1));
    for (int longitude=-180;longitude<=180;longitude+=30) painter.drawLine(pointAt(-90,longitude),pointAt(90,longitude));
    for (int latitude=-90;latitude<=90;latitude+=30) painter.drawLine(pointAt(latitude,-180),pointAt(latitude,180));
    if (m_known) {
        const auto point=pointAt(m_latitude,m_longitude); painter.setPen(QPen(Qt::white,4)); painter.setBrush(QColor("#A32B1F")); painter.drawEllipse(point,6,6);
        painter.setPen(QPen(QColor("#8B1C12"),1)); painter.drawEllipse(point,6,6);
    }
    painter.restore(); painter.setPen(QColor("#AAB5BF")); painter.setBrush(Qt::NoBrush); painter.drawRect(area); Win7::paintFocus(this,painter);
}
void NightTimeMapWidget::clampCenter()
{
    m_center.setX(qBound(-180.0+180/m_zoom,m_center.x(),180.0-180/m_zoom));
    m_center.setY(qBound(-90.0+90/m_zoom,m_center.y(),90.0-90/m_zoom));
}
void NightTimeMapWidget::setZoom(double value)
{
    value=qBound(1.0,value,5.0); if (qFuzzyCompare(m_zoom,value)) return;
    if (m_zoom==1 && m_known && value>1) m_center=QPointF(m_longitude,m_latitude);
    m_zoom=value; clampCenter(); update(); emit viewChanged();
}
void NightTimeMapWidget::zoomIn() { setZoom(m_zoom*1.25); }
void NightTimeMapWidget::zoomOut() { setZoom(m_zoom/1.25); }
void NightTimeMapWidget::resetView() { m_zoom=1; m_center=QPointF(); update(); emit viewChanged(); }
void NightTimeMapWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button()==Qt::LeftButton && mapRect().contains(event->position())) { setFocus(); m_pressed=true; m_panning=false; m_pressPosition=event->position(); m_pressCenter=m_center; event->accept(); }
    else QWidget::mousePressEvent(event);
}
void NightTimeMapWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_pressed) { QWidget::mouseMoveEvent(event); return; }
    const auto delta=event->position()-m_pressPosition; if (delta.manhattanLength()>QApplication::startDragDistance()) m_panning=true;
    if (m_panning) { const auto area=mapRect(); m_center=m_pressCenter+QPointF(-delta.x()*360/(area.width()*m_zoom),delta.y()*180/(area.height()*m_zoom)); clampCenter(); update(); }
    event->accept();
}
void NightTimeMapWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button()==Qt::LeftButton && m_pressed) {
        const bool select=!m_panning && m_selectionEnabled && mapRect().contains(event->position()); m_pressed=m_panning=false;
        if (select) { const auto coordinate=coordinateAt(event->position()); emit coordinateSelected(coordinate.y(),coordinate.x()); } event->accept();
    } else QWidget::mouseReleaseEvent(event);
}
void NightTimeMapWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key()==Qt::Key_Plus || event->key()==Qt::Key_Equal) { zoomIn(); event->accept(); return; }
    if (event->key()==Qt::Key_Minus) { zoomOut(); event->accept(); return; }
    if (event->key()==Qt::Key_Home) { resetView(); event->accept(); return; }
    double latitude=m_known?m_latitude:m_center.y(),longitude=m_known?m_longitude:m_center.x();
    const double step=event->modifiers().testFlag(Qt::ShiftModifier)?10:event->modifiers().testFlag(Qt::AltModifier)?0.1:1;
    bool choose=true;
    switch (event->key()) {
    case Qt::Key_Left: longitude-=step; break; case Qt::Key_Right: longitude+=step; break;
    case Qt::Key_Up: latitude+=step; break; case Qt::Key_Down: latitude-=step; break;
    case Qt::Key_Enter: case Qt::Key_Return: break; default: choose=false; break;
    }
    if (choose && m_selectionEnabled) { emit coordinateSelected(qBound(-90.0,latitude,90.0),qBound(-180.0,longitude,180.0)); event->accept(); }
    else QWidget::keyPressEvent(event);
}
void NightTimeMapWidget::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)) { if (event->angleDelta().y()>0) zoomIn(); else if (event->angleDelta().y()<0) zoomOut(); event->accept(); }
    else { event->ignore(); }
}
