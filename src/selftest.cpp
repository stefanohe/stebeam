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

// Headless self-test: synthesize Gaussian beams -> analyze -> compare against analytic ground truth
// (the entry point of the automated debugging loop).
// Usage: SteBeam.exe --selftest   prints PASS/FAIL lines, final line status=ok / status=fail
#include "beam_analyzer.h"
#include <QRect>
#include <cmath>
#include <cstdio>
#include <random>

static Frame makeGauss(int w, int h, double cx, double cy, double sx, double sy,
                       double amp, double bg, unsigned seed)
{
	Frame f;
	f.w = w; f.h = h;
	f.px.assign((size_t)w * h, 0);
	std::mt19937 rng(seed);
	std::normal_distribution<double> noise(0.0, 3.0);
	for (int y = 0; y < h; ++y) {
		double dy = y - cy;
		for (int x = 0; x < w; ++x) {
			double dx = x - cx;
			double g = amp * std::exp(-(dx * dx / (2 * sx * sx) + dy * dy / (2 * sy * sy)));
			double v = g + bg + noise(rng);
			if (v < 0) v = 0;
			if (v > 65535) v = 65535;
			f.px[(size_t)y * w + x] = (uint16_t)(v + 0.5);
		}
	}
	return f;
}

// Rotated elliptical Gaussian: major axis sigmaMajor along (cos phi, sin phi) (image coordinates, y down),
// minor axis sigmaMinor perpendicular -- used to verify orientation recovery.
static Frame makeGaussRot(int w, int h, double cx, double cy, double sMinor, double sMajor,
                          double phiDeg, double amp, double bg, unsigned seed)
{
	Frame f;
	f.w = w; f.h = h;
	f.px.assign((size_t)w * h, 0);
	const double phi = phiDeg * std::acos(-1.0) / 180.0;   // root fix: the old atan(-1.0) gave -pi/4, not pi, so synthetic beams were actually built at -phi/4 (the azimuth formula shared the same error and the assertion falsely passed)
	const double cp = std::cos(phi), sp = std::sin(phi);
	std::mt19937 rng(seed);
	std::normal_distribution<double> noise(0.0, 3.0);
	for (int y = 0; y < h; ++y) {
		double dy = y - cy;
		for (int x = 0; x < w; ++x) {
			double dx = x - cx;
			double u = dx * cp + dy * sp;    // projection onto the major axis
			double v = -dx * sp + dy * cp;   // projection onto the minor axis
			double g = amp * std::exp(-(u * u / (2 * sMajor * sMajor) + v * v / (2 * sMinor * sMinor)));
			double v2 = g + bg + noise(rng);
			if (v2 < 0) v2 = 0;
			if (v2 > 65535) v2 = 65535;
			f.px[(size_t)y * w + x] = (uint16_t)(v2 + 0.5);
		}
	}
	return f;
}

static bool nearRel(double got, double want, double tol)
{
	return std::fabs(got - want) <= tol * std::fabs(want);
}

