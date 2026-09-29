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

// Beam-tracking view (pure QPainter, header-only): centroid-trajectory scatter with y pointing UP --
// data is in sensor coordinates with bottom-left origin (the app-wide convention: larger values up,
// same frame of reference as the position readouts).
// Why not QChart (root cause of "the track tab shows axes but no beam", verified by a pixel-grab
// probe): QtCharts value axes do not support reversed ranges -- setRange(1024,0) makes every series
// attached to that axis vanish, and Qt 6.11 QtCharts offers no axis-inversion API at all; a forward
// range would mirror the plot vertically and contradict the image orientation. Hence custom painting.
// Data = MainWindow's m_trackHist (raw px, always the most recent 2000 points), multiplied by the
// current size scale at display time; older points fade darker = time encoding.
#pragma once
#include "i18n.h"   // painted text follows the main window's language switch
#include <QPainter>
#include <QWidget>
#include <algorithm>
#include <deque>

// Centroid trajectory point: full-sensor px + capture instant in epoch ms (the CSV export emits X/Y per timestamp)
struct TrackPoint { double x = 0, y = 0; qint64 ms = 0; };

class TrackView : public QWidget
{
public:
	explicit TrackView(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(480, 384); }
	void setSource(const std::deque<TrackPoint>* hist) { m_hist = hist; }   // pointer to a MainWindow member deque (host outlives this view)
	void setUnit(double scale, const QString& unit) { m_scale = scale; m_unit = unit; update(); }
	// Custom axis bounds: off = automatic tight window, on = fixed range; values are in the current display unit, compared in the same domain as the converted coordinates at paint time
	void setCustomRange(bool xOn, double xLo, double xHi, bool yOn, double yLo, double yHi)
	{
		m_xCustom = xOn; m_xLo = xLo; m_xHi = xHi;
		m_yCustom = yOn; m_yLo = yLo; m_yHi = yHi;
		update();
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.fillRect(rect(), Qt::white);   // same white background as the profile / beam-size pages
		const int ML = 70, MR = 14, MT = 34, MB = 46;
		QRectF plot(ML, MT, width() - ML - MR, height() - MT - MB);
		p.setPen(QColor(0x33, 0x33, 0x33));
		p.drawText(QRectF(0, 6, width(), 22), Qt::AlignHCenter, L("光束追踪（质心轨迹）", "Centroid Track (beam path)"));
		if (!m_hist || m_hist->empty() || plot.width() < 20 || plot.height() < 20) {
			p.setPen(Qt::gray);
			p.drawText(plot, Qt::AlignCenter, L("等待帧…", "Waiting for frame…"));
			return;
		}
		double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
		for (const TrackPoint& q : *m_hist) {
			double x = q.x * m_scale, y = q.y * m_scale;
			minX = std::min(minX, x); maxX = std::max(maxX, x);
			minY = std::min(minY, y); maxY = std::max(maxY, y);
		}
		double padX = std::max((maxX - minX) * 0.15, 2.0 * m_scale);
		double padY = std::max((maxY - minY) * 0.15, 2.0 * m_scale);
		minX -= padX; maxX += padX; minY -= padY; maxY += padY;
		if (m_xCustom && m_xHi > m_xLo) { minX = m_xLo; maxX = m_xHi; }   // custom bounds override the automatic window
		if (m_yCustom && m_yHi > m_yLo) { minY = m_yLo; maxY = m_yHi; }
		auto mapX = [&](double v) { return plot.left() + (v - minX) / (maxX - minX) * plot.width(); };
		auto mapY = [&](double v) { return plot.bottom() - (v - minY) / (maxY - minY) * plot.height(); };   // y up: sensor bottom-left origin (app-wide convention)
		QFont lf = font(); lf.setPointSizeF(8.0); p.setFont(lf);   // copy the widget font and change only the size (inherits the resolved family instead of rebuilding a QFont)
		const int dec = m_unit == QLatin1String("px") ? 1 : 3;   // tick decimals follow the readout rule: px 1 digit, um/mm 3 digits
		for (int i = 0; i <= 4; ++i) {   // 5x5 grid + tick values
			double fx = minX + (maxX - minX) * i / 4.0, fy = minY + (maxY - minY) * i / 4.0;
			double px = mapX(fx), py = mapY(fy);
			p.setPen(QColor(0xe6, 0xe6, 0xe6));
			p.drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom()));
			p.drawLine(QPointF(plot.left(), py), QPointF(plot.right(), py));
			p.setPen(QColor(0x66, 0x66, 0x66));
			p.drawText(QRectF(px - 55, plot.bottom() + 4, 110, 16), Qt::AlignHCenter,
			           QString::number(fx, 'f', dec));
			p.drawText(QRectF(2, py - 8, ML - 10, 16), Qt::AlignRight | Qt::AlignVCenter,
			           QString::number(fy, 'f', dec));
		}
		p.setPen(QColor(0x99, 0x99, 0x99));
		p.drawText(QRectF(plot.left(), height() - 20, plot.width(), 16), Qt::AlignHCenter, "x (" + m_unit + ")");
		p.save();
		p.translate(12, plot.center().y());
		p.rotate(-90);
		p.drawText(QRectF(-100, -12, 200, 16), Qt::AlignHCenter, "y (" + m_unit + ")");
		p.restore();
		p.setPen(QColor(0xbb, 0xbb, 0xbb));
		p.setBrush(Qt::NoBrush);
		p.drawRect(plot);
		size_t n = m_hist->size();
		p.setPen(Qt::NoPen);
		for (size_t k = 0; k < n; ++k) {   // scatter: newer points darker (time encoding)
			int a = 60 + (int)(195.0 * k / std::max<size_t>(n - 1, 1));
			p.setBrush(QColor(32, 159, 223, a));
			p.drawEllipse(QPointF(mapX((*m_hist)[k].x * m_scale), mapY((*m_hist)[k].y * m_scale)), 2.5, 2.5);
		}
	}

private:
	const std::deque<TrackPoint>* m_hist = nullptr;
	double m_scale = 1.0;
	QString m_unit = "px";
	bool m_xCustom = false, m_yCustom = false;
	double m_xLo = 0, m_xHi = 0, m_yLo = 0, m_yHi = 0;
};
