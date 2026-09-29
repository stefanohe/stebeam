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

// Simulated beam source: synthetic Gaussian spot (optional drift/noise) so the whole pipeline can
// be self-tested without hardware.
// Root cause of "Live stuck at 2-4Hz while CMOS runs 30Hz smoothly": the old tick ran on the GUI
// thread, and a full-frame Gaussian + Gaussian noise cost ~100ms per frame exclusively -- the frame
// rate could not physically rise and dragging/clicking all jammed. Now it shares the camera
// source's architecture: frames are produced on a worker thread (m_worker affinity anchor + timer
// living in the worker, DirectConnection makes tick execute there), and frameReady is posted to the
// GUI via a queued connection. Parameter setters still write directly from the GUI (the worker's
// read is a benign race -- worst case one frame mixes parameters, harmless for a simulator).
#pragma once
#include "camera_source.h"
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <algorithm>

class SimulatedSource : public CameraSource
{
	Q_OBJECT
public:
	explicit SimulatedSource(QObject* parent = nullptr);
	~SimulatedSource() override;   // quit+wait to stop the thread (destroying a running QThread member would terminate it; stop explicitly)

	bool start() override;
	void stop() override;
	bool setExposureUs(double us) override;    // simulated: exposure scales amplitude + background (frame values really change, matching real hardware)
	bool setGainDb(double db) override;        // simulated: gain scales amplitude only
	bool setFpsHz(double hz) override;         // simulated: changes the timer interval (the live fps readout follows, exercising the whole fps chain)
	bool setRoi(int w, int h, int xc, int ycBottom) override;   // simulated: generates only the window region (verifies the sensor-cropping chain)
	double exposureUs() const override { return m_expUs; }   // readback = last value sent (a simulated device takes effect trivially)
	QString describe() const override { return QString("模拟源 %1x%2@%3fps").arg(m_roiW).arg(m_roiH).arg(1000 / std::max(1, m_timer.interval())); }   // fps follows setFpsHz; size = current sensor window

	void setSigma(double sx, double sy);       // beam width parameters (px)
	void setDrift(bool on);                    // sinusoidal drift on/off

private slots:
	void tick();

private:
	QThread m_thread;              // frame-producing thread (event loop resident since construction, negligible idle cost)
	QObject* m_worker = nullptr;   // thread-affinity anchor (the timer lives in the worker with it; finished is wired to deleteLater)
	QTimer m_timer;
	QElapsedTimer m_clock;
	double m_sx = 25.0, m_sy = 18.0;
	double m_expScale = 1.0, m_gainScale = 1.0;
	double m_expUs = 10000.0;
	bool m_drift = true;
	// sensor cropping: window size + center (bottom-left origin, y up), defaults to the full sensor
	int m_roiW = kSensorW, m_roiH = kSensorH, m_roiXc = kSensorW / 2, m_roiYc = kSensorH / 2;
};
