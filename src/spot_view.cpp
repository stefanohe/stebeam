// SteBeam - laser beam profiler
// Copyright (C) 2026 Stefano's AI Lab
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this library.  If not, see <https://www.gnu.org/licenses/>.

#include "spot_view.h"
#include "colormap.h"   // shared jet LUT + dark-gray background (same for 2D/3D)
#include "i18n.h"       // painted text follows the main window's language switch
#include <QColor>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QRectF>
#include <algorithm>
#include <cmath>

SpotView::SpotView(QWidget* parent) : QWidget(parent)
{
	setMinimumSize(480, 384);
	setMouseTracking(false);
	setCursor(m_roiMode ? Qt::CrossCursor : Qt::ArrowCursor);
}

void SpotView::setFrame(const Frame& f)
{
	if (!f.valid())
		return;
	m_raw = f.px;                  // keep the raw frame for manual-range recoloring; the 2.6MB copy is <1ms at 88fps, not a bottleneck
	m_rawW = f.w; m_rawH = f.h;
	m_ox = f.originX; m_oy = f.originY;                 // axis support: frame geometry stored per frame (origin shifts after the inscribed-rotation crop)
	m_sw = f.sensorW > 0 ? f.sensorW : f.w;
	m_sh = f.sensorH > 0 ? f.sensorH : f.h;
	m_hasFrame = true;
	recolour();
}

void SpotView::setAutoRange(bool on)
{
	m_autoRange = on;
	if (m_hasFrame) recolour();
}

void SpotView::setRange(int lo, int hi)
{
	m_lo = lo;
	m_hi = std::max(hi, lo + 1);   // coincident bounds would divide by zero in scale
	if (m_hasFrame && !m_autoRange) recolour();
}

void SpotView::recolour()
{
	static QRgb lut[256];          // build the LUT once (rebuilding 256 trig calls per frame was pure waste)
	static bool lutDone = false;
	if (!lutDone) { buildJetLut(lut); lutDone = true; }

	int lo = m_lo, hi = m_hi;
	if (m_autoRange) {
		// Auto color scale: histogram the 12-bit ADU domain (1024 bins, 4 per bin) and take the
		// 0.5%~99.5% percentiles approximately, so single hot pixels can't blow up the range
		uint32_t hist[1024] = {0};
		size_t n = m_raw.size();
		for (size_t k = 0; k < n; ++k)
			++hist[std::min<uint16_t>(m_raw[k], 4095) >> 2];
		size_t loCnt = n / 200, hiCnt = n - n / 200;
		int acc = 0;
		for (int i = 0; i < 1024; ++i) {
			acc += hist[i];
			if (acc >= (int)loCnt) { lo = i << 2; break; }
		}
		acc = 0;
		for (int i = 1023; i >= 0; --i) {
			acc += hist[i];
			if (acc >= (int)(n - hiCnt)) { hi = (i + 1) << 2; break; }
		}
	}
	if (hi - lo < 256) hi = lo + 256;
	double scale = 255.0 / (hi - lo);

	// reuse the buffer when the size is unchanged, avoiding a 5.2MB allocation churn per frame
	if (m_img.width() != m_rawW || m_img.height() != m_rawH)
		m_img = QImage(m_rawW, m_rawH, QImage::Format_RGB32);
	for (int y = 0; y < m_rawH; ++y) {
		const uint16_t* src = &m_raw[(size_t)y * m_rawW];
		QRgb* dst = (QRgb*)m_img.scanLine(y);
		for (int x = 0; x < m_rawW; ++x) {
			int idx = (int)((src[x] - lo) * scale);
			if (idx < 0) idx = 0; else if (idx > 255) idx = 255;
			dst[x] = lut[idx];
		}
	}
	emit displayRangeChanged(lo, hi);
	update();
}

void SpotView::setCentroid(double cx, double cy, bool show)
{
	m_cx = cx; m_cy = cy; m_showCentroid = show;
	update();
}

void SpotView::setBeam84(double cx, double cy, double wx, double wy, double rotDeg, int shape, bool show)
{
	m_bcx = cx; m_bcy = cy; m_bwx = wx; m_bwy = wy; m_brot = rotDeg; m_bshape = shape; m_show84 = show;
	update();
}

void SpotView::setRoiDrawMode(bool on)
{
	m_roiMode = on;
	setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
}

void SpotView::clearRoi()
{
	m_roi = QRect();
	emit roiChanged(m_roi);
	update();
}

