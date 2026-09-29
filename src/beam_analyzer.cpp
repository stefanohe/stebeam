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

// Beam analysis implementation: three passes (peak -> sums/centroid -> second moments); threshold
// gating suppresses stray light; pure C++, zero dependencies.
#include "beam_analyzer.h"
#include <QRect>
#include <algorithm>
#include <cmath>
#include <vector>

// Equal-tail enclosed-energy width: spacing between the two sample positions where the cumulative
// energy crosses the [frac/2, 1-frac/2] percentiles (linear interpolation, unit = sample index).
// frac=0.16 -> 84% enclosed (the standard first-dark-ring energy); for a Gaussian 1D marginal this
// equals 2*sqrt(2)*erfinv(0.84)*sigma ~= 2.8145*sigma.
static double enclosedWidth(const std::vector<double>& m, double frac)
{
	double total = 0;
	for (double v : m) total += v;
	if (total <= 0) return 0;
	double a = total * frac * 0.5, b = total * (1.0 - frac * 0.5);
	double acc = 0, lo = 0, hi = 0;
	bool gotLo = false;
	for (size_t i = 0; i < m.size(); ++i) {
		double prev = acc;
		acc += m[i];
		if (acc <= prev) continue;   // no interpolation across all-zero stretches
		if (!gotLo && acc >= a) { lo = (double)(i - 1) + (a - prev) / (acc - prev); gotLo = true; }
		if (acc >= b) { hi = (double)(i - 1) + (b - prev) / (acc - prev); return hi - lo; }
	}
	return hi - lo;   // when the tail is cut off by the ROI, return whatever width is measurable
}

