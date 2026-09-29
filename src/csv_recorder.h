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

// Realtime CSV recorder: once enabled from the toolbar, each frame's 2D intensity matrix is saved
// into a folder under a timestamp-derived name; can be stopped at any time, with an optional
// "save every Nth frame" interval.
// A full 1280x1024 CSV is ~6 MB of text -- writing it synchronously on the GUI thread would drop
// frames, so a worker on a QThread event loop writes to disk asynchronously.
// Backpressure: while the worker is busy, incoming frames are dropped and counted (never let CSV
// data pile up in memory -- better to drop frames than to stall the UI).
// Filename = the frame's captureMs timestamp.
#pragma once
#include "camera_source.h"   // Q_DECLARE_METATYPE(Frame) (required for queued cross-thread signal delivery)
#include "frame.h"
#include <QMutex>
#include <QThread>
#include <atomic>

class CsvRecorder : public QObject
{
	Q_OBJECT
public:
	CsvRecorder();                       // starts the worker thread (idle event loop, negligible cost)
	~CsvRecorder() override;             // quit+wait until the in-flight file finishes; destroying a running QThread member would terminate it, so stop it explicitly
	void setDir(const QString& dir);     // target folder (default exe\realtime_csv or user-picked; created via mkpath when missing)
	// Current target folder (persisted to / displayed from the ini since storage location is customizable).
	// Race fix: m_dir is written by the GUI thread (setDir) and read by the worker (doWrite), and
	// QString assignment is not atomic -- all three access sites lock m_dirMtx (dir() inline read
	// lock; setDir/doWrite write / copy under the lock in the .cpp).
	QString dir() const { QMutexLocker lk(&m_dirMtx); return m_dir; }
	// Called from the GUI thread: if the worker is busy, count a drop (backpressure); otherwise post
	// a Frame copy to the queue. header = metadata block (6 English lines, precomputed on the GUI
	// thread and delivered with the frame -- the worker has no GUI state).
	void enqueue(const Frame& f, const QString& header);
	int saved() const { return m_saved.load(); }
	int dropped() const { return m_dropped.load(); }

signals:
	void wantWrite(const Frame& f, const QString& header);   // queued -> doWrite on the worker thread

private slots:
	void doWrite(const Frame& f, const QString& header);     // worker thread: write the header block, then one comma-separated pixel row per line

private:
	QThread m_thread;
	std::atomic_bool m_busy{ false };
	std::atomic_int m_saved{ 0 };
	std::atomic_int m_dropped{ 0 };
	QString m_dir;                 // guarded by m_dirMtx (see dir() comment)
	mutable QMutex m_dirMtx;
};