// Axis support: reserve left/bottom bands (tick values + unit), the image stays aspect-centered in
// the remaining plot area; left/right bands are equal width so the image sits dead center (the old
// 64/12 asymmetry pushed it right). Tick values are drawn inside the axis band, left of the tick
// marks ("don't let labels cover the intensity image"); band width scales with the widest tick label
// per unit (px~65 / um~78 / mm~56, so long um labels never collide with the vertical axis title).
// The labels' right edge is anchored to the image's left edge, so it tracks the tick marks when the
// sensor shrinks or rotation crops the frame.
int SpotView::axisMLFor(double widest, int dec) const
{
	QFont f = font(); f.setPointSizeF(8.0);
	QFontMetrics fm(f);
	// 32 = rotated title band (~19) + title-to-value gap (~6) + tick-to-value gap (~7)
	return 32 + fm.horizontalAdvance(QString::number(widest, 'f', dec));
}

int SpotView::axisML() const
{
	// widest tick label = the largest full-sensor coordinate (falls back to frame width when m_sw is 0)
	return axisMLFor((m_sw > 0 ? m_sw : m_rawW) * m_scale,
	                 m_unit == QLatin1String("px") ? 1 : 3);
}

// widget draw area (frame aspect centered) -> frame coordinate conversion
static QRect frameRectIn(const QSize& widget, const QSize& frame)
{
	if (frame.isEmpty()) return QRect();
	double s = std::min(widget.width() / (double)frame.width(),
	                    widget.height() / (double)frame.height());
	int w = (int)(frame.width() * s), h = (int)(frame.height() * s);
	return QRect((widget.width() - w) / 2, (widget.height() - h) / 2, w, h);
}

// aspect-centered image rect inside the plot area (widget minus axis bands), in widget coordinates
static QRect imageRectIn(const QRect& plot, const QSize& frame)
{
	QRect r = frameRectIn(plot.size(), frame);
	r.translate(plot.topLeft());
	return r;
}

QRect SpotView::viewToFrame(const QRect& r) const
{
	if (!m_hasFrame) return QRect();
	const int ml = axisML();
	QRect area = imageRectIn(rect().adjusted(ml, kAxisMT, -ml, -kAxisMB), m_img.size());
	if (area.isEmpty()) return QRect();
	auto tx = [&](const QPoint& p) {
		return QPoint((p.x() - area.x()) * m_img.width() / area.width(),
		              (p.y() - area.y()) * m_img.height() / area.height());
	};
	QPoint a = tx(r.topLeft()), b = tx(r.bottomRight());
	QRect out = QRect(a, b).normalized();
	return out.intersected(QRect(0, 0, m_img.width(), m_img.height()));
}

