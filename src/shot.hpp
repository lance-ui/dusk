// The screenshot tool (Print): the screen dims, a bar at the bottom picks
// a picture or a video (recording), and what to capture — an area you drag out, a window or a whole screen — with
// the pointer in it or not and an optional delay. Saving, copying and the
// notification afterwards follow Settings > Screenshots.
#pragma once

#include <string>
#include <vector>

#include "server.hpp"
#include "ui.hpp"

namespace dusk {

class ScreenshotTool {
public:
	explicit ScreenshotTool(Server *server);
	~ScreenshotTool();
	ScreenshotTool(const ScreenshotTool &) = delete;
	ScreenshotTool &operator=(const ScreenshotTool &) = delete;

	bool active() const { return open_; }
	void open();
	void close();
	void output_gone(Output *output);

	// While it's open it gets all pointer and key input.
	void motion(double lx, double ly);
	void button(double lx, double ly, uint32_t button, bool pressed);
	bool key(xkb_keysym_t sym);

private:
	enum class Mode { Area, Window, Screen };
	struct Hot {
		std::string id;
		wlr_box box;
	};

	void redraw_bar();
	void update_target();     // what would be captured, from the pointer and mode
	void show_target();       // the dimming around it
	void capture();
	bool on_bar(double lx, double ly) const;
	std::string hot_at(double lx, double ly) const;
	void set_mode(Mode mode);
	void set_video(bool video);   // the photo / video switch

	Server *server_;
	wlr_scene_tree *tree_;
	wlr_scene_rect *dim_[4];
	wlr_scene_rect *edge_[4];
	wlr_scene_buffer *label_;
	UiLayer bar_;
	std::vector<Hot> hots_;
	std::string hover_;

	bool open_ = false;
	Output *output_ = nullptr;   // where the bar is
	wlr_box bar_box_{};
	Mode mode_ = Mode::Area;
	bool pointer_ = false;
	int delay_ = 0;              // seconds
	bool video_ = false;         // recording instead of a picture

	wlr_box target_{};           // what Capture takes (width 0 = nothing yet)
	wlr_box area_{};             // the area you dragged out (kept between uses)
	bool selecting_ = false, moving_ = false;
	double press_x_ = 0, press_y_ = 0;
	wlr_box press_area_{};
	double px_ = 0, py_ = 0;     // the pointer
	std::string label_text_;

	// Drawing on it before it's taken (the pen on the bar).
	View *raised_ = nullptr;     // window mode: the window brought to the front
	// Window mode: the open windows, in a row under the buttons (hover one to
	// see it, click to take it).
	std::vector<View *> chips_;
	View *chip_view(const std::string &id) const;
	void collect_chips();
	bool pen_ = false;
	int ink_color_ = 0;
	bool drawing_ = false;
	std::vector<InkStroke> ink_;
	wlr_scene_buffer *ink_node_ = nullptr;
	void redraw_ink();
	Color ink_color() const;
};

} // namespace dusk
