/*
* Made using example source code from the Libinput and Rohrkabel projects.
* Any code directly referenced or reused is the work of its original authors.
*/

#include <algorithm>
#include <cctype>
#include <string_view>
#include <thread>
#include <atomic>
#include <string>
#include <unordered_map>
#include <libinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <xkbcommon/xkbcommon.h>
#include <X11/Xlib.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/input.h>

#include <rohrkabel/device/device.hpp>
#include <rohrkabel/spa/pod/object/body.hpp>
#include <rohrkabel/registry/events.hpp>
#include <rohrkabel/registry/registry.hpp>

#include <libconfig.h++>

namespace pw = pipewire;
namespace cfg = libconfig;

constexpr bool VERBOSE_MODE = false;

static constexpr size_t MOUSE_BUTTON_COUNT = 5;
static constexpr int MOUSE_BUTTON_LOOKUP[MOUSE_BUTTON_COUNT] = { BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK, BTN_TASK };

static struct xkb_context *xkb_context;
static struct xkb_keymap *keymap = NULL;
static struct xkb_state *xkb_state = NULL;

std::atomic<bool> thread_key_chord_pressed = false,
					thread_mouse_pressed = false,
					quit_application = false,
					thread_finished = false;

std::string desired_mic = "";
std::unordered_map<KeySym, bool> ptt_key_chord;
std::unordered_map<int, bool> ptt_mouse_state;

enum log_level
{
	verbose,
	info,
	warn,
	critical
};


// TODO: Make a much, much safer version of this.
void print_log(log_level lv, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	if ( VERBOSE_MODE || lv >= log_level::info )
	{
		vprintf(fmt, args);
	}

	va_end(args);
}


// Ergonomics: In case you keep many shells open at once on your desktop.
void set_term_title(const std::string& title)
{
	printf("\033]0;%s\007", title.c_str());
}


bool ichar_equals(char a, char b)
{
	return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}


bool iequals(std::string_view lhs, std::string_view rhs)
{
	return std::ranges::equal(lhs, rhs, ichar_equals);
}


void add_key_to_chord(const libconfig::Setting& key_str, std::unordered_map<KeySym, bool>& key_chord_state)
{
	if (key_str.isString())
	{
		if (const KeySym sym = XStringToKeysym(key_str.c_str()))
		{
			key_chord_state.emplace(sym, false);
		}
	}
}


void enable_mouse_button(const libconfig::Setting& mouse_button, std::unordered_map<int, bool>& mouse_button_state)
{
	if (mouse_button.isNumber())
	{
		const int button_idx = int(mouse_button) - 1;
		if (button_idx >= 0 && button_idx < MOUSE_BUTTON_COUNT)
		{
			mouse_button_state.emplace(MOUSE_BUTTON_LOOKUP[button_idx], false);
		}
	}
}


bool load_config()
{
	// See ptt.conf.example for a list of parameters.
	std::string xdg_config_dir = secure_getenv("HOME");
	xdg_config_dir += "/.config/ptt.conf";
	cfg::Config my_cfg;

	try
  	{
		my_cfg.readFile(xdg_config_dir);
  	}
	catch (const cfg::FileIOException &e)
	{
		print_log(log_level::warn, "Failure opening config file \"%s\" for reading.\n", xdg_config_dir.c_str());
		return false;
	}
	catch (const cfg::ParseException &e)
	{
		print_log(log_level::warn, "Failure parsing config file \"%s\", line %d: %s\n", xdg_config_dir.c_str(), e.getLine(), e.getError());
		return false;
	}

	// Desired mic name:
	my_cfg.lookupValue("mic", desired_mic);

	// Desired keyboard key(s):
	if (my_cfg.exists("keyboard_keys"))
	{
		const libconfig::Setting& keyboard_keys = my_cfg.getRoot()["keyboard_keys"];
		if (keyboard_keys.isArray())
		{
			for(size_t i = 0; i < keyboard_keys.getLength(); ++i)
			{
				add_key_to_chord(keyboard_keys[i], ptt_key_chord);
			}
		}
		else if (keyboard_keys.isString())
		{
			add_key_to_chord(keyboard_keys, ptt_key_chord);
		}
	}

	// Desired mouse button(s):
	if (my_cfg.exists("mouse_buttons"))
	{
		const libconfig::Setting& mouse_buttons = my_cfg.getRoot()["mouse_buttons"];
		if (mouse_buttons.isArray())
		{
			for(size_t i = 0; i < mouse_buttons.getLength(); ++i)
			{
				enable_mouse_button(mouse_buttons[i], ptt_mouse_state);
			}
		}
		else if (mouse_buttons.isString())
		{
			enable_mouse_button(mouse_buttons, ptt_mouse_state);
		}
	}

	return true;
}


