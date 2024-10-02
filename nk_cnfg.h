#ifdef NK_CNFG_IMPLEMENTATION
#include <stdlib.h>
#include <stdint.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "Nuklear\src\stb_truetype.h"
#define STB_RECT_PACK_IMPLEMENTATION
#include "Nuklear\src\stb_rect_pack.h"
#include "CNFG.h"
#define NK_CNFG_COLOR(color) (uint32_t)(color.r << 24 | color.g << 16 | color.b << 8 | color.a)
#define NK_CNFG_COLOR_SPLIT(color, packed_color) { color.r = ((uint8_t)((packed_color >> 24) & 0xFF)); color.g = ((uint8_t)((packed_color >> 16) & 0xFF)); color.b = ((uint8_t)((packed_color >> 8) & 0xFF)); color.a = ((uint8_t)(packed_color & 0xFF)); }

struct nk_cnfg_font
{
	struct nk_user_font nk;
	stbtt_fontinfo font_info;
	stbtt_pack_context pack_context;
	stbtt_packedchar packed_chars[128];
	uint8_t* ttf_buffer;
	struct nk_vec2i atlas_size;
	uint32_t* atlas_bitmap;
	float scale, font_size;
	int num_characters, first_codepoint;
	int ascent, descent, line_gap;
};

// CNFGTackSegment does not have thickness
NK_INTERN void CNFGTackThickSegment(int x0, int y0, int x1, int y1, int thickness)
{
	float dx = (float)(x1 - x0);
	float dy = (float)(y1 - y0);
	float length = sqrtf(dx * dx + dy * dy);

	if (length == 0)
		return;

	dx /= length;
	dy /= length;

	float px = -dy * (thickness / 2.0f);
	float py = dx * (thickness / 2.0f);

	float x0p1 = x0 + px;
	float y0p1 = y0 + py;
	float x0p2 = x0 - px;
	float y0p2 = y0 - py;

	float x1p1 = x1 + px;
	float y1p1 = y1 + py;
	float x1p2 = x1 - px;
	float y1p2 = y1 - py;

	for (int i = 0; i <= thickness; i++)
	{
		float t = (float)i / thickness;
		float ix0 = x0p1 + t * (x0p2 - x0p1);
		float iy0 = y0p1 + t * (y0p2 - y0p1);
		float ix1 = x1p1 + t * (x1p2 - x1p1);
		float iy1 = y1p1 + t * (y1p2 - y1p1);

		CNFGTackSegment((int)ix0, (int)iy0, (int)ix1, (int)iy1);
	}
}

NK_INTERN void CNFGTackCircle(short x, short y, short radius, int num_segments)
{
	uint32_t points_size = sizeof(RDPoint) * num_segments;
	RDPoint* points = malloc(points_size);
	memset(points, 0, points_size);

	float theta = 2 * NK_PI / num_segments;

	for (int i = 0; i < num_segments; i++)
	{
		float angle = theta * i;
		points[i].x = x + (short)(cosf(angle) * radius);
		points[i].y = y + (short)(sinf(angle) * radius);
	}

	CNFGTackPoly(points, num_segments);
}

NK_INTERN void CNFGTackFilledCircle(short x, short y, short radius, int num_segments)
{
	uint32_t points_size = sizeof(RDPoint) * (num_segments + 1);
	RDPoint* points = malloc(points_size);
	memset(points, 0, points_size);

	points[0].x = x;
	points[0].y = y;

	float theta = 2 * NK_PI / num_segments;

	for (int i = 1; i <= num_segments; i++)
	{
		float angle = theta * (i - 1);
		points[i].x = x + (short)(cosf(angle) * radius);
		points[i].y = y + (short)(sinf(angle) * radius);
	}

	CNFGTackPoly(points, num_segments + 1);
}

