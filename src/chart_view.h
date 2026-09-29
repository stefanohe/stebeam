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

// Generic line-chart view (pure QPainter, header-only), shared by the profile and beam-size pages.
// It replaces QtCharts: Qt6Charts is GPL-3.0-only, and dropping it keeps the whole dependency
// closure LGPL-3.0-compatible. Visual language follows track_view: white background, light-gray
// 5x5 grid, 8pt tick labels, y axis ascending upward. Ranges, titles and decimal places are all
// set by the host (the pages keep their own auto-tight-window + custom-bound logic; this view only
// draws within the given range). Legend = color swatch + name at the plot's top-right corner.
#pragma once
#include "i18n.h"   // painted text follows the same language switch as the main window
#include <QPainter>
#include <QPainterPath>   // since Qt 6, QPainter no longer transitively includes QPainterPath
#include <QWidget>
#include <QVector>

class LineChartView : public QWidget
{
public:
	struct Series { QString name; QColor color; QVector<QPointF> pts; };

	explicit LineChartView(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(480, 384); }

	void setSeries(QVector<Series> s) { m_series = std::move(s); update(); }
	void setXRange(double lo, double hi) { if (hi > lo) { m_xLo = lo; m_xHi = hi; } }
	void setYRange(double lo, double hi) { if (hi > lo) { m_yLo = lo; m_yHi = hi; } }
	void setTitles(const QString& top, const QString& x, const QString& y)
	{
		m_top = top; m_xTitle = x; m_yTitle = y; update();
	}
	void setDecimals(int xDec, int yDec) { m_xDec = xDec; m_yDec = yDec; }

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.fillRect(rect(), Qt::white);
		const int ML = 70, MR = 14, MT = 34, MB = 46;
		QRectF plot(ML, MT, width() - ML - MR, height() - MT - MB);
		p.setPen(QColor(0x33, 0x33, 0x33));
		if (!m_top.isEmpty())
			p.drawText(QRectF(0, 6, width(), 22), Qt::AlignHCenter, m_top);
		if (plot.width() < 20 || plot.height() < 20) return;
		auto mapX = [&](double v) { return plot.left() + (v - m_xLo) / (m_xHi - m_xLo) * plot.width(); };
		auto mapY = [&](double v) { return plot.bottom() - (v - m_yLo) / (m_yHi - m_yLo) * plot.height(); };
		QFont lf = font(); lf.setPointSizeF(8.0); p.setFont(lf);
		for (int i = 0; i <= 4; ++i) {   // 5x5 grid + tick values (same as track_view)
			double fx = m_xLo + (m_xHi - m_xLo) * i / 4.0, fy = m_yLo + (m_yHi - m_yLo) * i / 4.0;
			double px = mapX(fx), py = mapY(fy);
			p.setPen(QColor(0xe6, 0xe6, 0xe6));
			p.drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom()));
			p.drawLine(QPointF(plot.left(), py), QPointF(plot.right(), py));
			p.setPen(QColor(0x66, 0x66, 0x66));
			p.drawText(QRectF(px - 55, plot.bottom() + 4, 110, 16), Qt::AlignHCenter,
			           QString::number(fx, 'f', m_xDec));
			p.drawText(QRectF(2, py - 8, ML - 10, 16), Qt::AlignRight | Qt::AlignVCenter,
			           QString::number(fy, 'f', m_yDec));
		}
		p.setPen(QColor(0x99, 0x99, 0x99));
		if (!m_xTitle.isEmpty())
			p.drawText(QRectF(plot.left(), height() - 20, plot.width(), 16), Qt::AlignHCenter, m_xTitle);
		if (!m_yTitle.isEmpty()) {
			p.save();
			p.translate(12, plot.center().y());
			p.rotate(-90);
			p.drawText(QRectF(-plot.height() / 2, -8, plot.height(), 16), Qt::AlignHCenter, m_yTitle);
			p.restore();
		}
		p.setPen(QColor(0xbb, 0xbb, 0xbb));
		p.setBrush(Qt::NoBrush);
		p.drawRect(plot);
		// Series polylines: clipped to the plot rect (out-of-range points are cut); antialiasing off to keep the old QChartView look
		p.save();
		p.setClipRect(plot);
		p.setRenderHint(QPainter::Antialiasing, false);
		for (const Series& s : m_series) {
			if (s.pts.size() < 2) continue;
			QPainterPath path;
			path.moveTo(mapX(s.pts[0].x()), mapY(s.pts[0].y()));
			for (int i = 1; i < s.pts.size(); ++i)
				path.lineTo(mapX(s.pts[i].x()), mapY(s.pts[i].y()));
			p.setPen(QPen(s.color, 1));
			p.drawPath(path);
		}
		p.restore();
		// Legend: horizontal swatch+name row at the plot's top-right (same placement semantics as a QChart legend)
		if (!m_series.isEmpty()) {
			int lx = (int)plot.right() - 10, ly = (int)plot.top() + 6;
			for (int i = m_series.size() - 1; i >= 0; --i) {   // lay out right-to-left
				const int w = 12 + 4 + fontMetrics().horizontalAdvance(m_series[i].name) + 14;
				lx -= w;
				p.fillRect(QRectF(lx, ly, 12, 8), m_series[i].color);
				p.setPen(QColor(0x33, 0x33, 0x33));
				p.drawText(QRectF(lx + 16, ly - 4, w, 16), Qt::AlignLeft | Qt::AlignVCenter, m_series[i].name);
			}
		}
	}

private:
	QVector<Series> m_series;
	double m_xLo = 0, m_xHi = 1, m_yLo = 0, m_yHi = 1;
	QString m_top, m_xTitle, m_yTitle;
	int m_xDec = 1, m_yDec = 1;
};