static void process_event (struct libinput_event* event)
{
	int type = libinput_event_get_type (event);

	if (type == LIBINPUT_EVENT_KEYBOARD_KEY)
	{
		struct libinput_event_keyboard *keyboard_event = libinput_event_get_keyboard_event (event);
		uint32_t key = libinput_event_keyboard_get_key (keyboard_event);
		int state = libinput_event_keyboard_get_key_state (keyboard_event);
		xkb_state_update_key (xkb_state, key+8, (xkb_key_direction)state);
		KeySym sym = xkb_state_key_get_one_sym(xkb_state, key+8);

		if (ptt_key_chord.find(sym) != ptt_key_chord.end())
		{
			ptt_key_chord[sym] = state;
		}

		thread_key_chord_pressed = !ptt_key_chord.empty() && std::all_of(ptt_key_chord.begin(), ptt_key_chord.end(), [](const auto& p) { return p.second; });
	}

	else if (type == LIBINPUT_EVENT_POINTER_BUTTON)
	{
		struct libinput_event_pointer* pointer_event = libinput_event_get_pointer_event (event);
		uint32_t which_button = libinput_event_pointer_get_button(pointer_event);

		if (ptt_mouse_state.find(which_button) != ptt_mouse_state.end())
		{
			ptt_mouse_state[which_button] = libinput_event_pointer_get_button_state(pointer_event);
		}

		thread_mouse_pressed = std::any_of(ptt_mouse_state.begin(), ptt_mouse_state.end(), [](const auto& p) { return p.second; });
	}

	libinput_event_destroy (event);
}


static int open_restricted (const char* path, int flags, void* user_data)
{
	return open (path, flags);
}


static void close_restricted (int fd, void* user_data)
{
	close (fd);
}


void libinput_poll()
{
	bool all_ok = true;
	static struct libinput_interface interface = {&open_restricted, &close_restricted};
	static uint32_t last_button_pressed = 0;

	struct udev* udev = udev_new ();
	if (!udev)
	{
		print_log(log_level::critical, "Udev error.\n");
		all_ok = false;
	}

	struct libinput* libinput = libinput_udev_create_context (&interface, NULL, udev);
	if (!libinput)
	{
		print_log(log_level::critical, "Libinput error.\n");
		all_ok = false;
	}
	else if (libinput_udev_assign_seat (libinput, "seat0") == -1)
	{
		print_log(log_level::critical, "Libinput cannot assign seat0.\n");
		all_ok = false;
	}

	xkb_context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
	keymap = xkb_keymap_new_from_names (xkb_context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap)
	{
		print_log(log_level::critical, "Xkb keymap error.\n");
		all_ok = false;
	}
	xkb_state = xkb_state_new (keymap);
	
	while (all_ok && !quit_application)
	{
		struct pollfd fd = { libinput_get_fd(libinput), POLLIN, 0 };
		poll (&fd, 1, 1000);	// Timeout to avoid blocking if the user doesn't have permission for /dev/input
		libinput_dispatch (libinput);
		struct libinput_event *event;
		while ((event = libinput_get_event(libinput)))
		{
			process_event (event);
		}
	}
	
	if (libinput)
	{
		libinput_unref (libinput);
	}
	if (udev)
	{
		udev_unref (udev);
	}

	print_log(log_level::info, "Stopping input polling.\n");
	thread_finished = true;
}


