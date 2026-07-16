/* plotter_view.h — one page accessor for the UI/IPC/SVG surfaces, whichever
 * plotter is live (beads-5q4v milestone C).
 *
 * Under the sub-cycle engine with the serial pair plugged, the page lives in
 * the plotter Device (plotter_hp7470a_segments — hw truth); under engine=0
 * the legacy backend feeds g_plotter directly. Callers read the page through
 * these functions and never pick a source themselves. Device segments are
 * converted into the legacy PlotSegment shape once per page revision (a
 * render-loop cache, per the review themes). */

#pragma once

#include <string>
#include <vector>

#include "plotter.h"

// The live page: the Device's (converted, cached by page_rev) when the
// bridge machine runs with the plotter plugged, else g_plotter's.
const std::vector<PlotSegment>& plotter_view_segments();

// Carriage/pen status from the same source.
struct PlotterViewStatus {
  int pen = 0;
  bool pen_down = false;
  float x = 0, y = 0;
};
PlotterViewStatus plotter_view_status();

// Tear the sheet off (clears whichever store is live).
void plotter_view_clear();

// Export the live page as SVG.
bool plotter_view_export_svg(const std::string& path);
