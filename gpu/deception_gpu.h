/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef DECEPTION_GPU_H
#define DECEPTION_GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    const char *splash;         /* start-up picture (the dump's sce_sys/pic1.png), NULL: none */
} DeceptionGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void deception_gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int deception_gpu_init(const DeceptionGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t deception_gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int deception_gpu_handle_fault(void *ucontext, void *address);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int deception_gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int deception_gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int deception_gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int deception_gpu_choice_poll(void);
/* Confirmation with a separate preview of the new save; PNG data is decoded before returning. */
int deception_gpu_choice_confirm(const char *title, const char *const *items, int count, int focus,
                                 const unsigned char *png, uint64_t size, const char *caption, const char *unavailable);
/* 1 while a dialog (save list, text entry) is shown: the game's pad input is held neutral. */
int deception_gpu_overlay_captures_input(void);
/* Opens the Display Settings panel (F10, or Select held for 2 s: runtime_pad.c); 1 while it is open. */
void deception_display_settings_open(void);
int deception_display_settings_is_open(void);
/* Asks "Exit Deception?" (No focused); Yes closes the game as closing its window does. Any thread. */
void deception_exit_prompt_open(void);
/* settings.ini title_back_exits (DECEPTION_TITLE_BACK_EXITS, default on): Back on the title screen
 * opens the exit prompt (runtime_pad.c). Any thread. */
int deception_title_back_exits(void);
/* Ends the game as closing its window does (any thread). */
void deception_gpu_request_quit(void);
/* 1 when settings.ini render_resolution (DECEPTION_RENDER_RESOLUTION) is 4k: the game runs in PS4
 * Pro mode with a 3840x2160 video out. Valid once deception_gpu_init returned. */
int deception_render_4k(void);
/* The game's display width at 2160 lines: 3840 (16:9) for a 16:9 or narrower output, up to 7680
 * (32:9) for a wider one; the output at start decides (the window's size, the desktop's in
 * borderless). The video out is that wide (half with render_resolution=1080p) and
 * runtime_game_widen widens the engine to match. Valid once deception_gpu_init returned. */
int deception_width_4k(void);
/* Sets it back (3840) when the engine could not be widened; before the game opens its video out. */
void deception_set_width_4k(int width);
/* 1 when settings.ini hdr (DECEPTION_HDR) is on: the video out offers BT.2020 PQ when the display
 * has an HDR10 surface, and the game then outputs HDR. Valid once deception_gpu_init returned. */
int deception_hdr(void);
/* 1 when settings.ini skip_intro (DECEPTION_SKIP_INTRO, default on) is on: the game starts without
 * its start-up logos and notices (-nosplash). Valid once deception_gpu_init returned. */
int deception_skip_intro(void);
/* settings.ini frame_rate (DECEPTION_FRAME_RATE): 30, 60, 90, 120, 144 or 180; the Display Settings panel
 * changes it while the game runs. The PS4 vblank runs at it and the engine's target frame rate is set to
 * match at each flip (runtime_game.c). Valid once deception_gpu_init returned; any thread. */
int deception_frame_rate(void);
/* Sets frame_rate (one of 30, 60, 90, 120, 144, 180) until the next start; any thread. */
void deception_set_frame_rate(int rate);
/* sceCommonDialogInitialize ran (runtime_services.c): the upstream dialog libraries check it. */
void deception_gpu_common_dialog_initialized(void);
/* Speaker layout for the game's 7.1 audio (settings.ini audio_channels): 0 automatic (the
 * device's), 1 stereo, 2 5.1, 3 7.1. Read when an audio port is opened. */
int deception_gpu_audio_channels(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned deception_gpu_symbol_count(void);
/* Yields of threads waiting for guest memory copies (CPU report). */
unsigned long long deception_gpu_copy_yields(void);
/* Frames handed to the presenter since start (hang dump). */
unsigned long long deception_gpu_flips(void);
/* A notice in the corner for `milliseconds` (any thread; the runtime's messages to the player). */
void deception_gpu_notice(const char *text, int milliseconds);
/* Trophy unlocked (any thread): its name as a notice in the corner. The other arguments are kept
 * for the runtime's call: id, description and PNG icon. */
void deception_gpu_toast_trophy(int id, const char *title, const char *detail, const unsigned char *png,
                                 uint64_t png_size);
#ifdef __cplusplus
}
#endif
#endif
