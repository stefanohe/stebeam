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

// Beam analysis: centroid / second moments / D4sigma beam widths / ellipticity / profiles / peak
// (ISO 11146 second-moment definitions).
#pragma once
#include "frame.h"
#include <QRect>
#include <cmath>
#include <vector>

struct AnalysisResult {
	bool valid = false;
	double power = 0;                 // sum of (px - bg) over the ROI, negatives clamped to 0
	double cx = 0, cy = 0;            // centroid (frame coordinates, px)
	double sigmaX = 0, sigmaY = 0;    // second-moment standard deviations (px)
	double d4sigmaX = 0, d4sigmaY = 0; // D4sigma beam width = 4*sigma (px)
	double d84X = 0, d84Y = 0;       // 84% enclosed-energy width (px) -- the standard first-dark-ring energy measure (the Airy first dark ring encloses 84% of the energy);
	                                  // per axis it is the 8%/92% equal-tail percentile spacing; for a Gaussian 1D marginal the theoretical value is 2*sqrt(2)*erfinv(0.84)*sigma ~= 2.8145*sigma
	double fwhmX = 0, fwhmY = 0;      // Gaussian-approximate FWHM = 2*sqrt(2*ln2)*sigma (px)
	double ellipticity = 0;           // minor/major axis ratio (0..1)
	double orientationDeg = 0;        // azimuth of the principal (max-variance) axis, degrees -90..90; theta = 0.5*atan2(2*mu_xy, mu_xx - mu_yy),
	                                  // the image y axis points down, so a positive angle rotates from +x toward +y (down)
	uint16_t peak = 0;
	int peakX = 0, peakY = 0;
	double cx0 = 0, cy0 = 0;            // centroid in full-sensor coordinates (px, bottom-left origin y up -- the app-wide convention;
	                                    // the caller maps it back to the unrotated frame via unrotatePos, then converts with frameToTarget) --
	                                    // position readouts and beam tracking use these: a centroid on a rotated frame sweeps an arc as the rotation angle changes
	int peakX0 = 0, peakY0 = 0;         // peak position in full-sensor coordinates (px, bottom-left origin, same as above)
	double backgroundUsed = 0;          // effective background (auto mode = ROI median; echoed back by UI/probes)
	std::vector<double> profileX;     // along X: per-column pixel sums (length = ROI width)
	std::vector<double> profileY;     // along Y: per-row pixel sums (length = ROI height)
	int roiX = 0, roiY = 0, roiW = 0, roiH = 0;
};

// background: per-pixel background subtraction (0 = none, <0 = auto via ROI median); an empty roi rect = whole frame.
// thresholdPct: threshold gating (percent of peak, 0 = off) -- after background subtraction, pixels
// below the threshold are excluded from power/centroid/second moments; this suppresses diffuse
// stray light at the cost of underestimating Gaussian beam widths by roughly (threshold% -> sigma
// underestimate ~1.6x threshold%). Use 0 for pure ISO definitions.
AnalysisResult analyzeBeam(const Frame& f, const QRect& roi, double background, double thresholdPct = 0.0);

// Beam rotation: rotate the image content about its center by deg (image coordinates, positive =
// from +x toward +y), resampled with bilinear interpolation; the output is cropped to the largest
// inscribed rectangle that can still be fully sampled after rotation ("the whole sensor shrinks
// accordingly"), so rotating the beam's major axis vertical uses alpha = normalize(90 - orientationDeg).
// deg = 0 returns the frame unchanged.
Frame rotateFrameBilinear(const Frame& src, double deg);

// Exact resampling to a requested output size: sample counter-rotated about the source-frame center,
// output is always outW x outH (no inscribed-rectangle crop); corner regions whose samples fall
// outside the source read 0 (when the acquisition size >= the bounding box of the rotated output
// rect the black corners are zero; when acquisition is clamped by the sensor limit, black corners
// honestly show what the limit allows)
Frame rotateFrameBilinear(const Frame& src, double deg, int outW, int outH);

// Inscribed-rectangle scale t (shared core of rotateFrameBilinear's "keep sensor size after rotation"):
// a W x H source rotated by deg yields t*W x t*H; callers derive the required acquisition size as
// desired/t (clamped by the sensor limit)
double rotInscribedScale(int W, int H, double deg);

// Inverse position transform: map position quantities (centroid/peak) measured on a rotated frame
// back to unrotated-frame coordinates. Rotation is about the frame center -- when the centroid is
// off-center, changing the rotation angle sweeps it along an arc on the rotated frame and the
// tracking trace drifts into a slanted line; positions must be expressed in the unrotated frame
// (the sensor's true location). The formula is the direct inverse of rotateFrameBilinear's
// sampling map fx = cxS + qx*c + qy*s / fy = cyS - qx*s + qy*c.
inline void unrotatePos(const Frame& src, const Frame& rot, double deg, double& x, double& y)
{
	if (deg == 0.0 || (rot.w == src.w && rot.h == src.h)) return;   // no actual rotation (returned as-is) -> positions are already in unrotated-frame coordinates
	const double rad = deg * std::acos(-1.0) / 180.0;
	const double c = std::cos(rad), s = std::sin(rad);
	const double qx = x - (rot.w - 1) * 0.5, qy = y - (rot.h - 1) * 0.5;
	x = (src.w - 1) * 0.5 + qx * c + qy * s;
	y = (src.h - 1) * 0.5 - qx * s + qy * c;
}

// Frame coordinates (top-left origin, y down) -> full-sensor coordinates (bottom-left origin, y up,
// the app-wide convention). A frame may be a sensor sub-window (originX/originY = the frame's
// top-left corner in full-sensor top-left coordinates, 0 when uncropped): first translate back into
// the full-sensor top-left system, then flip y about the sensor's bottom edge (when sensorH = 0,
// use the frame height, i.e. an uncropped frame). Position readouts, beam tracking and the sensor
// window center all share this convention.
inline void frameToTarget(const Frame& f, double xTop, double yTop, double& xb, double& yb)
{
	const int sh = f.sensorH > 0 ? f.sensorH : f.h;
	xb = f.originX + xTop;
	yb = (sh - 1) - (f.originY + yTop);
}
