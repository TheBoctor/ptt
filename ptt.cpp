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

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <rohrkabel/device/device.hpp>
#include <rohrkabel/spa/pod/object/body.hpp>
#include <rohrkabel/registry/events.hpp>
#include <rohrkabel/registry/registry.hpp>

#include <libconfig.h++>

namespace pw = pipewire;
namespace cfg = libconfig;

enum class InputType {
	MOUSE_ONLY,
	KEYBOARD_ONLY,
	MOUSE_AND_KEYBOARD
};

std::unordered_map<std::string, InputType> input_type_lookup =
{
	{ "MOUSE_ONLY", InputType::MOUSE_ONLY },
	{ "MOUSE", InputType::MOUSE_ONLY },

	{ "KEYBOARD_ONLY", InputType::KEYBOARD_ONLY },
	{ "KEYBOARD", InputType::KEYBOARD_ONLY },

	{ "KEYBOARD_AND_MOUSE", InputType::MOUSE_AND_KEYBOARD },
	{ "MOUSE_AND_KEYBOARD", InputType::MOUSE_AND_KEYBOARD },
	{ "BOTH", InputType::MOUSE_AND_KEYBOARD }
};

std::atomic<bool> thread_button_state = false,
					quit_application = false,
					thread_finished = false;


constexpr bool VERBOSE_MODE = false;
InputType DESIRED_EVENT_TYPE = InputType::MOUSE_AND_KEYBOARD;
std::string DESIRED_MIC = "";
KeySym PTT_KEY_SYM = NoSymbol;
int PTT_SOUND_VOLUME = 48;
#define PTT_ON_SOUND "sounds/on.ogg"
#define PTT_OFF_SOUND "sounds/off.ogg"

static struct xkb_context *xkb_context;
static struct xkb_keymap *keymap = NULL;
static struct xkb_state *xkb_state = NULL;

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
		print_log(log_level::warn, "Failure parsing config file \"%s\"\n", xdg_config_dir.c_str());
		return false;
	}

	try
	{
		std::string mic = my_cfg.lookup("mic");
		DESIRED_MIC = mic;
	}
	catch (const cfg::SettingNotFoundException &e)
	{
		print_log(log_level::warn, "Couldn't find \"mic\" setting in config file.");
	}

	try
	{
		std::string listen_for_type = my_cfg.lookup("keyboard_or_mouse");

		if ( auto find = input_type_lookup.find(listen_for_type); find != input_type_lookup.end() )
		{
			DESIRED_EVENT_TYPE = find->second;
			print_log(log_level::info, "Listening for events with filter: %s.\n", listen_for_type.c_str());
		}
	}
	catch (const cfg::SettingNotFoundException &e)
	{
		print_log(log_level::verbose, "No filter specified for exclusive keyboard and/or mouse listening. Defaulting to \"BOTH\".\n");
		print_log(log_level::info, "If you want to only listen for keyboard or mouse events individually, specify \"keyboard_or_mouse\" in ptt.conf. Valid settings include \"KEYBOARD_ONLY\", \"MOUSE_ONLY\", or \"BOTH\"\n");
	}

	try
	{
		std::string btn = my_cfg.lookup("key");
		if (!btn.empty() && DESIRED_EVENT_TYPE != InputType::MOUSE_ONLY)
		{
			PTT_KEY_SYM = XStringToKeysym(btn.c_str());
			if (PTT_KEY_SYM != NoSymbol)
			{
				std::string keyboard_help_msg = ".";
				if (DESIRED_EVENT_TYPE == InputType::MOUSE_AND_KEYBOARD)
				{
					keyboard_help_msg = ", or hold your mouse's side button.";
				}
				print_log(log_level::info, "Hold the %s key to talk%s\n", btn.c_str(), keyboard_help_msg.c_str());
			}
			else
			{
				print_log(log_level::info, "Invalid keybind \"%s\" specified in config file.\nHold your mouse's side button to talk.\n", btn.c_str());
				DESIRED_EVENT_TYPE = InputType::MOUSE_ONLY;
			}
		}
	}
	catch (const cfg::SettingNotFoundException &e)
	{
		DESIRED_EVENT_TYPE = InputType::MOUSE_ONLY;
		print_log(log_level::info, "No \"key\" setting specified in ptt.conf. Hold your mouse's side button to talk.\n");
	}

	try
	{
		int desired_volume = my_cfg.lookup("volume");
		PTT_SOUND_VOLUME = std::clamp(desired_volume, 0, MIX_MAX_VOLUME);
	}
	catch (const cfg::SettingNotFoundException &e)
	{
		print_log(log_level::verbose, "Using default volume level for push-to-talk sound effects. (%d)\n", PTT_SOUND_VOLUME);
	}

	return true;
}