void handle_quit_signal(sig_atomic_t s)
{
	print_log(log_level::info, "Got quit signal.\n");
	quit_application = true;
}


auto try_get_mute_prop(const pw::spa::pod &pod)
{
	// NOLINTNEXTLINE
	auto impl = [](const pw::spa::pod_prop *parent, const pw::spa::pod &pod, auto &self) -> std::optional<pw::spa::pod_prop>
	{
		if (pod.type() == pw::spa::pod_type::object)
		{
			for (const auto &item : pod.body<pw::spa::pod_object_body>())
			{
				auto rtn = self(&item, item.value(), self);
				if (!rtn.has_value())
				{
					continue;
				}

				return rtn;
			}
		}

		if (parent && pod.type() == pw::spa::pod_type::boolean && parent->name().find("mute") != std::string::npos)
		{
			return *parent;
		}

		return std::nullopt;
	};

	return impl(nullptr, pod, impl);
};


void set_mute_all(std::vector<pw::device>& devs, std::shared_ptr<pipewire::core> core, bool new_mute_value)
{
	if (!core)
	{
		return;
	}

	for ( auto& dev : devs )
	{
		auto dev_name = dev.info().props.at("device.description");
		auto params = dev.params();
		core->update();

		for (const auto &[pod_id, pod] : params.get())
		{
			auto mute_prop = try_get_mute_prop(pod);
			if (mute_prop)
			{
				mute_prop->value().write(new_mute_value);
				dev.set_param(pod_id, 0, pod);
				core->update();
				print_log(log_level::verbose, "The device, \"%s\", has been %s.\n", dev_name.c_str(), (new_mute_value ? "muted" : "unmuted"));
			}
		}
	}
}


int main ()
{
	set_term_title("Push to Talk");

	bool is_pressed = false, last_is_pressed = false;
	signal (SIGINT, handle_quit_signal);

	load_config();
	if (desired_mic.empty())
	{
		print_log(log_level::info, "You have not chosen a mic in your config.\nDetected audio sources:\n");
		quit_application = true;
	}

	auto main_loop = pw::main_loop::create();
	auto context   = pipewire::context::create(main_loop);
	auto core      = context->core();
	auto reg       = core->registry();

	std::vector<pw::device> devices;

	auto listener = reg->listen();

	auto on_global = [&](const pipewire::global &global)
	{
		if (global.type != pipewire::device::type)
		{
			return;
		}

		auto device = reg->bind<pipewire::device>(global.id).get();
		auto info   = device->info();
		auto params  = device->params();

		if (info.props["media.class"] != "Audio/Device")
		{
			return;
		}

		if (info.props.contains("device.description"))
		{
			std::string& desc = info.props.at("device.description");
			print_log(log_level::info, "  %s %s\n", (desc == desired_mic ? "--> " : "    "), desc.c_str());
			if (desc == desired_mic)
			{
				devices.emplace_back(std::move(*device));
			}
		}
	};

	listener.on<pipewire::registry_event::global>(on_global);
	core->update();

	if (!desired_mic.empty() && devices.empty())
	{
		print_log(log_level::critical, "Your desired mic, \"%s\", was not detected.\n", desired_mic.c_str());
		quit_application = true;
	}

	auto libinput_loop = std::thread(libinput_poll);
	libinput_loop.detach();

	print_log(log_level::verbose, "Turning microphone OFF when starting main loop.\n");
	set_mute_all(devices, core, true);

	while (!thread_finished)
	{
		last_is_pressed = is_pressed;
		is_pressed = thread_key_chord_pressed.load() || thread_mouse_pressed.load();

		if (is_pressed && !last_is_pressed)
		{
			print_log(log_level::verbose, "Push to talk ON.\n");
			set_mute_all(devices, core, false);
		}
		else if (!is_pressed && last_is_pressed)
		{
			print_log(log_level::verbose, "Push to talk OFF.\n");
			set_mute_all(devices, core, true);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	print_log(log_level::verbose, "Turning microphone back ON before quitting.\n");
	set_mute_all(devices, core, false);
	return 0;
}
