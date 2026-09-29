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

#include "csv_recorder.h"
#include <QDateTime>
#include <QDir>
#include <QFile>

CsvRecorder::CsvRecorder()
{
	moveToThread(&m_thread);
	connect(this, &CsvRecorder::wantWrite, this, &CsvRecorder::doWrite, Qt::QueuedConnection);
	m_thread.start();
}

CsvRecorder::~CsvRecorder()
{
	m_thread.quit();
	m_thread.wait();   // wait for the event loop to exit; an in-flight doWrite finishes before the loop returns, then quits
}

void CsvRecorder::setDir(const QString& dir)
{
	{ QMutexLocker lk(&m_dirMtx); m_dir = dir; }   // write under the lock (worker's doWrite reads concurrently, see header comment)
	QDir().mkpath(dir);   // create the realtime_csv folder on first enable
}

void CsvRecorder::enqueue(const Frame& f, const QString& header)
{
	if (m_busy.exchange(true)) {   // previous write still in flight: count a drop instead of queueing (backpressure)
		++m_dropped;
		return;
	}
	emit wantWrite(f, header);     // queued delivery: Frame is deep-copied into the event queue
}

// Direct uint16 digit writing (<= 5 digits), avoiding QTextStream per-value formatting overhead
// (a 6 MB file must stay writable even on the worker thread)
static void appendU16(QByteArray& out, uint16_t v)
{
	char buf[5];
	int i = 5;
	do { buf[--i] = char('0' + v % 10); v /= 10; } while (v);
	out.append(buf + i, 5 - i);
}

void CsvRecorder::doWrite(const Frame& f, const QString& header)
{
	const QString name = QDateTime::fromMSecsSinceEpoch(f.captureMs).toString("yyyyMMdd_HHmmss_zzz");
	QString base;
	{ QMutexLocker lk(&m_dirMtx); base = m_dir; }   // copy under the lock, then use locally (the GUI thread may setDir at any time)
	QFile file(base + "/" + (name.isEmpty() ? QString::number(f.captureMs) : name) + ".csv");
	if (file.open(QIODevice::WriteOnly)) {
		if (!header.isEmpty()) file.write(header.toUtf8());   // metadata header block (English, precomputed on the GUI thread)
		QByteArray row;
		row.reserve((qsizetype)f.w * 6);
		for (int y = 0; y < f.h; ++y) {
			row.clear();
			for (int x = 0; x < f.w; ++x) {
				if (x) row += ',';
				appendU16(row, f.px[(size_t)y * f.w + x]);
			}
			row += '\n';
			file.write(row);
		}
		file.close();
		++m_saved;
	}
	m_busy = false;   // release the gate for the next frame (also on open failure, so we never wedge)
}
