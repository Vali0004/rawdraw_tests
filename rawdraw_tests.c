#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "os_generic.h"
#define WINDOW_WIDTH  1920
#define WINDOW_HEIGHT 1080

// Make it so we don't need to include any other C files in our build.
#define CNFG_IMPLEMENTATION

#ifdef _MSC_VER
#define alloca malloc
#endif

#include "CNFG.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#define NK_CNFG_IMPLEMENTATION
#include "nuklear.h"
#include "nk_cnfg.h"

struct nk_context* ctx;
void HandleKey(int keycode, int bDown)
{
	nk_cnfg_input_key(ctx, keycode, bDown);
}
void HandleButton(int x, int y, int button, int bDown)
{
	nk_cnfg_input_button(ctx, x, y, button, bDown);
}
void HandleMotion(int x, int y, int mask)
{
	nk_cnfg_input_motion(ctx, x, y);
}
int HandleDestroy()
{
	return 0;
}

struct nk_cnfg_font* default_font;
int main()
{
	struct nk_context _ctx;
	ctx = &_ctx;

	nk_cnfg_init("Nuklear + RawDraw", WINDOW_WIDTH, WINDOW_HEIGHT, ctx);

	default_font = nk_cnfg_font_load_from_file("C:\\Windows\\Fonts\\arial.ttf", 20.f);
	nk_cnfg_set_font(ctx, default_font);

	printf("Init nuklear and RawDraw\n");

	// Main event loop
	while (CNFGHandleInput())
	{
		if (nk_begin(ctx, "Demo", nk_rect(50, 50, 230, 250),
			NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|
			NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
		{
			enum {EASY, HARD};
			static int op = EASY;
			static int property = 20;

			nk_layout_row_static(ctx, 30, 80, 1);
			if (nk_button_label(ctx, "button"))
				fprintf(stdout, "button pressed\n");
			nk_layout_row_dynamic(ctx, 30, 2);
			if (nk_option_label(ctx, "easy", op == EASY)) op = EASY;
			if (nk_option_label(ctx, "hard", op == HARD)) op = HARD;
			nk_layout_row_dynamic(ctx, 22, 1);
			nk_property_int(ctx, "Compression:", 0, &property, 100, 10, 1);

			nk_layout_row_dynamic(ctx, 20, 1);
			nk_label(ctx, "background:", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, 25, 1);
			if (nk_combo_begin_color(ctx, nk_rgb_cf(bg_color), nk_vec2(nk_widget_width(ctx),400))) {
				nk_layout_row_dynamic(ctx, 120, 1);
				bg_color = nk_color_picker(ctx, bg_color, NK_RGBA);
				nk_layout_row_dynamic(ctx, 25, 1);
				bg_color.r = nk_propertyf(ctx, "#R:", 0, bg_color.r, 1.0f, 0.01f,0.005f);
				bg_color.g = nk_propertyf(ctx, "#G:", 0, bg_color.g, 1.0f, 0.01f,0.005f);
				bg_color.b = nk_propertyf(ctx, "#B:", 0, bg_color.b, 1.0f, 0.01f,0.005f);
				bg_color.a = nk_propertyf(ctx, "#A:", 0, bg_color.a, 1.0f, 0.01f,0.005f);
				nk_combo_end(ctx);
			}
		}
		nk_end(ctx);

		nk_cnfg_render(ctx);
	}

	// Cleanup
	nk_cnfg_font_destroy(default_font);
	nk_free(ctx);
	printf("Exiting\n");
}
