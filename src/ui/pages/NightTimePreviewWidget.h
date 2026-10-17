#pragma once
#include "settings/NightTimeForecast.h"
#include <QWidget>

// A pure, schematic cycle display. Its data is a forecast, never a provider or
// an observation that the daemon adopted the staged preferences.
class NightTimePreviewWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NightTimePreviewWidget(QWidget *parent = nullptr);
    void setForecast(const NightTimeForecast::Result &result,const QDateTime &reference,const QString &description);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
private:
    NightTimeForecast::Result m_result;
    QDateTime m_reference;
    QString m_description;
};
