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

#include "view3d.h"
#include "colormap.h"
#include "i18n.h"   // painted text follows the main window's language switch
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>

View3D::View3D(QWidget* parent) : QWidget(parent)
{
	setMinimumSize(480, 384);
	setCursor(Qt::SizeAllCursor);
	m_throttle.start();
	m_adu.assign((size_t)GWMAX * GHMAX, 0.f);
}

void View3D::setFrame(const Frame& f)
{
	if (!f.valid())
		return;
	// Float bin mapping: cell i covers pixels [i*w/m_gw, (i+1)*w/m_gw) -- seamless full coverage
	// (the old integer stride dropped trailing rows/columns: only 1248 of 1280 columns were used)
	m_gw = std::min(m_qw, f.w);
	m_gh = std::min(m_qh, f.h);
	for (int j = 0; j < m_gh; ++j) {
		int y0 = (int)((long long)j * f.h / m_gh);
		int y1 = std::max(y0 + 1, (int)((long long)(j + 1) * f.h / m_gh));
		for (int i = 0; i < m_gw; ++i) {
			int x0 = (int)((long long)i * f.w / m_gw);
			int x1 = std::max(x0 + 1, (int)((long long)(i + 1) * f.w / m_gw));
			double s = 0;
			int n = 0;
			for (int yy = y0; yy < y1; ++yy) {
				const uint16_t* row = &f.px[(size_t)yy * f.w];
				for (int xx = x0; xx < x1; ++xx) { s += row[xx]; ++n; }
			}
			m_adu[(size_t)j * m_gw + i] = n ? (float)(s / n) : 0.f;
		}
	}
	bool first = !m_has;
	m_has = true;
	if (first || m_throttle.elapsed() >= 66) {   // ~15fps throttle: data always stored, repaints skipped
		m_throttle.restart();
		update();
	}
}

void View3D::setRange(int lo, int hi)
{
	m_lo = lo;
	m_hi = std::max(hi, lo + 1);
	update();
}

void View3D::setAngles(double yaw, double pitch)
{
	m_yaw = yaw;
	m_pitch = std::clamp(pitch, 5.0, 89.0);
	update();
}

void View3D::setQuality(int q)
{
	static const int kQ[][2] = { {96, 76}, {192, 152}, {320, 256} };   // standard / high / ultra
	q = std::clamp(q, 0, 2);
	m_qw = kQ[q][0];
	m_qh = kQ[q][1];
	m_has = false;   // old grid is void; the next frame resamples at the new level (grab intervals <=17ms make the gap invisible)
	update();
}

void View3D::setSmooth(bool s)
{
	m_smooth = s;
	update();
}

