/* konCePCja — HP-GL Plotter Emulation (HP 7470A via Centronics)
 *
 * Parses HP-GL commands from the CPC's parallel port and renders them
 * as vector graphics.  Exports to SVG.
 */

#include "plotter.h"
#include "log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

HpglPlotter g_plotter;

HpglPlotter::HpglPlotter() {
    reset();
}

void HpglPlotter::reset() {
    pen_x_ = 0; pen_y_ = 0;
    pen_down_ = false;
    selected_pen_ = 0;
    line_type_ = -1;
    p1_x_ = 0; p1_y_ = 0;
    p2_x_ = HPGL_MAX_X; p2_y_ = HPGL_MAX_Y;
    scaling_active_ = false;
    win_x1_ = 0; win_y1_ = 0;
    win_x2_ = HPGL_MAX_X; win_y2_ = HPGL_MAX_Y;
    char_width_ = 0.187f; char_height_ = 0.269f;
    char_dir_run_ = 1; char_dir_rise_ = 0;
    char_slant_ = 0;
    label_terminator_ = 0x03;
    cmd_buf_.clear();
    in_label_ = false;
    label_buf_.clear();
}

void HpglPlotter::clear() {
    segments_.clear();
    reset();
}

void HpglPlotter::feed_byte(uint8_t byte) {
    char c = static_cast<char>(byte);

    // Inside a label: accumulate until terminator
    if (in_label_) {
        if (c == label_terminator_) {
            cmd_LB(label_buf_);
            label_buf_.clear();
            in_label_ = false;
        } else {
            label_buf_ += c;
        }
        return;
    }

    // Accumulate command bytes; semicolon terminates
    if (c == ';') {
        if (!cmd_buf_.empty()) {
            process_command(cmd_buf_);
            cmd_buf_.clear();
        }
    } else if (c >= 0x20 || c == '\r' || c == '\n') {
        // Skip control chars except CR/LF (used in some drivers as whitespace)
        if (c != '\r' && c != '\n')
            cmd_buf_ += c;
    }
}

void HpglPlotter::parse_params(const std::string& params, std::vector<float>& out) {
    out.clear();
    if (params.empty()) return;
    std::istringstream ss(params);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // Trim whitespace
        size_t start = token.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        token = token.substr(start);
        try {
            out.push_back(std::stof(token));
        } catch (const std::invalid_argument&) {
        } catch (const std::out_of_range&) {
        }
    }
}

void HpglPlotter::process_command(const std::string& cmd) {
    if (cmd.size() < 2) return;

    // Extract 2-letter mnemonic (uppercase)
    char m0 = static_cast<char>(toupper(cmd[0]));
    char m1 = static_cast<char>(toupper(cmd[1]));
    std::string params = cmd.substr(2);
    std::vector<float> p;

    if (m0 == 'I' && m1 == 'N') { cmd_IN(); return; }
    if (m0 == 'D' && m1 == 'F') { cmd_DF(); return; }

    // LB is special — the rest of input until terminator is the label text
    if (m0 == 'L' && m1 == 'B') {
        in_label_ = true;
        label_buf_ = params;  // may already contain part of the text
        return;
    }
    if (m0 == 'D' && m1 == 'T') { cmd_DT(params); return; }

    parse_params(params, p);

    if (m0 == 'S' && m1 == 'P') cmd_SP(p);
    else if (m0 == 'P' && m1 == 'U') cmd_PU(p);
    else if (m0 == 'P' && m1 == 'D') cmd_PD(p);
    else if (m0 == 'P' && m1 == 'A') cmd_PA(p);
    else if (m0 == 'P' && m1 == 'R') cmd_PR(p);
    else if (m0 == 'C' && m1 == 'I') cmd_CI(p);
    else if (m0 == 'A' && m1 == 'A') cmd_AA(p);
    else if (m0 == 'A' && m1 == 'R') cmd_AR(p);
    else if (m0 == 'E' && m1 == 'A') cmd_EA(p);
    else if (m0 == 'E' && m1 == 'R') cmd_ER(p);
    else if (m0 == 'L' && m1 == 'T') cmd_LT(p);
    else if (m0 == 'S' && m1 == 'I') cmd_SI(p);
    else if (m0 == 'D' && m1 == 'I') cmd_DI(p);
    else if (m0 == 'S' && m1 == 'C') cmd_SC(p);
    else if (m0 == 'I' && m1 == 'P') cmd_IP(p);
    else if (m0 == 'I' && m1 == 'W') cmd_IW(p);
    // Ignored commands: VS (velocity), PT, SL, SR, DR, CP, ES, TL, XT, YT,
    // FT, RA, RR, and all query commands (OA, OC, OE, OF, OH, OP, OS)
    else {
        LOG_DEBUG("HP-GL: unknown command " << m0 << m1);
    }
}

