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

#include "simulated_source.h"
#include "i18n.h"
#include <QDateTime>
#include <cmath>
#include <random>

SimulatedSource::SimulatedSource(QObject* parent) : CameraSource(parent)
{
	// Threaded frame production (see header comment): the timer moves to the worker thread and is
	// wired with DirectConnection so tick executes there. moveToThread must be called from the
	// object's own current thread (the timer was constructed on the GUI thread -> move it from
	// there; safe because no timerId is active yet); connect is thread-agnostic; the timer interval
	// is set before start on the worker side.
	m_worker = new QObject;
	m_worker->moveToThread(&m_thread);
	m_timer.moveToThread(&m_thread);
	connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);   // the standard worker-teardown idiom from Qt docs
	m_timer.setInterval(100);   // ~10fps default; UI fps settings override immediately via setFpsHz
	QObject::connect(&m_timer, &QTimer::timeout, this, &SimulatedSource::tick, Qt::DirectConnection);
	m_thread.start();
}

SimulatedSource::~SimulatedSource()
{
	m_thread.quit();
	m_thread.wait();
}

bool SimulatedSource::start()
{
	// QTimer thread affinity = worker: calling start from the GUI thread would be a no-op, so post it to the worker's event loop
	QMetaObject::invokeMethod(m_worker, [this] { m_clock.start(); m_timer.start(); }, Qt::QueuedConnection);
	emit statusText(L("模拟源已启动", "Simulated source started"));
	return true;
}

void SimulatedSource::stop()
{
	QMetaObject::invokeMethod(m_worker, [this] { m_timer.stop(); }, Qt::QueuedConnection);
	emit statusText(L("模拟源已停止", "Simulated source stopped"));
}

bool SimulatedSource::setExposureUs(double us)
{
	m_expScale = us / 10000.0;   // equivalent scale with 10ms as 1.0
	m_expUs = us;
	return true;
}

bool SimulatedSource::setGainDb(double db)
{
	m_gainScale = std::pow(10.0, db / 20.0);
	return true;
}

// Frame rate: changing the timer interval takes effect immediately; clamped to 1-240fps so a mistaken 0/negative can't divide by zero or spin
bool SimulatedSource::setFpsHz(double hz)
{
	if (hz < 1.0 || hz > 240.0) return false;
	const int ms = std::max(1, (int)std::lround(1000.0 / hz));
	QMetaObject::invokeMethod(m_worker, [this, ms] { m_timer.setInterval(ms); }, Qt::QueuedConnection);   // the timer lives in the worker, so interval changes are posted there too
	return true;
}

// Sensor cropping: validate the center against the size-coupled bounds, store it; from then on tick generates only the window region (data volume really shrinks, so the fps chain is verifiable)
bool SimulatedSource::setRoi(int w, int h, int xc, int ycBottom)
{
	if (w < 4 || h < 4 || w > kSensorW || h > kSensorH) return false;
	if (xc < w / 2 || xc > kSensorW - w / 2 || ycBottom < h / 2 || ycBottom > kSensorH - h / 2) return false;
	m_roiW = w; m_roiH = h; m_roiXc = xc; m_roiYc = ycBottom;
	return true;
}

void SimulatedSource::setSigma(double sx, double sy)
{
	m_sx = sx; m_sy = sy;
}

void SimulatedSource::setDrift(bool on)
{
	m_drift = on;
}

void SimulatedSource::tick()
{
	// Sensor cropping: the beam is synthesized in full-sensor coordinates (the drift trajectory
	// doesn't jump with the window), and only in-window pixels are generated -- data volume really
	// drops (equivalent to the camera side). originX/originY + sensorW/H let position readouts map
	// back to full-sensor coordinates.
	const int W = kSensorW, H = kSensorH;
	const int w = m_roiW, h = m_roiH;
	const int ox = m_roiXc - w / 2;              // window's top-left corner in full-sensor top-left coordinates
	const int oyTop = H - m_roiYc - h / 2;       // center given in bottom-left origin -> flip y into the top-left system
	Frame f;
	f.w = w; f.h = h;
	f.originX = ox; f.originY = oyTop;
	f.sensorW = W; f.sensorH = H;
	f.px.resize((size_t)w * h);
	f.ts = m_clock.isValid() ? m_clock.elapsed() / 1000.0 : 0.0;
	f.captureMs = QDateTime::currentMSecsSinceEpoch();   // replay timestamps / realtime-CSV naming share the camera source's convention

	// Beam center: frame center by default; with drift enabled it traces an ellipse (full-sensor top-left system)
	double t = f.ts;
	double cx = W / 2.0 + (m_drift ? 40.0 * std::sin(t * 0.7) : 0.0);
	double cy = H / 2.0 + (m_drift ? 30.0 * std::cos(t * 0.5) : 0.0);
	// Pipeline-wide 12-bit ADU scale: frame values must land in 0..4095 -- the old 65535 ceiling was
	// treated as saturation by the 12-bit criteria and desynchronized the analysis-side threshold /
	// saturation percentages (one root cause of "power reads all zero after unit switch").
	double amp = 3600.0 * m_expScale * m_gainScale;
	double bg = 200.0 * m_expScale;   // background (dark current / stray) grows linearly with exposure, matching real hardware

	// Performance (the old formulation cost ~250ms/frame full-frame -> a physical source limit of
	// 4Hz, the other half of the "Live 2-4Hz" root cause):
	// (1) separable Gaussian -- 2D Gaussian = row factor x column factor; exp calls drop from one per
	//     pixel (1.3M) to w+h, mathematically equivalent;
	// (2) noise lookup table -- per-pixel std::normal_distribution (Box-Muller with log/sqrt/trig)
	//     was the other hotspot; replaced by a 65536-entry normal table read with a per-frame
	//     offset start (so noise patterns don't repeat across frames), same distribution (sigma=12).
	static const std::vector<float> kNoise = [] {
		std::vector<float> t(65536);
		std::mt19937 rng(0x5EED);
		std::normal_distribution<float> nd(0.0f, 12.0f);
		for (float& v : t) v = nd(rng);
		return t;
	}();
	static thread_local size_t noisePos = 0;
	noisePos = (noisePos + 77771) & 0xFFFF;   // stagger each frame's start (prime stride)

	std::vector<double> ex((size_t)w), ey((size_t)h);
	for (int x = 0; x < w; ++x) { double dx = (ox + x) - cx; ex[x] = std::exp(-dx * dx / (2.0 * m_sx * m_sx)); }
	for (int y = 0; y < h; ++y) { double dy = (oyTop + y) - cy; ey[y] = std::exp(-dy * dy / (2.0 * m_sy * m_sy)); }

	for (int y = 0; y < h; ++y) {
		const double ay = amp * ey[y];
		uint16_t* row = &f.px[(size_t)y * w];
		const size_t nBase = (noisePos + (size_t)y * 3) & 0xFFFF;   // stagger per row too, so noise doesn't repeat with row periodicity
		for (int x = 0; x < w; ++x) {
			double v = ay * ex[x] + bg + kNoise[(nBase + x) & 0xFFFF];
			if (v < 0) v = 0;
			if (v > 4095) v = 4095;
			row[x] = (uint16_t)(v + 0.5);
		}
	}
	emit frameReady(f);
}
