#ifndef DECEPTION_RUNTIME_H
#define DECEPTION_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in deception_longjmp
 * (gpu/shim/deception_toggles.h uses the same buffer type). */
typedef deception_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) deception_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (loader.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
/* DECEPTION_SLOW_WAITS=<ms>: log guest blocking calls (locks, waits, reads) longer than that.
 * runtime_wait_begin() returns 0 when it is off; runtime_wait_end() then does nothing. */
uint64_t runtime_wait_begin(void);
void runtime_wait_end(const char *what,uint64_t start,const char *detail,long long arg);
/* The floating-point state a PS4 gives its threads (runtime_thread.c). */
void runtime_orbis_fpu(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
/* libc.prx's thread-local destructor runner (_sceKernelSetThreadDtors); NULL until it registers. */
extern GuestCallback runtime_guest_thread_dtors, runtime_guest_thread_atexit_count, runtime_guest_thread_atexit_report;
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
/* Called at the end of runtime_report (the loader adds its own counters). */
void runtime_set_report_hook(void (*hook)(void));
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* Host access to guest memory past page protection, through the backing view: write_backing
 * returns 0 when part of the range is unmapped, and skips GPU tracking. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
/* Logs "Game: ..." (param.sfo TITLE_ID and APP_VER, and the eboot's SHA-256 from analysis.json
 * beside the image). */
void runtime_game_identify(const char *title_id, const char *app_version, const char *app0, const char *image_path);
/* The loaded image, for runtime_game_frame_rate. */
void runtime_game_set_image(unsigned char *image);
/* DECEPTION_TRACE_CALLS: log the return values of the listed call sites (development). */
void runtime_game_trace_calls(unsigned char *image);
/* At each flip, on the game's thread (videoout, gnmdriver): settings.ini frame_rate onto the
 * engine's frame clock when they differ. */
void runtime_game_frame_rate(void);
/* A wider picture than 16:9 (width_4k = deception_width_4k(), above 3840): the engine's display
 * size table and the scene targets its render scripts create are widened to it. At load, before
 * the image is protected. 0, with a log line, when the image is not eboot 01.10 as expected. */
int runtime_game_widen(unsigned char *image, uint32_t width_4k);
/* The width a render script's target of width x height (named `name`) gets: 16:9 and 8:9
 * (checkerboard) sizes scale by width_4k/3840; the main menu's panels keep theirs. */
uint32_t runtime_game_wide_width(const char *name, uint32_t width, uint32_t height, uint32_t width_4k);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
/* TLS module ids 2..RUNTIME_TLS_MODULES-1 (link.py numbers every module with TLS). */
#define RUNTIME_TLS_MODULES 32
/* A module of the linked image (RGIMAGE6, link.py). Addresses are image offsets. Deferred
 * modules are mapped and relocated at boot but start only when the game loads them. */
#define RUNTIME_MODULE_DEFERRED 1
typedef struct { char nid[16]; uint64_t address, kind; } RuntimeModuleExport;
typedef struct {
    uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module, flags, procparam;
    char name[64];
    uint64_t export_count;
    RuntimeModuleExport *exports;
    int started;
    /* sceKernelGetModuleInfoForUnwind, image offsets (RGIMAGE7): .eh_frame_hdr (0: none),
     * .eh_frame and its size, first segment and its size. */
    uint64_t eh_frame_hdr, eh_frame, eh_frame_size, seg0, seg0_size;
} RuntimeModule;
/* The loader hands over its module table once the image is mapped (runtime_module.c), and the
 * eboot's record (base 0, size up to the first module, name and unwind fields). */
void runtime_module_register(unsigned char *image, RuntimeModule *modules, uint64_t count);
void runtime_module_set_main(const RuntimeModule *main);
/* Candidate guest return addresses on this thread's stack as "Module.prx+0xoffset" (traces). */
void runtime_guest_backtrace(char *out, size_t size, int max);
/* DECEPTION_THROW_TRACE=1: log guest C++ throws (after runtime_module_set_main). */
void runtime_throw_trace_install(void);
/* DECEPTION_VERBOSE=1: log routine events too (runtime.c). */
int runtime_verbose(void);
uintptr_t runtime_module_resolve(const char *name);
void runtime_module_report(void);
/* Starts a linked module by file name ("libSceNpToolkit2.prx") unless it already runs: its
 * handle (>0) and, for a new start, the start routine's result; or a negative SCE error. */
int32_t runtime_module_start(const char *file, uint64_t args, const void *argp, int32_t *result);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
/* A joinable guest thread running entry(argument) (libSceUlt's ulthreads); 0 or an SCE error. */
int32_t runtime_thread_spawn(void **out, void *entry, void *argument, const char *name);
int runtime_thread_finished(void *thread);
int32_t runtime_thread_join(void *thread, void **result);
uintptr_t runtime_ult_resolve(const char *name);
uintptr_t runtime_ngs2_resolve(const char *name);
uintptr_t runtime_audiodec_resolve(const char *name);
/* Counts a guest sched_yield (zero_sleep 0) or a zero-length sleep (1) for the report. */
void runtime_thread_count_yield(int zero_sleep);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
/* The stack the calling thread runs guest code on, when it is not the one the host gave it (Linux:
 * the guest main stack below 1 TiB): scePthreadAttrGet reports it. */
void runtime_thread_set_stack(void *low, uint64_t size);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
uintptr_t runtime_eventflag_resolve(const char *name);
void runtime_eventflag_report(void);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
/* An extra folder of add-ons next to <user>/addcont/ (its <title_id>/ subfolder when it has one). */
void runtime_content_extra_dir(const char *folder,const char *title_id);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
void runtime_kernel_set_neo_mode(int enabled);
/* 1 in PS4 Pro mode: set before the guest runs, so the direct memory pool can size itself from it. */
int runtime_kernel_neo_mode(void);
/* skip_intro's title screen press (runtime_pad.c): the AvPlayer reports title.mp4 starting (1) and
 * its player going away (0). */
/* settings.ini skip_intro (runtime_intro.c): started before the game's code with the game folder;
 * told of reads of FILE_PS4/archive00.lnk; asks the pad for Cross while the notice is up. */
void runtime_intro_start(const char *app0);
void runtime_intro_archive_read(uint64_t offset, uint64_t size);
int runtime_intro_cross(void);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* Offline networking (POSIX sockets, libSceHttp extras), PSN libraries and their dialogs, and
 * devices the PC lacks (camera, Audio3d, social screen): runtime_net.c, runtime_np.c, runtime_devices.c. */
uintptr_t runtime_net_resolve(const char *name);
uintptr_t runtime_np_resolve(const char *name);
uintptr_t runtime_devices_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
/* The update (patch) package overlaid on /app0: its files win over the game's. */
void runtime_file_set_update(const char *update);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
/* The game image, below 2 GiB (32-bit code addresses). */
void *runtime_image_map(size_t size, int prot);
void *runtime_low_map(size_t size, int prot);
/* Zeroed, 64-byte aligned host records the guest sees by address, below 1 TiB (DECEPTION_LOW_HANDLES). */
void *runtime_low_alloc(size_t size);
void runtime_low_free(void *pointer);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
/* deception --gamepad: prints one "Gamepad:" line for the first pad found (returns 1) or none (0). */
int runtime_pad_describe(void);
uintptr_t runtime_rtc_resolve(const char *name);
uintptr_t runtime_json_resolve(const char *name);
uintptr_t runtime_fiber_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
int64_t runtime_savedata_mtime(int32_t user, const char *dir);
void runtime_thread_attach_host(const char *name);
#endif