// ── Coordinate transform ──────────────────────────────────────────────

float HpglPlotter::tx(float x) const {
    if (!scaling_active_) return x;
    float frac = (x - sc_xmin_) / (sc_xmax_ - sc_xmin_);
    return p1_x_ + frac * (p2_x_ - p1_x_);
}

float HpglPlotter::ty(float y) const {
    if (!scaling_active_) return y;
    float frac = (y - sc_ymin_) / (sc_ymax_ - sc_ymin_);
    return p1_y_ + frac * (p2_y_ - p1_y_);
}

void HpglPlotter::move_to(float x, float y) {
    if (pen_down_ && selected_pen_ > 0) {
        PlotSegment seg;
        seg.type = PlotPrimitive::Line;
        seg.pen = selected_pen_;
        seg.x1 = pen_x_; seg.y1 = pen_y_;
        seg.x2 = x; seg.y2 = y;
        seg.line_type = line_type_;
        segments_.push_back(seg);
    }
    pen_x_ = x;
    pen_y_ = y;
}

// ── Command handlers ──────────────────────────────────────────────────

void HpglPlotter::cmd_IN() {
    pen_down_ = false;
    selected_pen_ = 0;
    pen_x_ = 0; pen_y_ = 0;
    line_type_ = -1;
    p1_x_ = 0; p1_y_ = 0;
    p2_x_ = HPGL_MAX_X; p2_y_ = HPGL_MAX_Y;
    scaling_active_ = false;
    win_x1_ = 0; win_y1_ = 0;
    win_x2_ = HPGL_MAX_X; win_y2_ = HPGL_MAX_Y;
    char_width_ = 0.187f; char_height_ = 0.269f;
    char_dir_run_ = 1; char_dir_rise_ = 0;
    char_slant_ = 0;
    label_terminator_ = 0x03;
}

void HpglPlotter::cmd_DF() {
    line_type_ = -1;
    scaling_active_ = false;
    char_width_ = 0.187f; char_height_ = 0.269f;
    char_dir_run_ = 1; char_dir_rise_ = 0;
    char_slant_ = 0;
    label_terminator_ = 0x03;
}

void HpglPlotter::cmd_SP(const std::vector<float>& p) {
    pen_down_ = false;  // pen lifts during change
    selected_pen_ = p.empty() ? 0 : static_cast<int>(p[0]);
    if (selected_pen_ < 0 || selected_pen_ > 2) selected_pen_ = 0;
}

void HpglPlotter::cmd_PU(const std::vector<float>& p) {
    pen_down_ = false;
    // Optional coordinate pairs
    for (size_t i = 0; i + 1 < p.size(); i += 2) {
        pen_x_ = tx(p[i]);
        pen_y_ = ty(p[i + 1]);
    }
}

void HpglPlotter::cmd_PD(const std::vector<float>& p) {
    pen_down_ = true;
    for (size_t i = 0; i + 1 < p.size(); i += 2)
        move_to(tx(p[i]), ty(p[i + 1]));
}

void HpglPlotter::cmd_PA(const std::vector<float>& p) {
    for (size_t i = 0; i + 1 < p.size(); i += 2)
        move_to(tx(p[i]), ty(p[i + 1]));
}

void HpglPlotter::cmd_PR(const std::vector<float>& p) {
    for (size_t i = 0; i + 1 < p.size(); i += 2) {
        float dx = scaling_active_ ? (p[i] / (sc_xmax_ - sc_xmin_)) * (p2_x_ - p1_x_) : p[i];
        float dy = scaling_active_ ? (p[i+1] / (sc_ymax_ - sc_ymin_)) * (p2_y_ - p1_y_) : p[i+1];
        move_to(pen_x_ + dx, pen_y_ + dy);
    }
}

void HpglPlotter::cmd_CI(const std::vector<float>& p) {
    if (p.empty()) return;
    float radius = p[0];
    if (scaling_active_)
        radius = (radius / (sc_xmax_ - sc_xmin_)) * (p2_x_ - p1_x_);
    if (selected_pen_ > 0) {
        PlotSegment seg;
        seg.type = PlotPrimitive::Circle;
        seg.pen = selected_pen_;
        seg.x1 = pen_x_; seg.y1 = pen_y_;
        seg.radius = std::abs(radius);
        seg.line_type = line_type_;
        segments_.push_back(seg);
    }
}

