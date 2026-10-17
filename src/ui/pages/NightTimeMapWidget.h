#pragma once
#include <QPointF>
#include <QWidget>

// An offline equirectangular map. Painting and view changes never request a
// location. A selection is an explicit request to stage manual coordinates.
class NightTimeMapWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NightTimeMapWidget(QWidget *parent = nullptr);
    void setCoordinate(bool known,double latitude,double longitude);
    void setSelectionEnabled(bool enabled);
    double zoom() const { return m_zoom; }
    QSize sizeHint() const override;
public slots:
    void zoomIn();
    void zoomOut();
    void resetView();
signals:
    void coordinateSelected(double latitude,double longitude);
    void viewChanged();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    QRectF mapRect() const;
    QPointF coordinateAt(const QPointF &point) const;
    QPointF pointAt(double latitude,double longitude) const;
    void setZoom(double value);
    void clampCenter();
    bool m_known=false,m_selectionEnabled=false,m_pressed=false,m_panning=false;
    double m_latitude=0,m_longitude=0,m_zoom=1;
    QPointF m_center,m_pressPosition,m_pressCenter;
};
