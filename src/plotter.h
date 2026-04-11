/* konCePCja — HP-GL Plotter Emulation (HP 7470A via Centronics)
 *
 * Emulates an HP 7470A 2-pen flatbed plotter connected to the CPC's
 * Centronics parallel port.  The GSX driver DDHP7470.PRL sends HP-GL
 * commands which this module parses into drawable primitives and
 * exports as SVG.
 */

#ifndef PLOTTER_H
#define PLOTTER_H

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>

// HP 7470A hard-clip limits (plotter units, A4 landscape)
constexpr int HPGL_MAX_X = 10365;
constexpr int HPGL_MAX_Y = 7962;
constexpr float HPGL_UNITS_PER_MM = 40.0f;

// Drawing primitive types
enum class PlotPrimitive {
    Line,       // from (x1,y1) to (x2,y2)
    Circle,     // center (x1,y1), radius r
    Arc,        // center (x1,y1), start angle, sweep angle, radius
    Label,      // text at (x1,y1)
};

struct PlotSegment {
    PlotPrimitive type;
    int pen;            // 1 or 2
    float x1, y1;       // start or center (plotter units)
    float x2, y2;       // end (for lines)
    float radius;        // for circles/arcs
    float start_angle;   // for arcs (degrees)
    float sweep_angle;   // for arcs (degrees)
    std::string text;    // for labels
    int line_type;       // 0=solid, 1-6=patterns
};

class HpglPlotter {
public:
    HpglPlotter();

    // Feed a byte from the serial data port
    void feed_byte(uint8_t byte);

    // Reset plotter state (IN command)
    void reset();

    // Export accumulated drawing to SVG file
    bool export_svg(const std::string& path) const;

    // Get drawing segments for ImGui preview
    const std::vector<PlotSegment>& segments() const { return segments_; }

    // Clear all segments
    void clear();

    // Is there any output?
    bool has_output() const { return !segments_.empty(); }

    // Get current pen position (for preview)
    float pen_x() const { return pen_x_; }
    float pen_y() const { return pen_y_; }
    bool pen_down() const { return pen_down_; }
    int selected_pen() const { return selected_pen_; }

private:
    // Command parsing
    void process_command(const std::string& cmd);
    void parse_params(const std::string& params, std::vector<float>& out);

    // Command handlers
    void cmd_IN();  // Initialize
    void cmd_SP(const std::vector<float>& p);  // Select Pen
    void cmd_PU(const std::vector<float>& p);  // Pen Up
    void cmd_PD(const std::vector<float>& p);  // Pen Down
    void cmd_PA(const std::vector<float>& p);  // Plot Absolute
    void cmd_PR(const std::vector<float>& p);  // Plot Relative
    void cmd_CI(const std::vector<float>& p);  // Circle
    void cmd_AA(const std::vector<float>& p);  // Arc Absolute
    void cmd_AR(const std::vector<float>& p);  // Arc Relative
    void cmd_EA(const std::vector<float>& p);  // Edge Rectangle Absolute
    void cmd_ER(const std::vector<float>& p);  // Edge Rectangle Relative
    void cmd_LT(const std::vector<float>& p);  // Line Type
    void cmd_LB(const std::string& text);      // Label
    void cmd_DT(const std::string& params);    // Define label Terminator
    void cmd_SI(const std::vector<float>& p);  // Absolute character Size
    void cmd_DI(const std::vector<float>& p);  // label Direction
    void cmd_SC(const std::vector<float>& p);  // Scale
    void cmd_IP(const std::vector<float>& p);  // Input P1/P2
    void cmd_IW(const std::vector<float>& p);  // Input Window
    void cmd_DF();  // Default

    // Coordinate transform (user → plotter units)
    float tx(float x) const;
    float ty(float y) const;

    // Move pen, optionally drawing
    void move_to(float x, float y);

    // State
    float pen_x_ = 0, pen_y_ = 0;
    bool pen_down_ = false;
    int selected_pen_ = 0;    // 0=stowed, 1-2=active
    int line_type_ = -1;      // -1=solid, 0-6=patterns

    // Scaling
    float p1_x_ = 0, p1_y_ = 0;
    float p2_x_ = HPGL_MAX_X, p2_y_ = HPGL_MAX_Y;
    bool scaling_active_ = false;
    float sc_xmin_ = 0, sc_xmax_ = 1, sc_ymin_ = 0, sc_ymax_ = 1;

    // Clipping window
    float win_x1_ = 0, win_y1_ = 0;
    float win_x2_ = HPGL_MAX_X, win_y2_ = HPGL_MAX_Y;

    // Character settings
    float char_width_ = 0.187f;   // cm
    float char_height_ = 0.269f;  // cm
    float char_dir_run_ = 1, char_dir_rise_ = 0;
    float char_slant_ = 0;
    char label_terminator_ = 0x03; // ETX

    // Command buffer
    std::string cmd_buf_;
    bool in_label_ = false;  // inside LB command (waiting for terminator)
    std::string label_buf_;

    // Drawing output
    std::vector<PlotSegment> segments_;
};

// Global plotter instance
extern HpglPlotter g_plotter;

#endif // PLOTTER_H