void HpglPlotter::cmd_AA(const std::vector<float>& p) {
    if (p.size() < 3) return;
    float cx = tx(p[0]), cy = ty(p[1]);
    float sweep = p[2];
    float dx = pen_x_ - cx, dy = pen_y_ - cy;
    float radius = std::sqrt(dx * dx + dy * dy);
    float start = std::atan2(dy, dx) * 180.0f / static_cast<float>(M_PI);
    if (selected_pen_ > 0 && radius > 0) {
        PlotSegment seg;
        seg.type = PlotPrimitive::Arc;
        seg.pen = selected_pen_;
        seg.x1 = cx; seg.y1 = cy;
        seg.radius = radius;
        seg.start_angle = start;
        seg.sweep_angle = sweep;
        seg.line_type = line_type_;
        segments_.push_back(seg);
    }
    // Move pen to arc endpoint
    float end_angle = (start + sweep) * static_cast<float>(M_PI) / 180.0f;
    pen_x_ = cx + radius * std::cos(end_angle);
    pen_y_ = cy + radius * std::sin(end_angle);
}

void HpglPlotter::cmd_AR(const std::vector<float>& p) {
    if (p.size() < 3) return;
    // Convert relative center to absolute
    std::vector<float> abs_p = { pen_x_ + p[0], pen_y_ + p[1], p[2] };
    if (p.size() > 3) abs_p.push_back(p[3]);
    cmd_AA(abs_p);
}

void HpglPlotter::cmd_EA(const std::vector<float>& p) {
    if (p.size() < 2) return;
    float x2 = tx(p[0]), y2 = ty(p[1]);
    if (selected_pen_ > 0) {
        float x1 = pen_x_, y1 = pen_y_;
        auto add_line = [&](float ax, float ay, float bx, float by) {
            PlotSegment seg;
            seg.type = PlotPrimitive::Line;
            seg.pen = selected_pen_;
            seg.x1 = ax; seg.y1 = ay;
            seg.x2 = bx; seg.y2 = by;
            seg.line_type = line_type_;
            segments_.push_back(seg);
        };
        add_line(x1, y1, x2, y1);
        add_line(x2, y1, x2, y2);
        add_line(x2, y2, x1, y2);
        add_line(x1, y2, x1, y1);
    }
}

void HpglPlotter::cmd_ER(const std::vector<float>& p) {
    if (p.size() < 2) return;
    std::vector<float> abs_p = { pen_x_ + p[0], pen_y_ + p[1] };
    cmd_EA(abs_p);
}

void HpglPlotter::cmd_LT(const std::vector<float>& p) {
    line_type_ = p.empty() ? -1 : static_cast<int>(p[0]);
}

void HpglPlotter::cmd_LB(const std::string& text) {
    if (text.empty() || selected_pen_ == 0) return;
    PlotSegment seg;
    seg.type = PlotPrimitive::Label;
    seg.pen = selected_pen_;
    seg.x1 = pen_x_; seg.y1 = pen_y_;
    seg.text = text;
    seg.line_type = line_type_;
    segments_.push_back(seg);
    // Advance pen position by text width (approximate)
    float char_w_units = char_width_ * HPGL_UNITS_PER_MM * 10.0f; // cm → mm → units
    pen_x_ += char_w_units * text.size() * char_dir_run_;
    pen_y_ += char_w_units * text.size() * char_dir_rise_;
}

void HpglPlotter::cmd_DT(const std::string& params) {
    if (!params.empty())
        label_terminator_ = params[0];
    else
        label_terminator_ = 0x03;
}

void HpglPlotter::cmd_SI(const std::vector<float>& p) {
    if (p.size() >= 2) {
        char_width_ = p[0];
        char_height_ = p[1];
    }
}

void HpglPlotter::cmd_DI(const std::vector<float>& p) {
    if (p.size() >= 2) {
        char_dir_run_ = p[0];
        char_dir_rise_ = p[1];
    }
}

void HpglPlotter::cmd_SC(const std::vector<float>& p) {
    if (p.size() >= 4) {
        sc_xmin_ = p[0]; sc_xmax_ = p[1];
        sc_ymin_ = p[2]; sc_ymax_ = p[3];
        scaling_active_ = true;
    } else {
        scaling_active_ = false;
    }
}

void HpglPlotter::cmd_IP(const std::vector<float>& p) {
    if (p.size() >= 4) {
        p1_x_ = p[0]; p1_y_ = p[1];
        p2_x_ = p[2]; p2_y_ = p[3];
    } else {
        p1_x_ = 0; p1_y_ = 0;
        p2_x_ = HPGL_MAX_X; p2_y_ = HPGL_MAX_Y;
    }
}