NK_INTERN void CNFGTackArc(short x, short y, short radius, float start_angle, float end_angle, int num_segments)
{
	uint32_t points_size = sizeof(RDPoint) * (num_segments + 1);
	RDPoint* points = malloc(points_size);
	memset(points, 0, points_size);

	float theta = (end_angle - start_angle) / num_segments;

	for (int i = 0; i <= num_segments; i++)
	{
		float angle = start_angle + i * theta;
		points[i].x = x + (short)(cosf(angle) * radius);
		points[i].y = y + (short)(sinf(angle) * radius);
	}

	CNFGTackPoly(points, num_segments + 1);
}

NK_INTERN void CNFGTackFilledArc(short x, short y, short radius, float start_angle, float end_angle, int num_segments)
{
	uint32_t points_size = sizeof(RDPoint) * (num_segments + 2);
	RDPoint* points = malloc(points_size);
	memset(points, 0, points_size);

	points[0].x = x;
	points[0].y = y;

	float theta = (end_angle - start_angle) / num_segments;

	for (int i = 1; i <= num_segments + 1; i++)
	{
		float angle = start_angle + (i - 1) * theta;
		points[i].x = x + (short)(cosf(angle) * radius);
		points[i].y = y + (short)(sinf(angle) * radius);
	}

	CNFGTackPoly(points, num_segments + 2);
}

NK_INTERN float nk_cnfg_font_text_width(nk_handle handle, float height, const char* text, int len)
{
	nk_rune unicode;
	int text_len  = 0;
	float text_width = 0;
	int glyph_len = 0;
	float scale = 0;

	struct nk_cnfg_font *font = (struct nk_cnfg_font*)handle.ptr;
	NK_ASSERT(font);
	if (!font || !text || !len)
		return 0;

	scale = stbtt_ScaleForPixelHeight(&font->font_info, height);

	glyph_len = text_len = nk_utf_decode(text, &unicode, (int)len);
	if (!glyph_len) return 0;

	while (text_len <= (int)len && glyph_len)
	{
		int advanceWidth, leftSideBearing;
		if (unicode == NK_UTF_INVALID)
			break;

		stbtt_GetCodepointHMetrics(&font->font_info, unicode, &advanceWidth, &leftSideBearing);
		text_width += advanceWidth * scale;

		glyph_len = nk_utf_decode(text + text_len, &unicode, (int)len - text_len);
		text_len += glyph_len;
	}
	return text_width;
}

NK_INTERN int nk_cnfg_load_file(const char* path, uint8_t** buffer, size_t* sizeOut)
{
	FILE* file = NULL;

	fopen_s(&file, path, "rb");
	if (!file)
	{
		return 0;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);

	*buffer = (uint8_t*)malloc(size);
	fread(*buffer, 1, size, file);
	fclose(file);
	*sizeOut = size;

	return 1;
}

NK_INTERN struct nk_cnfg_font* nk_cnfg_font_load_internal(uint8_t* buffer, size_t buffer_size, float font_size)
{
	struct nk_cnfg_font* f = (struct nk_cnfg_font*)malloc(sizeof(struct nk_cnfg_font));

	f->font_size = font_size;
	f->num_characters = 96;
	f->first_codepoint = 32;

	f->ttf_buffer = (uint8_t*)malloc(buffer_size);
	if (!f->ttf_buffer)
	{
		free(f);
		return NULL;
	}

	memcpy(f->ttf_buffer, buffer, buffer_size);

	if (!stbtt_InitFont(&f->font_info, f->ttf_buffer, stbtt_GetFontOffsetForIndex(f->ttf_buffer, 0)))
	{
		free(f->ttf_buffer);
		free(f);
		return NULL;
	}

	f->scale = stbtt_ScaleForPixelHeight(&f->font_info, f->font_size);

	f->atlas_size = (struct nk_vec2i){ 4096, 4096 };
	
	uint8_t* p_data = (uint8_t*)malloc(f->atlas_size.x * f->atlas_size.y);
	if (!p_data)
	{
		free(p_data);
		free(f->ttf_buffer);
		free(f);
		return NULL;
	}
	size_t atlas_size = f->atlas_size.x * f->atlas_size.y;
	memset(p_data, 0, atlas_size);
	
	stbtt_PackBegin(&f->pack_context, p_data, f->atlas_size.x, f->atlas_size.y, 0, 1, NULL);
	stbtt_PackSetOversampling(&f->pack_context, 2, 2);
	stbtt_pack_range ranges[1];
	ranges[0].chardata_for_range = f->packed_chars;
	ranges[0].array_of_unicode_codepoints = 0;
	ranges[0].first_unicode_codepoint_in_range = f->first_codepoint;
	ranges[0].num_chars = f->num_characters;
	ranges[0].font_size = f->font_size * 1.1f;
	stbtt_PackFontRanges(&f->pack_context, f->ttf_buffer, 0, ranges, 1);
	stbtt_PackEnd(&f->pack_context);

	f->atlas_bitmap = (uint32_t*)malloc(atlas_size * sizeof(uint32_t));
	memset(f->atlas_bitmap, 0, atlas_size * sizeof(uint32_t));
	for (size_t i = 0; i != atlas_size; ++i)
	{
		struct nk_color color = (struct nk_color){ 255, 255, 255, p_data[i] };
		f->atlas_bitmap[i] = NK_CNFG_COLOR(color);
	}

	free(p_data);

	stbtt_GetFontVMetrics(&f->font_info, &f->ascent, &f->descent, &f->line_gap);

	return f;
}

