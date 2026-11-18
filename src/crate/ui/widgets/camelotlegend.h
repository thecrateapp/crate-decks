#pragma once

#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include "crate/ui/widgets/instrumentpainter.h"

namespace crate::widgets {

/// The Camelot wheel's legend (GU1 mockup library-compatibles, GenreWheel's
/// .genre-wheel-relationship-key): "KEY" in accentHi, then a sample and a
/// name for the master deck's key, the compatible edges, the relative
/// spokes, the keys in the results and the keys without, wrapping like a
/// row of flex items, and a note under them. 9 px mono in textMuted; the
/// samples are painted as the wheel paints them.
class CamelotLegend : public QWidget {
    Q_OBJECT
  public:
    enum class Sample {
        None,
        Master,
        Compatible,
        Relative,
        InResults,
        NoResults,
    };
    struct Item {
        Sample sample = Sample::None;
        QString text;
    };

    explicit CamelotLegend(const InstrumentStyle& style = InstrumentStyle::installed(),
            QWidget* pParent = nullptr);

    const QVector<Item>& items() const {
        return m_items;
    }
    QString note() const {
        return m_note;
    }

    /// Where each item goes at `width`, in widget pixels; the title is the
    /// first, the note the last.
    QVector<QRectF> layout(int width) const;

    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    double itemWidth(const Item& item) const;
    QRectF noteRect(double top, int width) const;
    void paintSample(QPainter* pPainter, Sample sample, const QRectF& box) const;

    const InstrumentStyle m_style;
    QString m_title;
    QVector<Item> m_items;
    QString m_note;
};

} // namespace crate::widgets