int runSelftest()
{
	int fails = 0;
	auto check = [&](const char* name, bool ok, double got, double want) {
		printf("%s: %s (got=%.4f want=%.4f)\n", name, ok ? "PASS" : "FAIL", got, want);
		if (!ok) ++fails;
	};

	// Case 1: sigma_x=6 sigma_y=12 centered at (64.3,63.7), background 100, full-frame ROI
	Frame f = makeGauss(128, 128, 64.3, 63.7, 6.0, 12.0, 20000.0, 100.0, 42);
	AnalysisResult r = analyzeBeam(f, QRect(), 100.0);
	check("valid", r.valid ? true : false, 0, 0);
	check("centroid_x", std::fabs(r.cx - 64.3) < 0.5, r.cx, 64.3);
	check("centroid_y", std::fabs(r.cy - 63.7) < 0.5, r.cy, 63.7);
	check("d4sigma_x", nearRel(r.d4sigmaX, 24.0, 0.05), r.d4sigmaX, 24.0);
	check("d4sigma_y", nearRel(r.d4sigmaY, 48.0, 0.05), r.d4sigmaY, 48.0);
	check("ellipticity", nearRel(r.ellipticity, 0.5, 0.08), r.ellipticity, 0.5);
	check("fwhm_x", nearRel(r.fwhmX, 2.3548 * 6.0, 0.05), r.fwhmX, 2.3548 * 6.0);
	// 84% enclosed-energy width (standard first-dark-ring energy): Gaussian 1D marginal = 2*sqrt(2)*erfinv(0.84)*sigma ~= 2.8145*sigma
	check("d84_x", nearRel(r.d84X, 2.8145 * 6.0, 0.05), r.d84X, 2.8145 * 6.0);
	check("d84_y", nearRel(r.d84Y, 2.8145 * 12.0, 0.05), r.d84Y, 2.8145 * 12.0);
	// analytic power: 2*pi*amp*sigma_x*sigma_y = 2*pi*20000*6*12 ~= 9047787
	check("power", nearRel(r.power, 2.0 * 3.14159265358979 * 20000.0 * 6.0 * 12.0, 0.03),
	      r.power, 2.0 * 3.14159265358979 * 20000.0 * 6.0 * 12.0);
	check("profile_sizes", (r.profileX.size() == 128 && r.profileY.size() == 128) ? 1 : 0, 1, 1);
	check("peak_near_center", (std::abs(r.peakX - 64) <= 1 && std::abs(r.peakY - 64) <= 1) ? 1 : 0, 1, 1);

	// Case 2: ROI limited to a concentric 64x64 window; beam widths should stay essentially unchanged (the beam fits entirely inside)
	AnalysisResult r2 = analyzeBeam(f, QRect(32, 32, 64, 64), 100.0);
	check("roi_d4sigma_x", nearRel(r2.d4sigmaX, 24.0, 0.08), r2.d4sigmaX, 24.0);
	check("roi_d4sigma_y", nearRel(r2.d4sigmaY, 48.0, 0.10), r2.d4sigmaY, 48.0);

	// Case 3: bad background input (background above signal) -> no valid power; must report invalid, not crash
	AnalysisResult r3 = analyzeBeam(f, QRect(), 70000.0);
	check("dead_bg_invalid", r3.valid ? 0 : 1, 0, 0);

	// Case 4: tight beam + frame-wide diffuse glow (reproduces measured laser stray light) -> the
	// correct usage is subtract-the-glow-background first, then threshold-gate.
	// sigma=10 -> true D4sigma 40; after subtracting bg=768 the 5% threshold clips tails and
	// underestimates sigma by ~8%, so the tolerance is 12%.
	// This case also pins the semantics: the threshold applies to background-subtracted values;
	// gating without subtraction lifts the offset into the mask and inflates sigma instead.
	Frame g = makeGauss(256, 256, 128.0, 127.0, 10.0, 10.0, 16000.0, 0.0, 7);
	for (auto& v : g.px) { v = (uint16_t)(v + 768); }
	AnalysisResult gNo = analyzeBeam(g, QRect(), 0.0, 0.0);
	AnalysisResult gTh = analyzeBeam(g, QRect(), 768.0, 5.0);
	check("glare_nothr_inflated", gNo.d4sigmaX > 60.0 ? 1 : 0, gNo.d4sigmaX, 40.0);   // ungated, the glow necessarily inflates it
	check("glare_thr_recovers", nearRel(gTh.d4sigmaX, 40.0, 0.12), gTh.d4sigmaX, 40.0);
	check("glare_centroid", std::fabs(gTh.cx - 128.0) < 1.0 && std::fabs(gTh.cy - 127.0) < 1.0, gTh.cx, 128.0);

	// Case 5: automatic background (bg<0 = ROI median) -- no manual entry needed under glow drift
	// (measured 768 -> 1280). When the beam covers <1% of the pixels the median approximates the
	// background level, so the result should match a manual 768.
	AnalysisResult gAu = analyzeBeam(g, QRect(), -1.0, 5.0);
	check("autobg_used", std::fabs(gAu.backgroundUsed - 768.0) < 20.0, gAu.backgroundUsed, 768.0);
	check("autobg_d4sigma", nearRel(gAu.d4sigmaX, 40.0, 0.12), gAu.d4sigmaX, 40.0);
	check("autobg_centroid", std::fabs(gAu.cx - 128.0) < 1.0 && std::fabs(gAu.cy - 127.0) < 1.0, gAu.cx, 128.0);

	// Case 6: orientation recovery for a rotated elliptical Gaussian (major sigma=12 along phi, minor sigma=6, image y-down convention), tolerance +/-2 degrees
	Frame rot1 = makeGaussRot(128, 128, 64.0, 64.0, 6.0, 12.0, 30.0, 20000.0, 100.0, 11);
	AnalysisResult o1 = analyzeBeam(rot1, QRect(), 100.0);
	check("orient_30", std::fabs(o1.orientationDeg - 30.0) < 2.0, o1.orientationDeg, 30.0);
	check("orient_ellip", nearRel(o1.ellipticity, 0.5, 0.08), o1.ellipticity, 0.5);
	Frame rot2 = makeGaussRot(128, 128, 64.0, 64.0, 6.0, 12.0, -60.0, 20000.0, 100.0, 12);
	AnalysisResult o2 = analyzeBeam(rot2, QRect(), 100.0);
	check("orient_neg60", std::fabs(o2.orientationDeg - (-60.0)) < 2.0, o2.orientationDeg, -60.0);

	// Case 7: beam rotation -- after rotating a 30-degree elongated elliptical Gaussian by alpha = 90 - theta,
	// the major axis should land on +Y: d4sigmaY > d4sigmaX, and the width magnitudes must not be
	// distorted by resampling (+/-5%); the inscribed-rectangle crop shrinks the sensor (w/h both <128).
	{
		AnalysisResult a0 = analyzeBeam(rot1, QRect(), 100.0);
		double alpha = 90.0 - a0.orientationDeg;
		if (alpha > 90.0) alpha -= 180.0;
		if (alpha <= -90.0) alpha += 180.0;
		Frame rot1r = rotateFrameBilinear(rot1, alpha);
		// 60-degree inscribed ratio = 1/(cos60+sin60) ~= 0.732 -> 128*0.732 ~= 93 (theoretical value)
		check("rot_crop", (rot1r.w < 128 && rot1r.h < 128 && rot1r.w >= 90 && rot1r.h >= 90) ? 1 : 0,
		      rot1r.w, 93.0);
		AnalysisResult rr = analyzeBeam(rot1r, QRect(), 100.0);
		check("rot_longaxis_vertical", rr.d4sigmaY > rr.d4sigmaX * 1.3, rr.d4sigmaY / rr.d4sigmaX, 2.0);
		check("rot_d4sigmaY_kept", nearRel(rr.d4sigmaY, 48.0, 0.05), rr.d4sigmaY, 48.0);
		check("rot_d4sigmaX_kept", nearRel(rr.d4sigmaX, 24.0, 0.05), rr.d4sigmaX, 24.0);
	}

	// Case 7b: inverse position transform after rotation (root fix for the beam-tracking "slanted
	// line" bug) -- for a beam whose centroid is off-center, rotating sweeps the centroid along an
	// arc on the rotated frame (old bug: the tracking trace drifted with the angle); unrotatePos
	// must recover the true unrotated-frame centroid. Case 7's centroid sat exactly at the frame
	// center (64,64) so it never exposed this -- a regression case must offset the centroid to test
	// position transforms.
	{
		Frame rot3 = makeGaussRot(128, 128, 72.0, 58.0, 3.0, 6.0, 30.0, 20000.0, 100.0, 13);
		Frame rot3r = rotateFrameBilinear(rot3, 40.0);
		AnalysisResult rr3 = analyzeBeam(rot3r, QRect(), 100.0);
		check("rot_cx_swept", std::fabs(rr3.cx - 72.0) > 2.0, rr3.cx, 72.0);   // pins the mechanism: the rotated-frame centroid really is swept away
		double ux = rr3.cx, uy = rr3.cy;
		unrotatePos(rot3, rot3r, 40.0, ux, uy);
		check("unrot_cx", std::fabs(ux - 72.0) < 0.5, ux, 72.0);
		check("unrot_cy", std::fabs(uy - 58.0) < 0.5, uy, 58.0);
	}

	// Case 8: halo-dominated regression -- a 30-degree elliptical core plus a near-circular large halo
	// tilted -60 degrees: at a low threshold (10%) the halo enters the mask and drags the
	// second-moment principal axis -- the root cause of the real-hardware "rotated twice as far as
	// expected" observation (measured: 10% gate gave a spurious 23-degree axis, 20-90% converged to
	// the 71-77-degree core axis). After switching auto angle detection to a 50%-of-peak gate the
	// halo is fully excluded and the core's major axis is recovered. Two assertions pin the semantics.
	{
		Frame core8 = makeGaussRot(128, 128, 64.0, 64.0, 6.0, 12.0, 30.0, 20000.0, 0.0, 21);
		Frame halo8 = makeGaussRot(128, 128, 64.0, 64.0, 34.0, 40.0, -60.0, 5000.0, 100.0, 31);
		for (size_t i = 0; i < core8.px.size(); ++i) {
			int v = core8.px[i] + halo8.px[i];
			halo8.px[i] = (uint16_t)(v > 65535 ? 65535 : v);
		}
		AnalysisResult h50 = analyzeBeam(halo8, QRect(), 100.0, 50.0);
		check("halo50_orient_core", std::fabs(h50.orientationDeg - 30.0) < 3.0, h50.orientationDeg, 30.0);
		AnalysisResult h10 = analyzeBeam(halo8, QRect(), 100.0, 10.0);
		check("halo10_orient_drift", std::fabs(h10.orientationDeg - 30.0) > 10.0, h10.orientationDeg, 30.0);   // pins the halo-dominated mechanism: low thresholds must not be used for auto leveling
	}

	// Case 9: sensor coordinate conversion -- frame coordinates (top-left origin) -> full-sensor
	// coordinates (bottom-left origin, y up): x translated by the window origin, y flipped about the
	// sensor's bottom edge; an uncropped frame degenerates to a flip about its own height.
	{
		Frame tf; tf.w = 64; tf.h = 48; tf.sensorW = kSensorW; tf.sensorH = kSensorH;
		tf.originX = 100; tf.originY = 200;
		double xb = 0, yb = 0;
		frameToTarget(tf, 10.0, 5.0, xb, yb);
		check("target_x_offset", std::fabs(xb - 110.0) < 1e-9, xb, 110.0);
		check("target_y_flip", std::fabs(yb - ((kSensorH - 1) - 205.0)) < 1e-9, yb, (double)((kSensorH - 1) - 205));   // (1024-1)-(200+5)
		Frame uf; uf.w = 64; uf.h = 48;   // uncropped: sensorH=0 falls back to frame height, origin 0
		frameToTarget(uf, 10.0, 5.0, xb, yb);
		check("target_identity", (std::fabs(xb - 10.0) < 1e-9 && std::fabs(yb - 42.0) < 1e-9) ? 1 : 0, yb, 42.0);
	}

	printf(fails == 0 ? "status=ok\n" : "status=fail\n");
	return fails == 0 ? 0 : 1;
}
