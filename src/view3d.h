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

// 3D beam surface view (pure QPainter software rendering): downsample the frame to a grid, then
// rasterize a filled surface with a per-pixel z-buffer. The color scale and dark-gray background
// share one source of truth with the 2D view (colormap.h: same dark-background rainbow, minimum
// dark gray -> dark blue -> rainbow). Left-drag rotates, wheel zooms; frame-driven repaints are
// throttled to ~15fps; frames are fed only while the stacked page is visible (2D/3D are mutually
// exclusive to save compute, scheduled by MainWindow). Fully opaque (no translucency).
// Rendering core rewritten: the old painter's algorithm sorted quads by average depth, so a
// foreground flat tile could completely cover a taller beam quad behind it (average depth cannot
// express occlusion between partially overlapping quads -- reproduced on real frames as "the beam
// disappears where it overlaps the background"). Now a per-pixel z-buffer keeps the nearest face,
// making occlusion pixel-exact. Three resolution levels + two shading levels.
#pragma once
#include "frame.h"
#include <QElapsedTimer>
#include <QImage>
#include <QWidget>
#include <vector>

class View3D : public QWidget
{
	Q_OBJECT
public:
	explicit View3D(QWidget* parent = nullptr);
	void setFrame(const Frame& f);    // caches the bin-mean downsample; repaints throttled to 15fps (data always stored, repaints skipped)
	void setRange(int lo, int hi);    // height normalization range (kept in sync with the 2D pseudocolor's active range)
	void setAngles(double yaw, double pitch);   // explicit view angle (probe hook: --selfviews rotates and grabs to reproduce the occlusion reading)
	void setQuality(int q);           // 0=standard / 1=high / 2=ultra (progressively denser grids)
	void setSmooth(bool s);           // true=smooth (per-pixel color interpolation) / false=fast (flat quad fill, cheaper)
	// Current surface grid data (CSV export saves the view's own data: for the 3D surface that is its
	// downsampled bin-mean matrix, row-major, row stride = gridW)
	bool hasGrid() const { return m_has && m_gw > 0 && m_gh > 0; }
	int gridW() const { return m_gw; }
	int gridH() const { return m_gh; }
	const std::vector<float>& gridAdu() const { return m_adu; }

protected:
	void paintEvent(QPaintEvent*) override;
	void mousePressEvent(QMouseEvent*) override;
	void mouseMoveEvent(QMouseEvent*) override;
	void wheelEvent(QWheelEvent*) override;

private:
	static constexpr int GWMAX = 320, GHMAX = 256; // ultra-grid upper bound (m_adu preallocated to it, so quality switches avoid reallocation)
	int m_qw = 192, m_qh = 152;                    // target grid of the current quality level (default = high)
	std::vector<float> m_adu;                      // bin means (raw ADU; normalized by range at paint time so range changes need no resampling; row stride = current grid width m_gw)
	int m_gw = 0, m_gh = 0;                        // actual effective grid (float bin mapping = min(target grid, frame size), full coverage)
	bool m_has = false;
	int m_lo = -100, m_hi = 4096;
	double m_yaw = -35, m_pitch = 30;              // degrees: yaw / pitch (5..89)
	double m_zoom = 1.0;
	bool m_smooth = true;                          // shading mode; the initial value must match the combo default -- when the ini lacks the key, loadSettingsIni returns early without calling setSmooth, and a mismatch would show "smooth" while rendering fast
	QPoint m_lastPt;
	QElapsedTimer m_throttle;                      // frame-driven repaint throttle (~15fps)
	QImage m_img;                                  // z-buffer raster output canvas (sized with the widget)
	std::vector<float> m_zbuf;                     // per-pixel nearest depth (reset at paint)
};