AnalysisResult analyzeBeam(const Frame& f, const QRect& roi, double background, double thresholdPct)
{
	AnalysisResult r;
	if (!f.valid())
		return r;

	QRect full(0, 0, f.w, f.h);
	QRect box = roi.isNull() ? full : (roi & full);
	if (box.width() < 2 || box.height() < 2)
		return r;

	r.roiX = box.x(); r.roiY = box.y(); r.roiW = box.width(); r.roiH = box.height();
	r.profileX.assign(box.width(), 0.0);
	r.profileY.assign(box.height(), 0.0);

	// Automatic background mode (background<0): use the ROI median -- when the beam covers <1% of the
	// pixels the median approximates the diffuse background level. Stray glow was measured to drift
	// on a minutes scale; under-subtracting with a fixed background would push residual background
	// and heavy tails into the mask and blow up the second moments.
	double bg = background;
	if (bg < 0.0) {
		// Subsample along x for the median estimate (cap ~330k samples): the diffuse background is
		// smooth, so a subsample median is statistically equivalent to the full one; copying +
		// nth_element over all 1.3M frame points was a per-frame GUI hotspot. Small ROIs naturally
		// degrade to full sampling.
		size_t total = (size_t)box.width() * box.height();
		int stride = std::max(1, (int)(total / 262144));
		std::vector<uint16_t> vals;
		vals.reserve(total / stride + box.height());
		for (int y = box.top(); y <= box.bottom(); ++y) {
			const uint16_t* row = &f.px[(size_t)y * f.w];
			for (int x = box.left(); x <= box.right(); x += stride)
				vals.push_back(row[x]);
		}
		auto mid = vals.begin() + vals.size() / 2;
		std::nth_element(vals.begin(), mid, vals.end());
		bg = *mid;
	}
	r.backgroundUsed = bg;   // report in manual mode too (the background readout / CSV header must show the effective value)

	// Pass 1: peak (raw value, the threshold reference)
	for (int y = box.top(); y <= box.bottom(); ++y) {
		const uint16_t* row = &f.px[(size_t)y * f.w];
		for (int x = box.left(); x <= box.right(); ++x)
			if (row[x] > r.peak) { r.peak = row[x]; r.peakX = x; r.peakY = y; }
	}
	double thr = 0;
	if (thresholdPct > 0.0 && (double)r.peak > bg)
		thr = ((double)r.peak - bg) * thresholdPct / 100.0;

	// Pass 2: total power, centroid and profiles after background subtraction (profiles are ungated
	// for display; power/centroid are threshold-gated). gx/gy = per-axis marginal distributions under
	// the same gate, feeding the 84% enclosed-energy widths (one energy accounting with the power).
	std::vector<double> gx(box.width(), 0.0), gy(box.height(), 0.0);
	double sum = 0, sumX = 0, sumY = 0;
	for (int y = box.top(); y <= box.bottom(); ++y) {
		const uint16_t* row = &f.px[(size_t)y * f.w];
		for (int x = box.left(); x <= box.right(); ++x) {
			double v = (double)row[x] - bg;
			if (v < 0) v = 0;
			if (v > thr) {
				sum += v;
				sumX += v * x;
				sumY += v * y;
				gx[x - box.left()] += v;
				gy[y - box.top()] += v;
			}
			r.profileX[x - box.left()] += v;
			r.profileY[y - box.top()] += v;
		}
	}
	if (sum <= 0)
		return r;

	r.power = sum;
	r.cx = sumX / sum;
	r.cy = sumY / sum;

	// Pass 3: second moments (ISO 11146 definitions, including sigma_xy; same threshold gate as the centroid)
	double sxx = 0, syy = 0, sxy = 0;
	for (int y = box.top(); y <= box.bottom(); ++y) {
		const uint16_t* row = &f.px[(size_t)y * f.w];
		double dy = y - r.cy;
		for (int x = box.left(); x <= box.right(); ++x) {
			double v = (double)row[x] - bg;
			if (v <= thr) continue;
			double dx = x - r.cx;
			sxx += v * dx * dx;
			syy += v * dy * dy;
			sxy += v * dx * dy;
		}
	}
	r.sigmaX = std::sqrt(sxx / sum);
	r.sigmaY = std::sqrt(syy / sum);
	r.d4sigmaX = 4.0 * r.sigmaX;
	r.d4sigmaY = 4.0 * r.sigmaY;
	const double kFwhm = 2.0 * std::sqrt(2.0 * std::log(2.0));   // ≈2.3548
	r.fwhmX = kFwhm * r.sigmaX;
	r.fwhmY = kFwhm * r.sigmaY;
	r.d84X = enclosedWidth(gx, 0.16);
	r.d84Y = enclosedWidth(gy, 0.16);
	// Ellipticity = minor/major principal-sigma ratio (eigenvalues of the covariance tensor, rotation
	// invariant). The axis-aligned sigma_x/sigma_y ratio distorts for tilted beams (a 30-degree
	// elongated ellipse measured 0.74 instead of the true 0.5).
	double tr = (sxx + syy) / sum * 0.5;
	double detTerm = std::sqrt(((sxx - syy) / sum * 0.5) * ((sxx - syy) / sum * 0.5) + (sxy / sum) * (sxy / sum));
	double lamMax = tr + detTerm, lamMin = tr - detTerm;
	r.ellipticity = lamMax > 0 && lamMin > -1e-12 ? std::sqrt(std::max(0.0, lamMin) / lamMax) : 0.0;
	// Principal-axis azimuth: principal direction of the second central-moment tensor (sxy accumulated in pass 3, same threshold gate as the widths)
	r.orientationDeg = 0.5 * std::atan2(2.0 * sxy / sum, (sxx - syy) / sum) * (180.0 / std::acos(-1.0));   // root fix: the old atan(-1.0) gave -pi/4, not pi (it once "passed" only because selftest frame generation shared the same error)

	r.valid = true;
	return r;
}

