// A small C++ wrapper around wl_listener.
//
//   Listener on_map;
//   on_map.connect<&View::handle_map>(&surface->events.map, this);
//
// The handler is a member function `void handle_map(void *data)`.
// The listener disconnects itself when destroyed, so objects can simply
// be deleted. It is safe for a handler to `delete this` as its last action.
#pragma once

#include "wlr.hpp"

namespace dusk {

struct Listener {
	wl_listener raw{};   // must stay the first member
	void *owner = nullptr;
	bool linked = false;

	Listener() = default;
	Listener(const Listener &) = delete;
	Listener &operator=(const Listener &) = delete;
	~Listener() { disconnect(); }

	template <auto Method, class T>
	void connect(wl_signal *signal, T *object) {
		disconnect();
		owner = object;
		raw.notify = [](wl_listener *l, void *data) {
			auto *self = reinterpret_cast<Listener *>(l);
			(static_cast<T *>(self->owner)->*Method)(data);
		};
		wl_signal_add(signal, &raw);
		linked = true;
	}

	void disconnect() {
		if (linked) {
			wl_list_remove(&raw.link);
			linked = false;
		}
	}
};

} // namespace dusk
