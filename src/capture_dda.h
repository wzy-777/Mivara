/**
 * CAPTURE_DDA.H – Windows desktop capture through DXGI Desktop Duplication.
 *
 * The GDI path (GetDC(NULL) + BitBlt) reads back the composed desktop through
 * the display driver and is quantised to the compositor cadence: on a 60 Hz
 * desktop a 640x480 readback and a 1080p readback both cost ~17 ms, and
 * anything larger costs ~33 ms. Desktop Duplication hands over the composed
 * frame as a GPU texture instead, so a capture costs only the staging copy
 * (~2-6 ms) and never waits a refresh interval.
 *
 * The duplicated image does NOT contain the mouse cursor; this module
 * composites it (GetCursorInfo + DrawIconEx) into the returned BGRA frame and
 * reports a new frame when either the desktop changed or the cursor moved.
 * Everything is Windows-only; other platforms compile empty stubs and
 * ddaOpen() returns false, so capture_dda.cpp can stay in the sender build.
 */
#ifndef MIVARA_CAPTURE_DDA_H
#define MIVARA_CAPTURE_DDA_H

#include <cstdint>
#include <string>

/** Opens duplication for the output whose GDI device name is `device_name`
 *  (for example "\\.\DISPLAY3"); nullptr/empty selects the primary output
 *  containing (0, 0). Returns false and writes a one-line reason to *error
 *  when the output cannot be duplicated (missing adapter, unsupported format,
 *  another exclusive duplicator, secure desktop, ...). */
bool ddaOpen(const char *device_name, std::string *error);

/** True between a successful ddaOpen() and ddaClose(). */
bool ddaIsOpen();

/** Waits up to `timeout_ms` for new desktop content or a cursor change.
 *  1 = a frame is ready, 0 = timeout, -1 = duplication lost / mode changed /
 *  no real image within 3 s of opening (empty initial frames are ignored).
 *  The caller must treat -1 as "close and fall back to GDI". The wait is
 *  sliced internally (<= 8 ms) so a moving cursor is reported promptly. */
int ddaWait(int timeout_ms);

/** Latest frame, top-down packed BGRA (cursor already composited).
 *  Valid until the next ddaWait()/ddaClose(). */
const uint8_t *ddaPixels();
int ddaStride();
int ddaWidth();
int ddaHeight();

void ddaClose();

#endif /* MIVARA_CAPTURE_DDA_H */