static void process_event (struct libinput_event* event)
{
	int type = libinput_event_get_type (event);

	if ( (DESIRED_EVENT_TYPE != InputType::MOUSE_ONLY) && (type == LIBINPUT_EVENT_KEYBOARD_KEY) )
	{
		struct libinput_event_keyboard *keyboard_event = libinput_event_get_keyboard_event (event);
		uint32_t key = libinput_event_keyboard_get_key (keyboard_event);
		int state = libinput_event_keyboard_get_key_state (keyboard_event);
		xkb_state_update_key (xkb_state, key+8, (xkb_key_direction)state);
		KeySym sym = xkb_state_key_get_one_sym(xkb_state, key+8);

		if (PTT_KEY_SYM == sym)
		{
			thread_button_state = state;
		}
	}

	if ( (DESIRED_EVENT_TYPE != InputType::KEYBOARD_ONLY) && (type == LIBINPUT_EVENT_POINTER_BUTTON) )
	{
		struct libinput_event_pointer* pointer_event = libinput_event_get_pointer_event (event);
		uint32_t which_button = libinput_event_pointer_get_button(pointer_event);
		bool is_talk_button = (which_button == BTN_SIDE || which_button == BTN_EXTRA);
		
		if (is_talk_button)
		{
			thread_button_state = libinput_event_pointer_get_button_state(pointer_event);
		}
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

	if ( SDL_Init(SDL_INIT_AUDIO) == -1 )
	{
		print_log(log_level::critical, "Failed to init SDL2.\n");
		return 0;
	}
	if ( Mix_OpenAudio( 44100, MIX_DEFAULT_FORMAT, 2, 2048) == -1 )
	{
		print_log(log_level::critical, "Failed to init SDL2 Mixer.\n");
		return 0;
	}

	bool is_pressed = false, last_is_pressed = false;
	signal (SIGINT, handle_quit_signal);

	load_config();
	if (DESIRED_MIC.empty())
	{
		print_log(log_level::info, "You have not chosen a mic in your config.\nDetected audio sources:\n");
		quit_application = true;
	}

	Mix_Chunk* ptt_on_sample = nullptr;
	Mix_Chunk* ptt_off_sample = nullptr;

	ptt_on_sample = Mix_LoadWAV(PTT_ON_SOUND);
	ptt_off_sample = Mix_LoadWAV(PTT_OFF_SOUND);

	if (ptt_on_sample && ptt_off_sample)
	{
		Mix_VolumeChunk(ptt_on_sample, PTT_SOUND_VOLUME);
		Mix_VolumeChunk(ptt_off_sample, PTT_SOUND_VOLUME);
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
			print_log(log_level::info, "  %s %s\n", (desc == DESIRED_MIC ? "--> " : "    "), desc.c_str());
			if (desc == DESIRED_MIC)
			{
				devices.emplace_back(std::move(*device));
			}
		}
	};

	listener.on<pipewire::registry_event::global>(on_global);
	core->update();

	if (!DESIRED_MIC.empty() && devices.empty())
	{
		print_log(log_level::critical, "Your desired mic, \"%s\", was not detected.\n", DESIRED_MIC.c_str());
		quit_application = true;
	}

	auto libinput_loop = std::thread(libinput_poll);
	libinput_loop.detach();

	print_log(log_level::verbose, "Turning microphone OFF when starting main loop.\n");
	set_mute_all(devices, core, true);

	while (!thread_finished)
	{
		last_is_pressed = is_pressed;
		is_pressed = thread_button_state.load();

		if(is_pressed && !last_is_pressed)
		{
			print_log(log_level::verbose, "Push to talk ON.\n");
			if (ptt_on_sample)
			{
				Mix_PlayChannel(-1, ptt_on_sample, 0);
			}
			set_mute_all(devices, core, false);
		}
		else if (!is_pressed && last_is_pressed)
		{
			print_log(log_level::verbose, "Push to talk OFF.\n");
			if (ptt_off_sample)
			{
				Mix_PlayChannel(-1, ptt_off_sample, 0);
			}
			set_mute_all(devices, core, true);
		}
		SDL_Delay(1);
	}

	print_log(log_level::verbose, "Turning microphone back ON before quitting.\n");
	set_mute_all(devices, core, false);

	Mix_FreeChunk(ptt_on_sample);
	Mix_FreeChunk(ptt_off_sample);
	Mix_CloseAudio();
	SDL_Quit();
	return 0;
}
