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

// Frame-source abstraction: the simulated source and the real camera source share one interface.
#pragma once
#include "frame.h"
#include <QMetaType>
#include <QObject>

class CameraSource : public QObject
{
	Q_OBJECT
public:
	explicit CameraSource(QObject* parent = nullptr) : QObject(parent) {}
	virtual ~CameraSource() = default;

	virtual bool start() = 0;
	virtual void stop() = 0;
	virtual bool setExposureUs(double us) = 0;
	virtual bool setGainDb(double db) = 0;
	// Frame-rate setting (Hz): the camera source writes the GenICam frame-rate nodes, the simulated
	// source changes its timer interval; returns false when not grabbing / unsupported
	virtual bool setFpsHz(double hz) { Q_UNUSED(hz); return true; }
	// Sensor cropping (reduce data volume while grabbing -- smaller ROI, higher frame rate):
	// acquisition window w x h centered at (xc, ycBottom) in full-sensor bottom-left origin
	// (y up, the convention used across the whole app). The camera side writes GenICam
	// Width/Height/OffsetX/OffsetY (while grabbing: stop -> write -> restart); the simulated source
	// only generates the window region. Returns false when not grabbing / unsupported.
	virtual bool setRoi(int w, int h, int xc, int ycBottom)
	{ Q_UNUSED(w); Q_UNUSED(h); Q_UNUSED(xc); Q_UNUSED(ycBottom); return true; }
	// Exposure readback (real-effect success criterion): camera side = GetFloatValue of the actual node value, <0 = read failed / not grabbing
	virtual double exposureUs() const { return -1.0; }
	virtual QString describe() const = 0;
	// Backpressure receipt: MainWindow calls this after consuming a frame; high-rate sources use it
	// to drop frames while the GUI is busy (otherwise queued signals pile up without bound and
	// latency/memory grow linearly over time -- the root cause of real-hardware stutter).
	virtual void frameConsumed() {}

signals:
	void frameReady(const Frame& f);
	void statusText(const QString& s);
	// Camera connection state (drives the green connected badge on the toolbar):
	// the camera source emits true when StartGrabbing succeeds and false on disconnect / thread exit;
	// the simulated source never emits (the main window greys the badge out by source identity).
	void connectionChanged(bool connected);
};

Q_DECLARE_METATYPE(Frame)   // needed for queued cross-thread signal delivery
