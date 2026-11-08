/* The 4:3 / 21:9 / 32:9 resizing (src/runtime_game.c): which render targets widen, and the stubs that
 * runtime_game_widen puts in front of the engine's create functions, run on stand-ins with the
 * same prologues in an image-sized RWX buffer. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../src/runtime_game.c"

int deception_frame_rate(void) { return 60; }
void deception_set_frame_rate(int rate) { (void)rate; }
int runtime_verbose(void) { return 0; }
void *runtime_low_map(size_t size, int prot) {
    (void)prot;
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* A stand-in create function: the engine's prologue, then returns `result` (89 c8: ecx, 89 d0:
 * edx) and unwinds as the real epilogue does. */
static void stand_in(unsigned char *at, unsigned char frame, unsigned char result) {
    const unsigned char code[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                  0x48, 0x83, 0xec, frame, 0x89, result, 0x48, 0x83, 0xc4, frame, 0x5b, 0x41,
                                  0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
    memcpy(at, code, sizeof code);
}

/* A stand-in camera setter: the engine's first 14 bytes, then returns fov + projection m5 as the
 * function sees them (addss xmm0, [rdx+0x14]; ret). */
static void camera_stand_in(unsigned char *at) {
    memcpy(at, camera_head, sizeof camera_head);
    const unsigned char tail[] = {0xf3, 0x0f, 0x58, 0x42, 0x14, 0xc3};
    memcpy(at + sizeof camera_head, tail, sizeof tail);
}
typedef float (ABI *SetCamera)(void *self, const float *view, const float *projection, float fov, float near_z,
                               float far_z, float aspect);
static int close_to(float a, float b) { return fabsf(a - b) < 1e-4f; }

typedef uint32_t (ABI *CreateTarget)(void *self, const char *name, uint32_t format, uint32_t width, uint32_t height, void *options);
typedef uint32_t (ABI *CreateDepth)(void *self, const char *name, uint32_t width, uint32_t height, uint32_t a, void *options);

int main(void) {
    /* 21:9 at 2560x1080: 5120 at 2160 lines, so x4/3. */
    assert(runtime_game_wide_width("mrt_albedo", 1920, 1080, 5120) == 2560);
    assert(runtime_game_wide_width("mrt_albedo", 1920, 2160, 5120) == 2560); /* checkerboard: half of 5120 */
    assert(runtime_game_wide_width("screen_tmp_b", 3840, 2160, 5120) == 5120);
    assert(runtime_game_wide_width("ssao_occlusion", 960, 540, 5120) == 1280);
    assert(runtime_game_wide_width("menu_overlay", 1920, 1080, 5040) == 2520); /* the pause menu */
    /* Not 16:9 or 8:9, the main menu's cards, or no widening at all. */
    assert(runtime_game_wide_width("bloom_1_2_target", 960, 544, 5120) == 960);
    assert(runtime_game_wide_width("shadow_array", 1024, 1024, 5120) == 1024);
    assert(runtime_game_wide_width("menu_slide_8", 1728, 972, 5120) == 1728);
    assert(runtime_game_wide_width("dummy_depth_xp_bar", 1728, 972, 5120) == 1728);
    assert(runtime_game_wide_width("mrt_albedo", 1920, 1080, 3840) == 1920);
    assert(runtime_game_wide_width(NULL, 1920, 1080, 7680) == 3840); /* 32:9 */
    /* 3440x1440 (5168): widths stay even. */
    assert(runtime_game_wide_width("x", 960, 540, 5168) == 1292);
    /* 4:3 (2880): narrowed by 3/4, the checkerboard to 1440x2160. */
    assert(runtime_game_wide_width("mrt_albedo", 1920, 1080, 2880) == 1440);
    assert(runtime_game_wide_width("mrt_albedo", 1920, 2160, 2880) == 1440);
    assert(runtime_game_wide_width("ssao_occlusion", 960, 540, 2880) == 720);
    assert(runtime_game_wide_width("menu_slide_8", 1728, 972, 2880) == 1728);

    const size_t size = 0xa5d200;
    unsigned char *image = runtime_low_map(size, 0);
    assert(image);
    memset(image, 0xcc, size);
    /* Not eboot 01.10: nothing is touched. */
    assert(!runtime_game_widen(image, 5120));
    const uint32_t table[4] = {1920, 1080, 3840, 2160};
    memcpy(image + 0xa5d120, table, sizeof table);
    stand_in(image + 0x476aa0, 0x28, 0xc8);
    stand_in(image + 0x478840, 0x18, 0xd0);
    stand_in(image + 0x478910, 0x18, 0xd0);
    assert(!runtime_game_widen(image, 5120)); /* the camera setter is missing yet */
    camera_stand_in(image + 0x420bd0);
    assert(runtime_game_widen(image, 5120));
    const uint32_t *sizes = (const uint32_t *)(image + 0xa5d120);
    assert(sizes[0] == 2560 && sizes[1] == 1080 && sizes[2] == 5120 && sizes[3] == 2160);

    const CreateTarget target = (CreateTarget)(image + 0x476aa0);
    const CreateDepth depth = (CreateDepth)(image + 0x478840), depth_ex = (CreateDepth)(image + 0x478910);
    assert(target(NULL, "mrt_albedo", 0xfac00a, 1920, 1080, NULL) == 2560);
    assert(target(NULL, "menu_slide_1", 0xfac00a, 1728, 960, NULL) == 1728);
    assert(depth(NULL, "mrt_z", 1920, 1080, 1, NULL) == 2560);
    assert(depth_ex(NULL, "mrt_z", 1920, 2160, 1, NULL) == 2560);
    assert(depth(NULL, "dummy_depth", 1728, 960, 1, NULL) == 1728);

    /* The main menu's camera at 2560x1080 as the engine builds it (fov pi/4, aspect 64/27): the
     * setter gets the 16:9 vertical scale the same camera has at 1920x1080 (m5 4.02369) and the
     * fov that gives it with the engine's tan(fov/2)/sqrt(1+aspect); the caller's matrix stays. */
    const SetCamera camera = (SetCamera)(image + 0x420bd0);
    const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float menu[16] = {1.86981f, 0, 0, 0, 0, 4.43215f, 0, 0, 0, 0, 1.07143f, 1, 0, 0, -21.42857f, 0};
    const float aspect = 5120.0f / 2160.0f, fov = 0.785398f;
    const float wide_fov = 2.0f * atanf(tanf(fov * 0.5f) * sqrtf((1.0f + aspect) / (1.0f + 16.0f / 9.0f)));
    assert(close_to(camera(NULL, view, menu, fov, 20, 300, aspect), wide_fov + 4.02369f));
    assert(menu[0] == 1.86981f && menu[5] == 4.43215f);
    /* Cameras of other shapes, and orthographic ones (w = 1), are left alone. */
    assert(close_to(camera(NULL, view, menu, fov, 20, 300, 1.0f), fov + 4.43215f));
    float ortho[16] = {1 / aspect, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1};
    assert(close_to(camera(NULL, view, ortho, 1.5708f, 1, 2, aspect), 1.5708f + 1.0f));

    /* 4:3 on a second image: the same camera at 1440x1080 (aspect 4/3, m5 sqrt(1+a)/tan(fov/2))
     * gets 16:9's horizontal scale instead, so it shows 16:9's width and more above and below:
     * m0 = 4.02369 * 9/16 and m5 = m0 * 4/3. */
    unsigned char *narrow = runtime_low_map(size, 0);
    assert(narrow);
    memset(narrow, 0xcc, size);
    memcpy(narrow + 0xa5d120, table, sizeof table);
    stand_in(narrow + 0x476aa0, 0x28, 0xc8);
    stand_in(narrow + 0x478840, 0x18, 0xd0);
    stand_in(narrow + 0x478910, 0x18, 0xd0);
    camera_stand_in(narrow + 0x420bd0);
    assert(runtime_game_widen(narrow, 2880));
    sizes = (const uint32_t *)(narrow + 0xa5d120);
    assert(sizes[0] == 1440 && sizes[2] == 2880);
    assert(((CreateTarget)(narrow + 0x476aa0))(NULL, "mrt_albedo", 0xfac00a, 1920, 1080, NULL) == 1440);
    assert(((CreateDepth)(narrow + 0x478910))(NULL, "mrt_z", 1920, 2160, 1, NULL) == 1440);
    const float a43 = 4.0f / 3.0f, m5_43 = sqrtf(1.0f + a43) / tanf(fov * 0.5f), m5_out = 4.02369f * 0.75f;
    float menu43[16] = {m5_43 / a43, 0, 0, 0, 0, m5_43, 0, 0, 0, 0, 1.07143f, 1, 0, 0, -21.42857f, 0};
    const float fov43 = 2.0f * atanf(tanf(fov * 0.5f) * m5_43 / m5_out);
    assert(close_to(((SetCamera)(narrow + 0x420bd0))(NULL, view, menu43, fov, 20, 300, a43), fov43 + m5_out));
    /* The screen's orthographic camera (x +-4/3, y +-1) zooms out to 16:9's width: m5 1 -> 0.75. */
    float ortho43[16] = {0.75f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1};
    assert(close_to(((SetCamera)(narrow + 0x420bd0))(NULL, view, ortho43, 1.5708f, 1, 2, a43), 1.5708f + 0.75f));
    assert(ortho43[5] == 1.0f);
    puts("wide: ok");
    return 0;
}