NK_INTERN struct nk_cnfg_font* nk_cnfg_font_load_from_memory(uint8_t* buffer, size_t buffer_size, float font_size)
{
	struct nk_cnfg_font* font = nk_cnfg_font_load_internal(buffer, buffer_size, font_size);
	font->nk.userdata = nk_handle_ptr(font);
	font->nk.height = font_size;
	font->nk.width = nk_cnfg_font_text_width;
}

NK_INTERN struct nk_cnfg_font* nk_cnfg_font_load_from_file(const char* font_path, float font_size)
{
	uint8_t* buffer = NULL;
	size_t buffer_size = 0;
	if (!nk_cnfg_load_file(font_path, &buffer, &buffer_size))
	{
		return NULL;
	}

	struct nk_cnfg_font* font = nk_cnfg_font_load_from_memory(buffer, buffer_size, font_size);

	free(buffer);

	return font;
}

NK_INTERN void nk_cnfg_set_font(struct nk_context* ctx, struct nk_cnfg_font* font)
{
	nk_style_set_font(ctx, &font->nk);
}

NK_INTERN void nk_cnfg_font_destroy(struct nk_cnfg_font* f)
{
	free(f->ttf_buffer);
	free(f->atlas_bitmap);
	free(f);
}

#define NK_CNFG_PIXEL_ROUND(x) (float)((int)(x))
NK_INTERN void nk_cnfg_render_character(struct nk_cnfg_font* f, float posX, float posY, char character, uint32_t color)
{
	if (character < 32 || character >= 128)
		return;

	stbtt_packedchar b = f->packed_chars[character - f->first_codepoint];

	float x = NK_CNFG_PIXEL_ROUND(posX + b.xoff);
	float y = NK_CNFG_PIXEL_ROUND(posY + b.yoff);
	float x2 = x + (b.xoff2 - b.xoff);
	float y2 = y + (b.yoff2 - b.yoff);

	float s0 = b.x0 / (float)f->atlas_size.x;
	float t0 = b.y0 / (float)f->atlas_size.y;
	float s1 = b.x1 / (float)f->atlas_size.x;
	float t1 = b.y1 / (float)f->atlas_size.y;

	CNFGColor(color);
	for (int y_pixel = (int)y; y_pixel != (int)y2; ++y_pixel)
	{
		for (int x_pixel = (int)x; x_pixel != (int)x2; ++x_pixel)
		{
			int atlasX = (int)((s0 + (float)(x_pixel - x) / (x2 - x) * (s1 - s0)) * f->atlas_size.x);
			int atlasY = (int)((t0 + (float)(y_pixel - y) / (y2 - y) * (t1 - t0)) * f->atlas_size.y);

			uint32_t atlas_packed_color = f->atlas_bitmap[atlasY * f->atlas_size.x + atlasX];
			struct nk_color atlas_color;
			NK_CNFG_COLOR_SPLIT(atlas_color, atlas_packed_color);

			if (atlas_color.a > 0)
			{
				CNFGTackPixel(x_pixel, y_pixel);
			}
		}
	}
}

