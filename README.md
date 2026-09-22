# SteBeam

English · [中文](README-zh.md)

## — A gift to everyone in optics

<p align="center">
  <img src="images/stebeam-logo.gif" alt="SteBeam animated logo">
</p>

**An ordinary industrial camera + SteBeam = a professional beam profiler.**

---

## Why we built it

Measuring laser beam quality and analyzing how light is distributed across a spot have long depended on expensive commercial beam profilers. But most of the time we are not producing certified standard data — the capabilities we need are already within reach of an ordinary industrial camera; what it lacks is proper measurement and analysis software.

SteBeam is a powerful beam-analysis application, developed under the supervision of a senior optical engineer — AI-driven automated debugging and iteration, with human-machine cross-checking at every step. One honest note: absolute power is derived from the spectral-response (QE) curve in the camera's datasheet; the infrared band beyond that curve is a fitted extrapolation and has never been calibrated against a reference, so power readings cannot be guaranteed accurate. If you can measure actual power in your setup, fine-tune the reading with the Calibration Coefficient. We do not claim to replace professional commercial instruments. What we do aim for is: **to let a 100-dollar-class industrial camera deliver laboratory-grade beam analysis.**

## Feature summary

1. **A standards-based measurement core**
   D4σ beam widths per ISO 11146 second moments, plus 84% encircled-energy (stray-light robust) and FWHM diameter definitions, switchable in one click. Ellipticity, major-axis orientation, centroid, peak and power come out on every frame — all on one consistent coordinate convention, so the screen and your exported data always agree.

2. **Five live views, plus frame replay**
   2D pseudo-color intensity map, mouse-rotatable 3D surface, X/Y line profiles, diameter-over-time sliding windows and centroid drift tracking — shape and dynamics through one window. A built-in 100-frame replay buffer freezes acquisition on demand so you can scrub back through the recent history of your beam.

3. **Pure-software rendering, a steady 40+ fps**
   The 3D surface is drawn pixel by pixel by a custom rasterizer — no graphics-hardware requirements, integrated graphics included. At 500 px × 500 px the pure-software rendering holds a frame rate steadily above 40 fps, and every frame still gets the complete measurement set; switch a view away and back, and not one frame is missing — smoothness isn't tuned, it's architected.

4. **Beam leveling**
   One click rotates an elliptical spot's major axis to vertical, with live, one-shot and custom-angle modes; position readouts map back to real sensor coordinates automatically — clean comparisons, reports and batch analysis.

5. **Automatic background subtraction**
   With the optional shutter accessory, one click runs "close shutter → sample dark frame → subtract → reopen"; without a shutter, a manual occlusion path keeps every feature intact.

6. **Your data stays yours**
   Live CSV export of every measurement, one-click frame saving, and one-click RAW output of the original pixels for third-party analysis; a built-in photometric calibration chain lets a single external power-meter reading calibrate absolute power. Your data is stored automatically and kept permanently.

7. **The centroid wander map**
   The centroid-tracking view is my favorite feature: the spot's centroid traces a strikingly random walk across the screen, like a random-number generator running right in front of you — great fun.

## Interface gallery

Main window (measurement readouts):

![Main window](<images/en-main-window.png>)

3D beam surface, rotated by mouse drag:

![3D surface](<images/en-3d-surface.png>)

2D pseudo-color intensity map (rotation-corrected):

![2D pseudo-color](<images/en-2d-intensity.png>)

Centroid tracking:

![Centroid tracking](<images/en-centroid-track.png>)

## Who it's for

- **University & research labs**;
- **Laser education**;
- **Production & integration**;
- **Custom instrument builders**.

## Supported hardware

V0.1 Preview is developed and calibrated around the Hikrobot MV-CU013-A0UM industrial area-scan camera and the Daheng GCI-7103M shutter driver box, and currently supports these two devices. A generic interface for more cameras is under development.

## Get it

- **Preview trial**: free to download, free to forward to colleagues and classmates;
  <!-- TODO: add cloud download link -->
- **User manual**: [PDF (English)](<manual/SteBeam User Manual.pdf>) · [PDF (Chinese)](<manual/SteBeam 用户手册.pdf>) · [HTML (bilingual)](manual/manual.html);
- **Open-source edition**: coming soon under LGPL-3.0 — repository entry at [Gitee](https://gitee.com/stkunlun) / [GitHub](https://github.com/stefanohe).

Trial terms in the bundled EULA.

---

**SteBeam** · Stefano's AI Lab