// Inscribed-rectangle scale t: the largest scale that keeps the aspect ratio, stays centered, and
// whose four corners still land inside the source rectangle after counter-rotation (both rectangles
// convex <=> the four-corner test is sufficient); 40 bisection steps converge to a 1e-12 ratio.
// Shared with rotateFrameBilinear so "keep sensor size after rotation" derives acquisition sizes from the same source.
double rotInscribedScale(int W, int H, double deg)
{
	if (deg == 0.0)
		return 1.0;
	const double PI = std::acos(-1.0);
	const double rad = deg * PI / 180.0;
	const double c = std::cos(rad), s = std::sin(rad);
	const double Wd = W, Hd = H;
	double lo = 0.0, hi = 1.0;
	for (int it = 0; it < 40; ++it) {
		double t = (lo + hi) * 0.5;
		double hw = t * Wd * 0.5, hh = t * Hd * 0.5;
		bool ok = true;
		for (int k = 0; k < 4 && ok; ++k) {
			double qx = (k & 1 ? hw : -hw), qy = (k & 2 ? hh : -hh);
			double px = qx * c + qy * s;    // R(−φ)q
			double py = -qx * s + qy * c;
			if (std::fabs(px) > Wd * 0.5 || std::fabs(py) > Hd * 0.5)
				ok = false;
		}
		if (ok) lo = t; else hi = t;
	}
	return lo;
}

// Beam rotation: forward map q = R(phi) p (image coordinates, y down, positive = from +x toward +y);
// each output pixel samples back toward the source (R(-phi)) with bilinear interpolation.
// Inscribed rectangle from rotInscribedScale ("rotate the ellipse's major axis to +Y, sensor shrinks accordingly").
Frame rotateFrameBilinear(const Frame& src, double deg)
{
	if (!src.valid() || deg == 0.0)
		return src;
	const double lo = rotInscribedScale(src.w, src.h, deg);
	const int ow = std::max(2, (int)std::floor(lo * src.w)), oh = std::max(2, (int)std::floor(lo * src.h));
	if (ow >= src.w && oh >= src.h)
		return src;   // numerically equal to the source frame, skip resampling
	return rotateFrameBilinear(src, deg, ow, oh);
}

// Exact-size variant: output = outW x outH, sampled counter-rotated about the source-frame center,
// corner regions outside the source read 0. The inscribed variant and the keep-size variant share this sampling core.
Frame rotateFrameBilinear(const Frame& src, double deg, int outW, int outH)
{
	if (!src.valid() || deg == 0.0 || outW < 2 || outH < 2)
		return src;
	const double PI = std::acos(-1.0);
	const double rad = deg * PI / 180.0;
	const double c = std::cos(rad), s = std::sin(rad);

	Frame o;
	o.w = outW;
	o.h = outH;
	o.ts = src.ts;
	o.captureMs = src.captureMs;
	o.px.assign((size_t)o.w * o.h, 0);
	const double cxO = (o.w - 1) * 0.5, cyO = (o.h - 1) * 0.5;
	const double cxS = (src.w - 1) * 0.5, cyS = (src.h - 1) * 0.5;
	for (int y = 0; y < o.h; ++y) {
		double qy = y - cyO;
		for (int x = 0; x < o.w; ++x) {
			double qx = x - cxO;
			double fx = cxS + qx * c + qy * s;    // R(-phi)q + source center
			double fy = cyS - qx * s + qy * c;
			int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
			double ax = fx - x0, ay = fy - y0;
			double v;
			// Performance: the vast majority of inscribed-rectangle pixels lie strictly inside the
			// source, so read the 4 neighbors by direct pointer without per-pixel bounds checks (the
			// old at() cost 4 branches per pixel -- the bulk of rotation time at 1280x1024);
			// only the thin border strip takes the clamped path.
			if (x0 >= 0 && x0 + 1 < src.w && y0 >= 0 && y0 + 1 < src.h) {
				const uint16_t* p = &src.px[(size_t)y0 * src.w + x0];
				v = p[0] * (1 - ax) * (1 - ay) + p[1] * ax * (1 - ay)
				  + p[src.w] * (1 - ax) * ay + p[src.w + 1] * ax * ay;
			} else {
				auto at = [&](int xx, int yy) -> double {
					if (xx < 0 || yy < 0 || xx >= src.w || yy >= src.h) return 0.0;
					return src.px[(size_t)yy * src.w + xx];
				};
				v = at(x0, y0) * (1 - ax) * (1 - ay) + at(x0 + 1, y0) * ax * (1 - ay)
				  + at(x0, y0 + 1) * (1 - ax) * ay + at(x0 + 1, y0 + 1) * ax * ay;
			}
			o.px[(size_t)y * o.w + x] = (uint16_t)(v + 0.5);
		}
	}
	return o;
}
