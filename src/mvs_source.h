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

// Hikrobot MVS camera source: loads MvCameraControl.dll at runtime (no import library, hence
// toolchain-agnostic); a dedicated grabbing thread does GetImageBuffer -> convert to 16-bit ->
// frameReady.
#pragma once
#include "camera_source.h"
#include <QThread>

class MvsWorker;

class MvsSource : public CameraSource
{
	Q_OBJECT
public:
	explicit MvsSource(QObject* parent = nullptr);
	~MvsSource() override;

	bool start() override;
	void stop() override;
	bool setExposureUs(double us) override;
	bool setGainDb(double db) override;
	bool setFpsHz(double hz) override;   // writes AcquisitionFrameRateEnable+AcquisitionFrameRate (write-only while grabbing, same rule as exposure)
	bool setRoi(int w, int h, int xc, int ycBottom) override;   // writes Width/Height/OffsetX/OffsetY (center given in bottom-left origin, converted to top-left inside the worker; while grabbing: stop -> write -> restart)
	double exposureUs() const override;   // defined in .cpp (needs the complete MvsWorker type): live value from the worker while grabbing, session-end cache after stop
	void frameConsumed() override;
	QString describe() const override { return "CMOS (MVS C API)"; }   // UI does not expose the hardware brand

private:
	MvsWorker* m_worker = nullptr;
	QThread m_thread;
	double m_expReadback = -1.0;   // cached exposure-node readback (the real-effect success criterion)
};