NK_INTERN void nk_cnfg_render_string(struct nk_cnfg_font* f, int posX, int posY, const char* text, uint32_t color)
{
	float scaledAscent = (float)(f->ascent - f->line_gap) * f->scale;
	float xpos = (float)posX;
	float ypos = (float)posY + scaledAscent;

	for (size_t i = 0; text[i] >= f->first_codepoint && text[i] < (f->first_codepoint + f->num_characters); ++i)
	{
		stbtt_packedchar b = f->packed_chars[text[i] - f->first_codepoint];
		nk_cnfg_render_character(f, xpos, ypos, text[i], color);
		xpos += b.xadvance;
	}
}

short scissor_x = 0, scissor_y = 0;
short scissor_w = 0, scissor_h = 0;
int scissor_enabled = 0;

NK_INTERN void nk_cnfg_scissor_cmd(const struct nk_command_scissor* cmd, struct nk_context* ctx)
{
	// Store the scissor region
	scissor_x = (short)cmd->x;
	scissor_y = (short)cmd->y;
	scissor_w = (short)cmd->w;
	scissor_h = (short)cmd->h;
	scissor_enabled = 1;  // Enable clipping
}

NK_INTERN int point_in_scissor(short x, short y)
{
	if (!scissor_enabled)
		return 1; // No scissor region, allow all points
	return (x >= scissor_x && x <= (scissor_x + scissor_w) &&
		y >= scissor_y && y <= (scissor_y + scissor_h));
}

NK_INTERN int rect_in_scissor(short x, short y, short w, short h)
{
	if (!scissor_enabled)
		return 1; // No scissor region, allow all rectangles

	// Check if the rectangle overlaps with the scissor region
	short x2 = x + w;
	short y2 = y + h;
	return !(x2 < scissor_x || x > (scissor_x + scissor_w) ||
		y2 < scissor_y || y > (scissor_y + scissor_h));
}

NK_INTERN void nk_cnfg_line_cmd(const struct nk_command_line* cmd, struct nk_context* ctx)
{
	if (!point_in_scissor(cmd->begin.x, cmd->begin.y) || !point_in_scissor(cmd->end.x, cmd->end.y))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackThickSegment(cmd->begin.x, cmd->begin.y, cmd->end.x, cmd->end.y, cmd->line_thickness);
}

NK_INTERN void nk_cnfg_curve_cmd(const struct nk_command_curve* cmd, struct nk_context* ctx)
{
	if (!point_in_scissor(cmd->begin.x, cmd->begin.y) || !point_in_scissor(cmd->end.x, cmd->end.y))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);

	float x0 = (float)cmd->begin.x;
	float y0 = (float)cmd->begin.y;
	float cx0 = (float)cmd->ctrl[0].x;
	float cy0 = (float)cmd->ctrl[0].y;
	float cx1 = (float)cmd->ctrl[1].x;
	float cy1 = (float)cmd->ctrl[1].y;
	float x1 = (float)cmd->end.x;
	float y1 = (float)cmd->end.y;

	float prev_x = x0;
	float prev_y = y0;

	for (int i = 1; i <= 32; i++)
	{
		float t = (float)i / 32.f;

		float t2 = t * t;
		float t3 = t2 * t;

		float u = 1 - t;
		float u2 = u * u;
		float u3 = u2 * u;

		float cur_x = u3 * x0 + 3 * u2 * t * cx0 + 3 * u * t2 * cx1 + t3 * x1;
		float cur_y = u3 * y0 + 3 * u2 * t * cy0 + 3 * u * t2 * cy1 + t3 * y1;

		CNFGTackSegment(prev_x, prev_y, cur_x, cur_y);

		prev_x = cur_x;
		prev_y = cur_y;
	}
}

