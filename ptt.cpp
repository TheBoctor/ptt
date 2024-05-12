/*
* Made using example source code from the Libinput and Rohrkabel projects.
* All code referenced or reused is the sole work of its original authors.
*/

#include <thread>
#include <atomic>
//#include <string>
#include <libinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
//#include <xkbcommon/xkbcommon.h>
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

namespace pw = pipewire;

std::atomic<bool> thread_button_state = false, quit_application = false, thread_finished = false;

#define DESIRED_MIC "Blue Snowball"
#define PTT_ON_SOUND "sounds/on.ogg"
#define PTT_OFF_SOUND "sounds/off.ogg"
constexpr bool VERBOSE_MODE = false;
constexpr int PTT_SOUND_VOLUME = MIX_MAX_VOLUME / 2;

/*
static struct xkb_context *xkb_context;
static struct xkb_keymap *keymap = NULL;
static struct xkb_state *xkb_state = NULL;
*/

enum log_level
{
	verbose,
	info,
	warn,
	critical
};


void print_log(log_level lv, const char *fmt, ...)
{
	va_list args;
    va_start(args, fmt);

	if ( VERBOSE_MODE || (!VERBOSE_MODE && lv >= log_level::info) )
	{
		printf("DEBUG: ");
		vprintf(fmt, args);
	}

	va_end(args);
}


static void process_event (struct libinput_event* event)
{
	int type = libinput_event_get_type (event);

	if (type == LIBINPUT_EVENT_POINTER_BUTTON)
	{
		struct libinput_event_pointer* pointer_event = libinput_event_get_pointer_event (event);
		uint32_t which_button = libinput_event_pointer_get_button(pointer_event);
		bool is_talk_button = (which_button == BTN_SIDE || which_button == BTN_EXTRA);
		
		if (is_talk_button)
		{
			thread_button_state = libinput_event_pointer_get_button_state(pointer_event);
		}
	}
	/*
	else if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
		struct libinput_event_keyboard *keyboard_event = libinput_event_get_keyboard_event (event);
		uint32_t key = libinput_event_keyboard_get_key (keyboard_event);
		int state = libinput_event_keyboard_get_key_state (keyboard_event);
		xkb_state_update_key (xkb_state, key+8, state);
		if (state == LIBINPUT_KEY_STATE_PRESSED) {
			uint32_t utf32 = xkb_state_key_get_utf32 (xkb_state, key+8);
			if (utf32) {
				if (utf32 >= 0x21 && utf32 <= 0x7E) {
					//printf ("the key %c was pressed\n", (char)utf32);
				}
				else {
					//printf ("the key U+%04X was pressed\n", utf32);
				}
			}
		}
	}
	*/
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

	/*
	xkb_context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
	keymap = xkb_keymap_new_from_names (xkb_context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) EXIT ("keymap error\n");
	xkb_state = xkb_state_new (keymap);
	*/
	
	while (all_ok && !quit_application) {
		struct pollfd fd = {libinput_get_fd(libinput), POLLIN, 0};
		poll (&fd, 1, -1);
		libinput_dispatch (libinput);
		struct libinput_event *event;
		while ((event = libinput_get_event(libinput))) {
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

	print_log(log_level::info, "Libinput thread finished.\n");
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
	auto impl = [](const pw::spa::pod_prop *parent, const pw::spa::pod &pod,
					auto &self) -> std::optional<pw::spa::pod_prop> {
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

	Mix_Chunk* ptt_on_sample = nullptr;
	Mix_Chunk* ptt_off_sample = nullptr;

	ptt_on_sample = Mix_LoadWAV(PTT_ON_SOUND);
	ptt_off_sample = Mix_LoadWAV(PTT_OFF_SOUND);

	if (ptt_on_sample && ptt_off_sample)
	{
		Mix_VolumeChunk(ptt_on_sample, PTT_SOUND_VOLUME);
		Mix_VolumeChunk(ptt_off_sample, PTT_SOUND_VOLUME);
	}

	bool is_pressed = false, last_is_pressed = false;
	signal (SIGINT, handle_quit_signal);

	auto main_loop = pw::main_loop::create();
    auto context   = pipewire::context::create(main_loop);
    auto core      = context->core();
    auto reg       = core->registry();

	std::vector<pw::device> devices;

	auto listener = reg->listen();

    auto on_global = [&](const pipewire::global &global) {
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
			if (info.props.at("device.description") == DESIRED_MIC)
			{
				devices.emplace_back(std::move(*device));
				print_log(log_level::info, "Got desired device.\n");
			}
		}
    };

	listener.on<pipewire::registry_event::global>(on_global);
	core->update();

	auto libinput_loop = std::thread(libinput_poll);
	libinput_loop.detach();

	print_log(log_level::info, "Turning microphone OFF when starting main loop.\n");
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

	print_log(log_level::info, "Turning microphone back ON before quitting.\n");
	set_mute_all(devices, core, false);

	Mix_FreeChunk(ptt_on_sample);
	Mix_FreeChunk(ptt_off_sample);
	Mix_CloseAudio();
	SDL_Quit();
	return 0;
}
