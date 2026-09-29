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

// Headless camera capture probe: grab real frames -> analyze each -> save the last frame as raw/png,
// driving the laser-shutter co-debugging loop to completion automatically.
// Usage: SteBeam.exe --capture [--exp us] [--gain db] [--bg n] [--thr %] [--dark raw] [--frames N] [--deadline ms] [--out prefix] [--expmid us] [--gainmid db]
#include "beam_analyzer.h"
#include "camera_source.h"
#include "mvs_source.h"
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int runCapture(int argc, char** argv)
{
	double exp = -1, gain = -1, bg = 0.0, thr = 0.0, expmid = -1, gainmid = -1;
	int framesWant = 20, deadlineMs = 15000, cyclesWant = 1;
	int rx = 0, ry = 0, rw = 0, rh = 0;
	const char* outPrefix = nullptr;
	const char* darkPath = nullptr;
	for (int i = 2; i < argc; ++i) {
		if      (!strcmp(argv[i], "--exp")      && i + 1 < argc) exp = atof(argv[++i]);
		else if (!strcmp(argv[i], "--gain")     && i + 1 < argc) gain = atof(argv[++i]);
		else if (!strcmp(argv[i], "--bg")       && i + 1 < argc) {
			++i;
			bg = !strcmp(argv[i], "auto") ? -1.0 : atof(argv[i]);
		}
		else if (!strcmp(argv[i], "--thr")      && i + 1 < argc) thr = atof(argv[++i]);
		else if (!strcmp(argv[i], "--dark")     && i + 1 < argc) darkPath = argv[++i];
		else if (!strcmp(argv[i], "--frames")   && i + 1 < argc) framesWant = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--deadline") && i + 1 < argc) deadlineMs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cycles")   && i + 1 < argc) cyclesWant = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out")      && i + 1 < argc) outPrefix = argv[++i];
		else if (!strcmp(argv[i], "--roi")      && i + 4 < argc)
			{ rx = atoi(argv[++i]); ry = atoi(argv[++i]); rw = atoi(argv[++i]); rh = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--expmid")   && i + 1 < argc) expmid = atof(argv[++i]);   // change exposure mid-stream (consumed at the worker-loop checkpoint, reproducing the GUI "Apply" path)
		else if (!strcmp(argv[i], "--gainmid")  && i + 1 < argc) gainmid = atof(argv[++i]);  // gain-only isolation probe (tests whether mid-stream stream death is specific to the ExposureTime node)
	}

	QCoreApplication app(argc, argv);
	qRegisterMetaType<Frame>("Frame");

	std::vector<Frame> got;
	MvsSource src;
	QObject::connect(&src, &CameraSource::frameReady, [&](const Frame& f) {
		src.frameConsumed();   // backpressure receipt: release one frame per frame received (missing receipts make the worker drop frames -- once wedged at captured=1)
		got.push_back(f);
		if (expmid >= 0 && (int)got.size() == 3) {   // mid-stream parameter change: goes through the worker-loop checkpoint (same path as the GUI "Apply" button)
			printf("[probe] mid-stream setExposureUs(%g) ret=%d\n", expmid, src.setExposureUs(expmid) ? 1 : 0); fflush(stdout);
		}
		if (expmid >= 0 && (int)got.size() == 6) {   // mid-stream gain probe (+6dB should roughly double the frame mean)
			printf("[probe] mid-stream setGainDb(6) ret=%d\n", src.setGainDb(6.0) ? 1 : 0); fflush(stdout);
		}
		if (expmid >= 0 && (int)got.size() == 9) {   // restore gain to 0dB (camera parameters persist across processes, so reset after probing)
			printf("[probe] mid-stream setGainDb(0) ret=%d\n", src.setGainDb(0.0) ? 1 : 0); fflush(stdout);
			expmid = -1;
		}
		if (gainmid >= 0 && (int)got.size() == 3) {   // write Gain only, leave ExposureTime alone -- isolates whether mid-stream death is node-specific
			printf("[probe] mid-stream setGainDb(%g) ret=%d\n", gainmid, src.setGainDb(gainmid) ? 1 : 0); fflush(stdout);
		}
		if (gainmid >= 0 && (int)got.size() == 9) {   // reset gain (camera parameters persist across processes)
			printf("[probe] mid-stream setGainDb(0) ret=%d\n", src.setGainDb(0.0) ? 1 : 0); fflush(stdout);
			gainmid = -1;
		}
		if ((int)got.size() >= framesWant)
			QCoreApplication::quit();
	});
	QObject::connect(&src, &CameraSource::statusText, [](const QString& s) {
		printf("[cam] %s\n", s.toUtf8().constData());
	});
	// Multi-cycle start/stop: repeatedly switch grabbing within one process, reproducing the "switch
	// sources away and back" path (worker lifecycle regression).
	for (int cyc = 0; cyc < cyclesWant; ++cyc) {
		got.clear();
		QTimer timer;                       // per-cycle scope: destroyed at iteration end so a leftover timer can't kill the next cycle
		timer.setSingleShot(true);
		QObject::connect(&timer, &QTimer::timeout, &app, &QCoreApplication::quit);
		timer.start(deadlineMs);
		src.start();
		if (exp >= 0)  src.setExposureUs(exp);
		if (gain >= 0) src.setGainDb(gain);
		app.exec();
		src.stop();
		printf("cycle %d: captured=%d\n", cyc, (int)got.size());
	}

	printf("captured=%d\n", (int)got.size());
	if (got.empty()) { printf("status=fail\n"); return 1; }

	// Per-pixel dark-frame subtraction (black flat field): the --dark raw must share this capture's exposure/gain
	std::vector<uint16_t> darkPx;
	if (darkPath) {
		QFile df(darkPath);
		if (df.open(QIODevice::ReadOnly)) {
			QByteArray b = df.readAll();
			darkPx.resize((size_t)b.size() / 2);
			memcpy(darkPx.data(), b.constData(), darkPx.size() * 2);
			printf("dark=%s (%zu px)\n", darkPath, darkPx.size());
		} else printf("dark-open-fail=%s\n", darkPath);
	}
	auto subtractDark = [&](const Frame& f) -> Frame {
		if (darkPx.size() != f.px.size()) return f;
		Frame c = f;
		for (size_t j = 0; j < c.px.size(); ++j) {
			int v = (int)f.px[j] - (int)darkPx[j];
			c.px[j] = v > 0 ? (uint16_t)v : 0;
		}
		return c;
	};

	QRect roiRect = (rw > 0 && rh > 0) ? QRect(rx, ry, rw, rh) : QRect();
	std::vector<Frame> corr;
	for (size_t k = 0; k < got.size(); ++k) {
		const Frame& f = got[k];
		Frame c = subtractDark(f);
		AnalysisResult r = analyzeBeam(c, roiRect, bg, thr);
		uint64_t sum = 0;
		size_t sat = 0, darkCnt = 0;
		int mx = 0;
		for (uint16_t v : c.px) {          // stats run on the dark-subtracted frame; saturation is the one exception, checked on the raw frame
			sum += v;
			if (v <= 16)    ++darkCnt;     // on the 12-bit scale this equals the old 16-bit threshold of 256
			if (v > mx) mx = v;
		}
		for (uint16_t v : f.px)
			if (v >= 4095) ++sat;          // full scale = 12-bit ADU 4095
		printf("frame %zu: %dx%d mean=%.1f max=%d sat=%zu dark%%=%.1f | valid=%d bg=%.0f power=%.0f "
		       "cx=%.1f cy=%.1f D4sx=%.1f D4sy=%.1f D84x=%.1f D84y=%.1f ellip=%.3f peak=%d@(%d,%d)\n",
		       k, f.w, f.h, (double)sum / c.px.size(), mx, sat,
		       100.0 * darkCnt / c.px.size(),
		       r.valid ? 1 : 0, r.backgroundUsed, r.power, r.cx, r.cy, r.d4sigmaX, r.d4sigmaY,
		       r.d84X, r.d84Y,
		       r.ellipticity, r.peak, r.peakX, r.peakY);
		corr.push_back(std::move(c));
	}

	if (outPrefix) {
		const Frame& f = corr.back();     // save the dark-corrected frame
		char fn[512];
		snprintf(fn, sizeof(fn), "%s.raw", outPrefix);
		QFile fo(fn);
		if (fo.open(QIODevice::WriteOnly)) {
			fo.write((const char*)f.px.data(), (qint64)f.px.size() * 2);
			printf("saved %s\n", fn);
		} else printf("save-fail %s\n", fn);
		snprintf(fn, sizeof(fn), "%s.png", outPrefix);
		QImage img(f.w, f.h, QImage::Format_Grayscale16);
		for (int y = 0; y < f.h; ++y)
			memcpy(img.scanLine(y), &f.px[(size_t)y * f.w], f.w * sizeof(uint16_t));
		printf(img.save(fn) ? "saved %s\n" : "save-fail %s\n", fn);
	}
	printf("status=ok\n");
	return 0;
}
