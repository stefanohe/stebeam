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

// 2D pseudocolor view: frame -> jet-colormap QImage, overlaid with the ROI box and centroid crosshair;
// drag-to-draw ROI selection supported.
#pragma once
#include "frame.h"
#include <QImage>
#include <QRect>
#include <QWidget>
#include <vector>

class SpotView : public QWidget
{
	Q_OBJECT
public:
	explicit SpotView(QWidget* parent = nullptr);

	void setFrame(const Frame& f);
	void setCentroid(double cx, double cy, bool show);
	void setRoiDrawMode(bool on);            // true = left-drag draws the ROI
	QRect roi() const { return m_roi; }
	void clearRoi();
	void setAutoRange(bool on);              // false = color with the manual range from setRange
	void setRange(int lo, int hi);           // manual pseudocolor bounds (12-bit ADU scale, default [-100,4096])
	// 84% enclosed-energy contour (frame coordinates / px widths); shape 0=ellipse 1=rectangle;
	// rotDeg = rotation about the centroid (positive in image coordinates). Pass 0 when beam rotation
	// is on (major axis already vertical); pass the orientation when off so the contour hugs the tilted beam.
	void setBeam84(double cx, double cy, double wx, double wy, double rotDeg, int shape, bool show);
	// Axis ticks follow the global size unit (px 1 decimal, um/mm 3 decimals, same rule as TrackView);
	// the value domain = the full-sensor span the frame actually covers (bottom-left origin, y up --
	// the same source as the position readouts / sensor-range line).
	void setUnit(double scale, const QString& unit) { m_scale = scale; m_unit = unit; update(); }
	// Axis-band sizes exposed so the main window can size its initial height (no vertical slack around the image = the bottom letterbox is only the axis annotation band)
	static constexpr int kAxisMT = 8, kAxisMB = 40;   // top/bottom axis band heights (bottom holds tick marks + values + unit title)
	int axisMLFor(double widest, int dec) const;      // left/right axis band width (scales with the widest tick label)

signals:
	void roiChanged(const QRect& roi);
	void displayRangeChanged(int lo, int hi);  // reports the active range (auto mode lets the sliders follow)

protected:
	void paintEvent(QPaintEvent*) override;
	void mousePressEvent(QMouseEvent*) override;
	void mouseMoveEvent(QMouseEvent*) override;
	void mouseReleaseEvent(QMouseEvent*) override;

private:
	void recolour();              // recolor from m_raw at the current range (slider tweaks respond instantly even on a frozen frame)
	int axisML() const;           // left/right axis band width (scales with the widest tick label; symmetric = image stays centered)
	QImage m_img;                 // colored display image
	std::vector<uint16_t> m_raw;  // raw frame copy -- the data source for manual-range recoloring
	int m_rawW = 0, m_rawH = 0;
	bool m_autoRange = false;                // manual fixed range by default (2^12 span)
	int m_lo = -100, m_hi = 4096;
	double m_bcx = 0, m_bcy = 0, m_bwx = 0, m_bwy = 0;   // 84% contour parameters
	double m_brot = 0;                                   // contour rotation angle (degrees, image coordinates)
	int m_bshape = 0;                                    // 0=ellipse 1=rectangle (selectable)
	bool m_show84 = false;
	QRect m_roi;                  // frame coordinates
	QRect m_drag;                 // rectangle being dragged (staged in view coordinates, converted back to frame coords on release)
	QPoint m_dragStart;
	double m_cx = 0, m_cy = 0;
	bool m_showCentroid = false;
	bool m_roiMode = false;
	bool m_hasFrame = false;
	// Axis support: frame geometry (full-sensor coordinates) + display unit (setUnit)
	double m_scale = 1.0;
	QString m_unit = "px";
	int m_ox = 0, m_oy = 0;         // frame's top-left corner in full-sensor top-left coordinates (stored by setFrame)
	int m_sw = 0, m_sh = 0;         // full-sensor size (falls back to frame size when 0)

	QRect viewToFrame(const QRect& r) const;   // widget coordinates -> frame coordinates
};
