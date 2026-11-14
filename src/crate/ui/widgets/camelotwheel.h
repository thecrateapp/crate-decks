#pragma once

#include <QPixmap>
#include <QPointF>
#include <QString>
#include <QWidget>

#include "crate/ui/widgets/camelotwheelmodel.h"
#include "crate/ui/widgets/instrumentpainter.h"

namespace crate::widgets {

/// The Camelot wheel of design 14.3 with the grammar of GenreWheel (GU1
/// mockup library-compatibles, decks.js camelotWheel): two rings of 12 keys
/// (A inside, B outside) on GenreWheel's canvas, the master deck's key
/// filled in accent, its compatible keys with an accentHi stroke and active
/// edges with arrows, keys in the results at rest and the others at
/// opacityContext, and the master key in display type in the centre.
///
/// It paints only when its model changes (design 9): the rings and the
/// related-pair spokes are cached in a pixmap until it is resized, and the
/// whole wheel in another until the model, the subtitle or the hovered key
/// change, so that an expose or a repaint of its parent only copies it. A click on a
/// key emits keyClicked(): the Compatibles view (plan V3.PR2, F5) filters
/// its list by it.
class CamelotWheel : public QWidget {
    Q_OBJECT
  public:
    /// `pModel` must outlive the wheel.
    explicit CamelotWheel(CamelotWheelModel* pModel,
            const InstrumentStyle& style = InstrumentStyle::installed(),
            QWidget* pParent = nullptr);

    /// The MonoLabel under the key in the centre ("A minor").
    void setSubtitle(const QString& subtitle);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int width) const override {
        return width;
    }

    /// The centre of node `index` of the model's nodes(), in widget pixels.
    QPointF nodeCentre(int index) const;
    /// The index of the node at `position`, or -1.
    int nodeAt(QPointF position) const;

    /// The cache key of the pixmap with the static scale; it changes only
    /// when the scale is painted again.
    qint64 scaleCacheKey() const {
        return m_scale.cacheKey();
    }
    /// The cache key of the pixmap with the whole wheel; it changes only
    /// when what the wheel shows changes.
    qint64 frameCacheKey() const {
        return m_frame.cacheKey();
    }

  signals:
    void keyClicked(mixxx::track::io::key::ChromaticKey key);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    void resizeEvent(QResizeEvent* pEvent) override;
    void mouseMoveEvent(QMouseEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void mouseReleaseEvent(QMouseEvent* pEvent) override;
    void leaveEvent(QEvent* pEvent) override;

  private:
    /// Canvas pixels to widget pixels
    double scale() const;
    QPointF centre() const;
    void paintScale();
    void paintEdges(QPainter* pPainter) const;
    void paintNodeAndLabel(QPainter* pPainter, int index) const;
    void paintCentre(QPainter* pPainter) const;
    void setHovered(int index);
    /// Paints the wheel again on the next paint event
    void invalidate();

    CamelotWheelModel* const m_pModel;
    const InstrumentStyle m_style;
    QString m_subtitle;
    QPixmap m_scale;
    QPixmap m_frame;
    int m_hovered = -1;
    int m_pressed = -1;
};

} // namespace crate::widgets