NK_INTERN void nk_cnfg_rect_cmd(const struct nk_command_rect* cmd, struct nk_context* ctx)
{
	if (!rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackSegment(cmd->x, cmd->y, cmd->x + cmd->w, cmd->y);              
	CNFGTackSegment(cmd->x, cmd->y + cmd->h, cmd->x + cmd->w, cmd->y + cmd->h);
	CNFGTackSegment(cmd->x, cmd->y, cmd->x, cmd->y + cmd->h);              
	CNFGTackSegment(cmd->x + cmd->w, cmd->y, cmd->x + cmd->w, cmd->y + cmd->h);
}

NK_INTERN void nk_cnfg_rect_filled_cmd(const struct nk_command_rect_filled* cmd, struct nk_context* ctx)
{
	if (!rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackRectangle(cmd->x, cmd->y, cmd->x + cmd->w, cmd->y + cmd->h);
}

NK_INTERN void nk_cnfg_rect_multi_color_cmd(const struct nk_command_rect_multi_color* cmd, struct nk_context* ctx)
{
	if (!rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint8_t lt_r = (cmd->left.r), lt_g = (cmd->left.g), lt_b = (cmd->left.b), lt_a = (cmd->left.a);
	uint8_t rt_r = (cmd->right.r), rt_g = (cmd->right.g), rt_b = (cmd->right.b), rt_a = (cmd->right.a);
	uint8_t lb_r = (cmd->bottom.r), lb_g = (cmd->bottom.g), lb_b = (cmd->bottom.b), lb_a = (cmd->bottom.a);
	uint8_t tb_r = (cmd->top.r), tb_g = (cmd->top.g), tb_b = (cmd->top.b), tb_a = (cmd->top.a);

	for (int y = 0; y != cmd->h; y++)
	{
		float v_factor = (float)y / (float)(cmd->h);

		uint8_t row_left_r = (uint8_t)((1 - v_factor) * lt_r + v_factor * lb_r);
		uint8_t row_left_g = (uint8_t)((1 - v_factor) * lt_g + v_factor * lb_g);
		uint8_t row_left_b = (uint8_t)((1 - v_factor) * lt_b + v_factor * lb_b);
		uint8_t row_left_a = (uint8_t)((1 - v_factor) * lt_a + v_factor * lb_a);

		uint8_t row_right_r = (uint8_t)((1 - v_factor) * tb_r + v_factor * rt_r);
		uint8_t row_right_g = (uint8_t)((1 - v_factor) * tb_g + v_factor * rt_g);
		uint8_t row_right_b = (uint8_t)((1 - v_factor) * tb_b + v_factor * rt_b);
		uint8_t row_right_a = (uint8_t)((1 - v_factor) * tb_a + v_factor * rt_a);

		for (int x = 0; x != cmd->w; x++)
		{
			float h_factor = (float)x / (float)(cmd->w);

			uint8_t pixel_r = (uint8_t)((1 - h_factor) * row_left_r + h_factor * row_right_r);
			uint8_t pixel_g = (uint8_t)((1 - h_factor) * row_left_g + h_factor * row_right_g);
			uint8_t pixel_b = (uint8_t)((1 - h_factor) * row_left_b + h_factor * row_right_b);
			uint8_t pixel_a = (uint8_t)((1 - h_factor) * row_left_a + h_factor * row_right_a);

			uint32_t color = (pixel_r << 24) | (pixel_g << 16) | (pixel_b << 8) | pixel_a;
			CNFGColor(color);
			CNFGTackPixel(cmd->x + x, cmd->y + y);
		}
	}
}

NK_INTERN void nk_cnfg_circle_cmd(const struct nk_command_circle* cmd, struct nk_context* ctx)
{
	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackCircle(cmd->x + cmd->w / 2, cmd->y + cmd->h / 2, cmd->w / 2, 32); // 32 segments for a smooth circle
}

NK_INTERN void nk_cnfg_circle_filled_cmd(const struct nk_command_circle_filled* cmd, struct nk_context* ctx)
{
	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackFilledCircle(cmd->x + cmd->w / 2, cmd->y + cmd->h / 2, cmd->w / 2, 32); // 32 segments for a smooth filled circle
}

NK_INTERN void nk_cnfg_arc_cmd(const struct nk_command_arc* cmd, struct nk_context* ctx)
{
	if (!point_in_scissor(cmd->cx, cmd->cy))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackArc(cmd->cx, cmd->cy, cmd->r, cmd->a[0], cmd->a[1], 32); // Use 32 segments for a smooth arc
}

NK_INTERN void nk_cnfg_arc_filled_cmd(const struct nk_command_arc_filled* cmd, struct nk_context* ctx)
{
	if (!point_in_scissor(cmd->cx, cmd->cy))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackFilledArc(cmd->cx, cmd->cy, cmd->r, cmd->a[0], cmd->a[1], 32); // Use 32 segments for a smooth filled arc
}

NK_INTERN void nk_cnfg_triangle_cmd(const struct nk_command_triangle* cmd, struct nk_context* ctx)
{
	if (!point_in_scissor(cmd->a.x, cmd->a.y) ||  !point_in_scissor(cmd->b.x, cmd->b.y) || !point_in_scissor(cmd->c.x, cmd->c.y))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackSegment(cmd->a.x, cmd->a.y, cmd->b.x, cmd->b.y);
	CNFGTackSegment(cmd->b.x, cmd->b.y, cmd->c.x, cmd->c.y);
	CNFGTackSegment(cmd->c.x, cmd->c.y, cmd->a.x, cmd->a.y);
}

NK_INTERN void nk_cnfg_triangle_filled_cmd(const struct nk_command_triangle_filled* cmd, struct nk_context* ctx)
{
	if (!point_in_scissor(cmd->a.x, cmd->a.y) ||  !point_in_scissor(cmd->b.x, cmd->b.y) ||  !point_in_scissor(cmd->c.x, cmd->c.y))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	RDPoint points[3] = {
		{ cmd->a.x, cmd->a.y },
		{ cmd->b.x, cmd->b.y },
		{ cmd->c.x, cmd->c.y }
	};
	CNFGTackPoly(points, 3);
}

NK_INTERN void nk_cnfg_polygon_cmd(const struct nk_command_polygon* cmd, struct nk_context* ctx)
{
	if (cmd->point_count < 3)
		return;

	for (int i = 0; i != cmd->point_count; i++)
	{
		if (!point_in_scissor(cmd->points[i].x, cmd->points[i].y))
			return;
	}

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);

	for (int i = 0; i != cmd->point_count - 1; i++)
	{
		CNFGTackSegment(cmd->points[i].x, cmd->points[i].y, cmd->points[i + 1].x, cmd->points[i + 1].y);
	}
	CNFGTackSegment(cmd->points[cmd->point_count - 1].x, cmd->points[cmd->point_count - 1].y, cmd->points[0].x, cmd->points[0].y);
}

NK_INTERN void nk_cnfg_polygon_filled_cmd(const struct nk_command_polygon_filled* cmd, struct nk_context* ctx)
{
	if (cmd->point_count < 3)
		return;

	for (int i = 0; i != cmd->point_count; i++)
	{
		if (!point_in_scissor(cmd->points[i].x, cmd->points[i].y))
			return;
	}

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackPoly((RDPoint*)&cmd->points[0], cmd->point_count);
}

NK_INTERN void nk_cnfg_polyline_cmd(const struct nk_command_polyline* cmd, struct nk_context* ctx)
{
	if (cmd->point_count < 2)
		return;

	for (int i = 0; i != cmd->point_count; i++)
	{
		if (!point_in_scissor(cmd->points[i].x, cmd->points[i].y))
			return;
	}

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);

	for (int i = 0; i < cmd->point_count - 1; i++)
	{
		CNFGTackThickSegment(cmd->points[i].x, cmd->points[i].y, cmd->points[i + 1].x, cmd->points[i + 1].y, cmd->line_thickness);
	}
}

NK_INTERN void nk_cnfg_text_cmd(const struct nk_command_text* cmd, struct nk_context* ctx)
{
	if (!rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->foreground);
	struct nk_cnfg_font* f = (struct nk_cnfg_font*)ctx->style.font->userdata.ptr;
	nk_cnfg_render_string(f, cmd->x, cmd->y, cmd->string, color);
}

NK_INTERN void nk_cnfg_image_cmd(const struct nk_command_image* cmd, struct nk_context* ctx)
{
	if (!rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->col);
	CNFGBlitImage(cmd->img.handle.ptr, cmd->x, cmd->y, cmd->w, cmd->h);
}

struct nk_colorf bg_color;
NK_INTERN void nk_cnfg_render(struct nk_context* ctx)
{
	const struct nk_command* cmd;

	// Clear screen (fill with black background)
	struct nk_color color = nk_rgb_cf(bg_color);
	CNFGBGColor = NK_CNFG_COLOR(color);
	CNFGClearFrame();

	// Iterate through and handle Nuklear drawing commands
	nk_foreach(cmd, ctx)
	{
		switch (cmd->type)
		{
			case NK_COMMAND_NOP:
			{
				// Do nothing (NOP)
			} break;

			case NK_COMMAND_SCISSOR:
			{
				const struct nk_command_scissor* s = (const struct nk_command_scissor*)cmd;
				nk_cnfg_scissor_cmd(s, ctx);
			} break;

			case NK_COMMAND_LINE:
			{
				const struct nk_command_line* l = (const struct nk_command_line*)cmd;
				nk_cnfg_line_cmd(l, ctx);
			} break;

			case NK_COMMAND_CURVE:
			{
				const struct nk_command_curve* c = (const struct nk_command_curve*)cmd;
				nk_cnfg_curve_cmd(c, ctx);
			} break;

			case NK_COMMAND_RECT:
			{
				const struct nk_command_rect* r = (const struct nk_command_rect*)cmd;
				nk_cnfg_rect_cmd(r, ctx);
			} break;

			case NK_COMMAND_RECT_FILLED:
			{
				const struct nk_command_rect_filled* r = (const struct nk_command_rect_filled*)cmd;
				nk_cnfg_rect_filled_cmd(r, ctx);
			} break;

			case NK_COMMAND_RECT_MULTI_COLOR:
			{
				const struct nk_command_rect_multi_color* r = (const struct nk_command_rect_multi_color*)cmd;
				nk_cnfg_rect_multi_color_cmd(r, ctx);
			} break;

			case NK_COMMAND_CIRCLE:
			{
				const struct nk_command_circle* c = (const struct nk_command_circle*)cmd;
				nk_cnfg_circle_cmd(c, ctx);
			} break;

			case NK_COMMAND_CIRCLE_FILLED:
			{
				const struct nk_command_circle_filled* c = (const struct nk_command_circle_filled*)cmd;
				nk_cnfg_circle_filled_cmd(c, ctx);
			} break;

			case NK_COMMAND_ARC:
			{
				const struct nk_command_arc* a = (const struct nk_command_arc*)cmd;
				nk_cnfg_arc_cmd(a, ctx);
			} break;

			case NK_COMMAND_ARC_FILLED:
			{
				const struct nk_command_arc_filled* a = (const struct nk_command_arc_filled*)cmd;
				nk_cnfg_arc_filled_cmd(a, ctx);
			} break;

			case NK_COMMAND_TRIANGLE:
			{
				const struct nk_command_triangle* t = (const struct nk_command_triangle*)cmd;
				nk_cnfg_triangle_cmd(t, ctx);
			} break;

			case NK_COMMAND_TRIANGLE_FILLED:
			{
				const struct nk_command_triangle_filled* t = (const struct nk_command_triangle_filled*)cmd;
				nk_cnfg_triangle_filled_cmd(t, ctx);
			} break;

			case NK_COMMAND_POLYGON:
			{
				const struct nk_command_polygon* p = (const struct nk_command_polygon*)cmd;
				nk_cnfg_polygon_cmd(p, ctx);
			} break;

			case NK_COMMAND_POLYGON_FILLED:
			{
				const struct nk_command_polygon_filled* p = (const struct nk_command_polygon_filled*)cmd;
				nk_cnfg_polygon_filled_cmd(p, ctx);
			} break;

			case NK_COMMAND_POLYLINE:
			{
				const struct nk_command_polyline* p = (const struct nk_command_polyline*)cmd;
				nk_cnfg_polyline_cmd(p, ctx);
			} break;

			case NK_COMMAND_TEXT:
			{
				const struct nk_command_text* t = (const struct nk_command_text*)cmd;
				nk_cnfg_text_cmd(t, ctx);
			} break;

			case NK_COMMAND_IMAGE:
			{
				const struct nk_command_image* i = (const struct nk_command_image*)cmd;
				nk_cnfg_image_cmd(i, ctx);
			} break;

			case NK_COMMAND_CUSTOM:
			{
				// Not needed, do nothing
			} break;
		}
	}

	// Swap buffers
	CNFGSwapBuffers();
}

void nk_cnfg_input_key(struct nk_context* ctx, int keycode, int bDown)
{
	switch (keycode)
	{
		case CNFG_KEY_LEFT_ARROW:
		nk_input_key(ctx, NK_KEY_LEFT, bDown);
		break;
		case CNFG_KEY_RIGHT_ARROW:
		nk_input_key(ctx, NK_KEY_RIGHT, bDown);
		break;
		case CNFG_KEY_TOP_ARROW:
		nk_input_key(ctx, NK_KEY_UP, bDown);
		break;
		case CNFG_KEY_BOTTOM_ARROW:
		nk_input_key(ctx, NK_KEY_DOWN, bDown);
		break;
		case CNFG_KEY_ENTER:
		nk_input_key(ctx, NK_KEY_ENTER, bDown);
		break;
		case CNFG_KEY_BACKSPACE:
		nk_input_key(ctx, NK_KEY_BACKSPACE, bDown);
		break;
		default:
		// Handle other keys if needed
		break;
	}
}

// Wrapping HandleButton for Nuklear context
void nk_cnfg_input_button(struct nk_context* ctx, int x, int y, int button, int bDown)
{
	switch (button)
	{
		case 1:  // Left mouse button
		nk_input_button(ctx, NK_BUTTON_LEFT, x, y, bDown);
		break;
		case 2:  // Right mouse button
		nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, bDown);
		break;
		case 3:  // Middle mouse button
		nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, bDown);
		break;
		default:
		// Handle additional buttons if necessary
		break;
	}
}

// Wrapping HandleMotion for Nuklear context
void nk_cnfg_input_motion(struct nk_context* ctx, int x, int y)
{
	nk_input_motion(ctx, x, y);
}

// Wrapping HandleScroll for Nuklear context
void nk_cnfg_input_scroll(struct nk_context* ctx, float scroll_x, float scroll_y)
{
	struct nk_vec2 scroll = nk_vec2(scroll_x, scroll_y);
	nk_input_scroll(ctx, scroll);
}

// Wrapping HandleChar for Nuklear context (text input)
void nk_cnfg_input_char(struct nk_context* ctx, char c)
{
	nk_input_char(ctx, c);
}

// Nuklear Cleanup (HandleDestroy wrapper)
int nk_cnfg_input_destroy(struct nk_context* ctx)
{
	// Cleanup code for Nuklear context if needed (e.g., freeing resources)
	// Currently just return 0 unless error handling is required
	return 0;
}

NK_INTERN void nk_cnfg_init(const char* title, int width, int height, struct nk_context* ctx)
{
	nk_init_default(ctx, NULL);

	nk_style_default(ctx);

	CNFGSetup(title, width, height);
}

#endif