void SpotView::paintEvent(QPaintEvent*)
{
	QPainter p(this);
	p.fillRect(rect(), viewBgColor());   // dark-gray background (not pure black)
	if (!m_hasFrame) {
		p.setPen(Qt::gray);
		p.drawText(rect(), Qt::AlignCenter, L("等待帧…", "Waiting for frame…"));
		return;
	}
	const int ml = axisML();
	const QRect plot = rect().adjusted(ml, kAxisMT, -ml, -kAxisMB);
	QRect area = imageRectIn(plot, m_img.size());
	p.drawImage(area, m_img);

	// X/Y axis ticks -- values are full-sensor coordinates (bottom-left origin, y up; same frame of
	// reference as the position readouts / sensor-range line), converted by the global size unit
	// (px 1 decimal, um/mm 3, same as TrackView); light text on the dark background, 5 ticks.
	// Y tick values sit inside the axis band, left of the tick marks (area.left()-5..-1), their right
	// edge anchored to area.left()-7 so they track the marks when the sensor shrinks or rotation
	// crops; the x unit title follows the image horizontally.
	{
		const int dec = m_unit == QLatin1String("px") ? 1 : 3;
		QFont lf = font(); lf.setPointSizeF(8.0); p.setFont(lf);
		for (int i = 0; i <= 4; ++i) {
			double fx = area.left() + area.width() * i / 4.0;
			double fy = area.bottom() - area.height() * i / 4.0;
			double xv = (m_ox + m_rawW * i / 4.0) * m_scale;
			double yv = (m_sh - m_oy - m_rawH + m_rawH * i / 4.0) * m_scale;
			p.setPen(QColor(0x8a, 0x8a, 0x8a));
			p.drawLine(QPointF(fx, area.bottom() + 1), QPointF(fx, area.bottom() + 5));
			p.drawLine(QPointF(area.left() - 5, fy), QPointF(area.left() - 1, fy));
			p.setPen(QColor(0xb8, 0xb8, 0xb8));
			p.drawText(QRectF(fx - 45, area.bottom() + 6, 90, 14), Qt::AlignHCenter, QString::number(xv, 'f', dec));
			p.drawText(QRectF(area.left() - ml + 4, fy - 7, ml - 11, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(yv, 'f', dec));
		}
		p.setPen(QColor(0x9a, 0x9a, 0x9a));
		// x unit title anchored to the image's bottom edge: with dynamic axis bands the image can be
		// vertically centered and its bottom rises; anchoring to the widget bottom would stretch the
		// title-to-tick gap, so follow area.bottom() to keep the original gap and move with the ticks
		p.drawText(QRectF(area.left(), area.bottom() + 26, area.width(), 14), Qt::AlignHCenter, "x (" + m_unit + ")");
		p.save();
		// y unit title follows the image's left edge (moves in sync with the tick values): the title
		// band's right edge sits ~6px from the values' left edge; at full width tx=12 nearly coincides
		// with the old fixed position 9
		p.translate(area.left() - ml + 12, area.center().y());
		p.rotate(-90);
		p.drawText(QRectF(-100, -7, 200, 14), Qt::AlignHCenter, "y (" + m_unit + ")");
		p.restore();
	}

	p.setRenderHint(QPainter::Antialiasing, false);
	if (m_roi.isValid() && !m_roi.isEmpty()) {
		QRect r(m_roi.x() * area.width() / m_img.width() + area.x(),
		        m_roi.y() * area.height() / m_img.height() + area.y(),
		        m_roi.width() * area.width() / m_img.width(),
		        m_roi.height() * area.height() / m_img.height());
		QPen pen(Qt::white, 1, Qt::DashLine);
		p.setPen(pen);
		p.drawRect(r);
	}
	if (!m_drag.isNull()) {
		p.setPen(QPen(Qt::yellow, 1, Qt::SolidLine));
		p.drawRect(m_drag);
	}
	if (m_show84 && m_bwx > 0 && m_bwy > 0) {
		// 84% enclosed-energy contour: ellipse/rectangle centered on the centroid with d84X/d84Y as
		// width/height (the equal-tail percentile widths, visualized), rotated about the centroid by
		// m_brot to hug a tilted beam (shape selectable, rotates with the orientation)
		double sx = area.width() / (double)m_img.width();
		double sy = area.height() / (double)m_img.height();
		p.save();
		p.setRenderHint(QPainter::Antialiasing, true);   // the rotated contour goes through a transform matrix, so jaggies show more than axis-aligned
		p.translate(area.x() + m_bcx * sx, area.y() + m_bcy * sy);
		// Normalize the angle to [-45,45]: d84X/d84Y are marginal-distribution widths (bounding
		// extents in screen coordinates) while the principal axis is a line -- theta and theta±90 are
		// the same direction. Rotating by the raw angle when |theta|>45 would put the contour's long
		// side perpendicular to the beam's (X/Y swapped; exposed when leveling to vertical or on steep
		// beams; horizontal beams with |theta|<45 were never affected, which is why it stayed hidden).
		// After normalization the long side always hugs the major axis.
		double rot = m_brot;
		while (rot > 45.0) rot -= 90.0;
		while (rot < -45.0) rot += 90.0;
		p.rotate(rot);
		QRectF r(-m_bwx * sx / 2, -m_bwy * sy / 2, m_bwx * sx, m_bwy * sy);
		p.setPen(QPen(QColor(0, 255, 255), 1));
		p.setBrush(Qt::NoBrush);
		if (m_bshape == 1)
			p.drawRect(r);
		else
			p.drawEllipse(r);
		p.restore();
	}
	if (m_showCentroid) {
		int cx = area.x() + (int)(m_cx * area.width() / m_img.width());
		int cy = area.y() + (int)(m_cy * area.height() / m_img.height());
		p.setPen(QPen(Qt::red, 1));
		p.drawLine(cx - 10, cy, cx + 10, cy);
		p.drawLine(cx, cy - 10, cx, cy + 10);
	}
}

void SpotView::mousePressEvent(QMouseEvent* e)
{
	if (m_roiMode && e->button() == Qt::LeftButton) {
		m_dragStart = e->pos();
		m_drag = QRect(m_dragStart, m_dragStart);
		update();
	}
}

void SpotView::mouseMoveEvent(QMouseEvent* e)
{
	if (m_roiMode && (e->buttons() & Qt::LeftButton)) {
		m_drag = QRect(m_dragStart, e->pos()).normalized();
		update();
	}
}

void SpotView::mouseReleaseEvent(QMouseEvent* e)
{
	if (m_roiMode && e->button() == Qt::LeftButton) {
		m_roi = viewToFrame(m_drag);
		m_drag = QRect();
		m_roiMode = false;
		setCursor(Qt::ArrowCursor);
		emit roiChanged(m_roi);
		update();
	}
}