void HpglPlotter::cmd_IW(const std::vector<float>& p) {
    if (p.size() >= 4) {
        win_x1_ = p[0]; win_y1_ = p[1];
        win_x2_ = p[2]; win_y2_ = p[3];
    } else {
        win_x1_ = 0; win_y1_ = 0;
        win_x2_ = HPGL_MAX_X; win_y2_ = HPGL_MAX_Y;
    }
}

// ── SVG Export ────────────────────────────────────────────────────────

bool HpglPlotter::export_svg(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;

    // SVG coordinate system: Y is flipped (HP-GL Y-up → SVG Y-down)
    // Viewbox covers the full plotter area
    float svg_w = static_cast<float>(HPGL_MAX_X) / HPGL_UNITS_PER_MM;  // mm
    float svg_h = static_cast<float>(HPGL_MAX_Y) / HPGL_UNITS_PER_MM;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
               "viewBox=\"0 0 %d %d\" "
               "width=\"%.1fmm\" height=\"%.1fmm\">\n",
            HPGL_MAX_X, HPGL_MAX_Y, svg_w, svg_h);

    // White paper background
    fprintf(f, "  <rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n",
            HPGL_MAX_X, HPGL_MAX_Y);

    // Pen colors: pen 1 = black, pen 2 = red (HP 7470A convention)
    auto pen_color = [](int pen) -> const char* {
        return (pen == 2) ? "#cc0000" : "#000000";
    };

    // Pen width: 0.3mm = 12 plotter units (standard HP fiber-tip pen)
    const float pen_width = 12.0f;

    // Group by pen for cleaner SVG
    for (int pen = 1; pen <= 2; pen++) {
        bool has_content = false;
        for (const auto& seg : segments_) {
            if (seg.pen == pen) { has_content = true; break; }
        }
        if (!has_content) continue;

        fprintf(f, "  <g stroke=\"%s\" fill=\"none\" stroke-width=\"%.1f\" "
                   "stroke-linecap=\"round\" stroke-linejoin=\"round\">\n",
                pen_color(pen), pen_width);

        for (const auto& seg : segments_) {
            if (seg.pen != pen) continue;
            // Flip Y: SVG_Y = HPGL_MAX_Y - HPGL_Y
            float fy1 = HPGL_MAX_Y - seg.y1;
            float fy2 = HPGL_MAX_Y - seg.y2;

            switch (seg.type) {
                case PlotPrimitive::Line:
                    fprintf(f, "    <line x1=\"%.1f\" y1=\"%.1f\" "
                               "x2=\"%.1f\" y2=\"%.1f\"/>\n",
                            seg.x1, fy1, seg.x2, fy2);
                    break;

                case PlotPrimitive::Circle:
                    fprintf(f, "    <circle cx=\"%.1f\" cy=\"%.1f\" r=\"%.1f\"/>\n",
                            seg.x1, fy1, seg.radius);
                    break;

                case PlotPrimitive::Arc: {
                    // Convert center+start+sweep to SVG arc path
                    float r = seg.radius;
                    float sa = seg.start_angle * static_cast<float>(M_PI) / 180.0f;
                    float ea = (seg.start_angle + seg.sweep_angle) * static_cast<float>(M_PI) / 180.0f;
                    float sx = seg.x1 + r * std::cos(sa);
                    float sy = HPGL_MAX_Y - (seg.y1 + r * std::sin(sa));
                    float ex = seg.x1 + r * std::cos(ea);
                    float ey = HPGL_MAX_Y - (seg.y1 + r * std::sin(ea));
                    int large_arc = std::abs(seg.sweep_angle) > 180 ? 1 : 0;
                    int sweep_flag = seg.sweep_angle > 0 ? 0 : 1;  // Y flip inverts sweep
                    fprintf(f, "    <path d=\"M%.1f,%.1f A%.1f,%.1f 0 %d,%d %.1f,%.1f\"/>\n",
                            sx, sy, r, r, large_arc, sweep_flag, ex, ey);
                    break;
                }

                case PlotPrimitive::Label: {
                    float font_size = char_height_ * HPGL_UNITS_PER_MM * 10.0f;
                    fprintf(f, "    <text x=\"%.1f\" y=\"%.1f\" "
                               "font-family=\"monospace\" font-size=\"%.1f\" "
                               "fill=\"%s\" stroke=\"none\">%s</text>\n",
                            seg.x1, HPGL_MAX_Y - seg.y1, font_size,
                            pen_color(pen), seg.text.c_str());
                    break;
                }
            }
        }
        fprintf(f, "  </g>\n");
    }

    fprintf(f, "</svg>\n");
    fclose(f);
    LOG_INFO("HP-GL: exported SVG to " << path);
    return true;
}