void View3D::paintEvent(QPaintEvent*)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);
	p.fillRect(rect(), viewBgColor());   // dark-gray background (same as the 2D view, not pure black)
	if (!m_has || m_gw < 2 || m_gh < 2) {
		p.setPen(Qt::gray);
		p.drawText(rect(), Qt::AlignCenter, L("等待帧…", "Waiting for frame…"));
		return;
	}
	static QRgb lut[256];
	static bool lutDone = false;
	if (!lutDone) { buildJetLut(lut); lutDone = true; }
	QElapsedTimer paintClock;   // for the diagnostic probe: this frame's render time (rasterization + blit)
	if (qEnvironmentVariableIsSet("STEBEAM_3D_DBG")) paintClock.start();

	const double PI = std::acos(-1.0);   // root fix: the old std::atan(-1.0) is -pi/4, not pi, which shrank angles to -1/10x (the other half of the real root cause of "drag rotation does nothing"; the correct idiom is acos(-1))
	double cyw = std::cos(m_yaw * PI / 180.0), syw = std::sin(m_yaw * PI / 180.0);
	double se = std::sin(m_pitch * PI / 180.0), ce = std::cos(m_pitch * PI / 180.0);
	double S = std::min(width(), height()) * 0.42 * m_zoom;
	QPointF C(width() / 2.0, height() / 2.0 + height() * 0.06);
	double span = std::max(1.0, (double)(m_hi - m_lo));

	// Vertex projection (orthographic: yaw in-plane rotation + pitch tilt; larger depth = farther) + per-vertex color index
	std::vector<QPointF> P((size_t)m_gw * m_gh);
	std::vector<float> D((size_t)m_gw * m_gh), IX((size_t)m_gw * m_gh);
	for (int j = 0; j < m_gh; ++j) {
		double v = j / (double)(m_gh - 1) * 2.0 - 1.0;
		for (int i = 0; i < m_gw; ++i) {
			double u = i / (double)(m_gw - 1) * 2.0 - 1.0;
			double zn = std::clamp(((double)m_adu[(size_t)j * m_gw + i] - m_lo) / span, 0.0, 1.0);
			double h = (zn - 0.5) * 0.9;
			double X = u * cyw - v * syw;
			double Y = u * syw + v * cyw;
			P[(size_t)j * m_gw + i] = QPointF(C.x() + X * S, C.y() - (Y * se + h * ce) * S);
			D[(size_t)j * m_gw + i] = (float)(Y * ce - h * se);
			IX[(size_t)j * m_gw + i] = (float)(zn * 255.999);
		}
	}

	// Per-pixel z-buffer rasterization (root fix for "the beam disappears where it overlaps the
	// background"): the old painter's algorithm drew quads sorted by average depth, so a foreground
	// flat tile could swallow the taller beam quad behind it wholesale (average depth cannot
	// express occlusion between partially overlapping quads -- confirmed on a real frame where the
	// beam's base was buried by the foreground background plane). The z-buffer keeps only the
	// nearest face per pixel, making occlusion pixel-exact, so the beam always emerges correctly
	// from behind foreground planes; full opacity unchanged.
	const int W = width(), H = height();
	if (m_img.size() != QSize(W, H))
		m_img = QImage(W, H, QImage::Format_RGB32);
	std::fill_n((QRgb*)m_img.bits(), (size_t)W * H, viewBgColor().rgb());
	m_zbuf.assign((size_t)W * H, 1e30f);

	// Triangle raster: depth and color index are both linear in screen space (orthographic projection
	// = affine, so barycentric interpolation degenerates to a linear screen-space form); the
	// top-left fill rule assigns each shared-edge pixel to exactly one of the two adjacent triangles, leaving no seams
	auto rasterTri = [&](QPointF v0, QPointF v1, QPointF v2, float d0, float d1, float d2,
	                     float i0, float i1, float i2) {
		auto cr = [](QPointF a, QPointF b) { return a.x() * b.y() - a.y() * b.x(); };
		double area = cr(v1 - v0, v2 - v0);
		if (area == 0)
			return;
		if (area < 0) { std::swap(v1, v2); std::swap(d1, d2); std::swap(i1, i2); area = -area; }
		const bool tl01 = (v1.y() < v0.y()) || (v1.y() == v0.y() && v1.x() < v0.x());
		const bool tl12 = (v2.y() < v1.y()) || (v2.y() == v1.y() && v2.x() < v1.x());
		const bool tl20 = (v0.y() < v2.y()) || (v0.y() == v2.y() && v0.x() < v2.x());
		const double idet = 1.0 / area;
		const double aD = ((double)(d1 - d0) * (v2.y() - v0.y()) - (double)(d2 - d0) * (v1.y() - v0.y())) * idet;
		const double bD = ((double)(d2 - d0) * (v1.x() - v0.x()) - (double)(d1 - d0) * (v2.x() - v0.x())) * idet;
		const double cD = d0 - aD * v0.x() - bD * v0.y();
		const double aI = ((double)(i1 - i0) * (v2.y() - v0.y()) - (double)(i2 - i0) * (v1.y() - v0.y())) * idet;
		const double bI = ((double)(i2 - i0) * (v1.x() - v0.x()) - (double)(i1 - i0) * (v2.x() - v0.x())) * idet;
		const double cI = i0 - aI * v0.x() - bI * v0.y();
		const int x0 = std::max(0, (int)std::floor(std::min({v0.x(), v1.x(), v2.x()})));
		const int x1 = std::min(W - 1, (int)std::ceil(std::max({v0.x(), v1.x(), v2.x()})));
		const int y0 = std::max(0, (int)std::floor(std::min({v0.y(), v1.y(), v2.y()})));
		const int y1 = std::min(H - 1, (int)std::ceil(std::max({v0.y(), v1.y(), v2.y()})));
		for (int y = y0; y <= y1; ++y) {
			const double py = y + 0.5;
			double e01 = (v1.x() - v0.x()) * (py - v0.y()) - (v1.y() - v0.y()) * (x0 + 0.5 - v0.x());
			double e12 = (v2.x() - v1.x()) * (py - v1.y()) - (v2.y() - v1.y()) * (x0 + 0.5 - v1.x());
			double e20 = (v0.x() - v2.x()) * (py - v2.y()) - (v0.y() - v2.y()) * (x0 + 0.5 - v2.x());
			double dRow = aD * (x0 + 0.5) + bD * py + cD;
			double iRow = aI * (x0 + 0.5) + bI * py + cI;
			QRgb* row = (QRgb*)m_img.scanLine(y);
			float* zrow = &m_zbuf[(size_t)y * W];
			for (int x = x0; x <= x1; ++x,
				     e01 -= (v1.y() - v0.y()), e12 -= (v2.y() - v1.y()), e20 -= (v0.y() - v2.y()),
				     dRow += aD, iRow += aI) {
				if (!(tl01 ? e01 >= 0 : e01 > 0) || !(tl12 ? e12 >= 0 : e12 > 0) || !(tl20 ? e20 >= 0 : e20 > 0))
					continue;
				if (dRow >= zrow[x])
					continue;
				zrow[x] = (float)dRow;
				const int idx = (int)iRow;
				row[x] = lut[idx < 0 ? 0 : (idx > 255 ? 255 : idx)];
			}
		}
	};

	for (int j = 0; j + 1 < m_gh; ++j) {
		for (int i = 0; i + 1 < m_gw; ++i) {
			const size_t a = (size_t)j * m_gw + i, b = a + 1, c = a + m_gw + 1, d = a + m_gw;
			if (m_smooth) {   // smooth: interpolate vertex color indices per pixel for a continuous gradient
				rasterTri(P[a], P[b], P[c], D[a], D[b], D[c], IX[a], IX[b], IX[c]);
				rasterTri(P[a], P[c], P[d], D[a], D[c], D[d], IX[a], IX[c], IX[d]);
			} else {          // fast: flat quad fill (both triangles one color), still pixel-exact depth testing
				const float flat = std::clamp((int)((IX[a] + IX[b] + IX[c] + IX[d]) * 0.25f), 0, 255);
				rasterTri(P[a], P[b], P[c], D[a], D[b], D[c], flat, flat, flat);
				rasterTri(P[a], P[c], P[d], D[a], D[c], D[d], flat, flat, flat);
			}
		}
	}
	p.drawImage(0, 0, m_img);

	if (qEnvironmentVariableIsSet("STEBEAM_3D_DBG"))   // diagnostic probe: effective angles + grid + shading mode + render time (troubleshooting hook for "angles not applied / occlusion")
		printf("[3d] yaw=%.1f pitch=%.1f S=%.0f grid=%dx%d quads=%d %s paint=%.1fms size=%dx%d\n",
		       m_yaw, m_pitch, S, m_gw, m_gh, (m_gw - 1) * (m_gh - 1), m_smooth ? "smooth" : "fast",
		       (double)paintClock.nsecsElapsed() / 1e6, W, H);
	p.setPen(QColor(0x9a, 0x9a, 0x9a));
	p.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignLeft,
	           L("拖拽旋转 · 滚轮缩放", "Drag to rotate · wheel to zoom"));
}

void View3D::mousePressEvent(QMouseEvent* e)
{
	if (e->button() == Qt::LeftButton)
		m_lastPt = e->pos();
}

void View3D::mouseMoveEvent(QMouseEvent* e)
{
	if (e->buttons() & Qt::LeftButton) {
		QPoint d = e->pos() - m_lastPt;
		m_lastPt = e->pos();
		m_yaw += d.x() * 0.4;
		m_pitch = std::clamp(m_pitch + d.y() * 0.3, 5.0, 89.0);
		update();
	}
}

void View3D::wheelEvent(QWheelEvent* e)
{
	m_zoom = std::clamp(m_zoom * (e->angleDelta().y() > 0 ? 1.1 : 0.9), 0.3, 3.0);
	update();
}
