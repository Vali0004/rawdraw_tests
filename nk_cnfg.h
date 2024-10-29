#ifdef NK_CNFG_IMPLEMENTATION
#include <stdlib.h>
#include <stdint.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "Nuklear/src/stb_truetype.h"
#define STB_RECT_PACK_IMPLEMENTATION
#include "Nuklear/src/stb_rect_pack.h"
#include "CNFG.h"
#include "nk_decompression.h"
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

NK_INTERN uint32_t CNFGBlendAlpha(uint32_t color, uint8_t alpha)
{
	uint8_t r = (color >> 24) & 0xFF;
	uint8_t g = (color >> 16) & 0xFF;
	uint8_t b = (color >> 8) & 0xFF;
	uint8_t a = color & 0xFF;

	r = (r * alpha) / 255;
	g = (g * alpha) / 255;
	b = (b * alpha) / 255;
	a = (a * alpha) / 255;

	return (r << 24) | (g << 16) | (b << 8) | a;
}

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

	float px = -dy * (thickness / 2.f);
	float py = dx * (thickness / 2.f);

	float x0p1 = x0 + px;
	float y0p1 = y0 + py;
	float x0p2 = x0 - px;
	float y0p2 = y0 - py;

	float x1p1 = x1 + px;
	float y1p1 = y1 + py;
	float x1p2 = x1 - px;
	float y1p2 = y1 - py;

	for (int i = 0; i <= thickness; ++i)
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

	float theta = 2.f * (NK_PI / num_segments);

	for (int i = 0; i != num_segments; ++i)
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

	points[0] = (RDPoint){ x, y };

	float theta = 2.f * (NK_PI / num_segments);

	for (int i = 1; i <= num_segments; ++i)
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
	if (!glyph_len)
		return 0;

	while (text_len <= (int)len && glyph_len)
	{
		int advance_width, left_side_bearing;
		if (unicode == NK_UTF_INVALID)
			break;

		int ch = unicode - font->first_codepoint;
		if (ch > sizeof(font->packed_chars) / sizeof(font->packed_chars[0]))
			continue;

		stbtt_packedchar b = font->packed_chars[ch];
		text_width += b.xadvance;

		glyph_len = nk_utf_decode(text + text_len, &unicode, len - text_len);
		text_len += glyph_len;
	}
	return text_width;
}

NK_INTERN int nk_cnfg_load_file(const char* path, uint8_t** buffer, nk_size* sizeOut)
{
	FILE* file = fopen(path, "rb");

	NK_ASSERT(file);
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

NK_INTERN struct nk_cnfg_font* nk_cnfg_font_load_internal(uint8_t* buffer, nk_size buffer_size, float font_size)
{
	struct nk_cnfg_font* f = (struct nk_cnfg_font*)malloc(sizeof(struct nk_cnfg_font));

	f->font_size = font_size;
	f->num_characters = 96;
	f->first_codepoint = 32;

	f->ttf_buffer = (uint8_t*)malloc(buffer_size);
	NK_ASSERT(f->ttf_buffer);
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

	nk_size atlas_size = f->atlas_size.x * f->atlas_size.y;

	uint8_t* p_data = (uint8_t*)malloc(atlas_size);
	NK_ASSERT(p_data);
	if (!p_data)
	{
		free(p_data);
		free(f->ttf_buffer);
		free(f);
		return NULL;
	}
	memset(p_data, 0, atlas_size);
	
	stbtt_PackBegin(&f->pack_context, p_data, f->atlas_size.x, f->atlas_size.y, 0, 1, NULL);
	stbtt_pack_range ranges[1];
	ranges[0].chardata_for_range = f->packed_chars;
	ranges[0].array_of_unicode_codepoints = 0;
	ranges[0].first_unicode_codepoint_in_range = f->first_codepoint;
	ranges[0].num_chars = f->num_characters;
	ranges[0].font_size = f->font_size;
	stbtt_PackFontRanges(&f->pack_context, f->ttf_buffer, 0, ranges, 1);
	stbtt_PackEnd(&f->pack_context);

	f->atlas_bitmap = (uint32_t*)malloc(atlas_size * sizeof(uint32_t));
	memset(f->atlas_bitmap, 0, atlas_size * sizeof(uint32_t));
	for (nk_size i = 0; i != atlas_size; ++i)
	{
		struct nk_color color = (struct nk_color){ 255, 255, 255, p_data[i] };
		f->atlas_bitmap[i] = NK_CNFG_COLOR(color);
	}

	free(p_data);

	stbtt_GetFontVMetrics(&f->font_info, &f->ascent, &f->descent, &f->line_gap);

	return f;
}

NK_API struct nk_cnfg_font* nk_cnfg_font_load_from_memory(uint8_t* buffer, nk_size buffer_size, float font_size)
{
	struct nk_cnfg_font* font = nk_cnfg_font_load_internal(buffer, buffer_size, font_size);
	font->nk.userdata = nk_handle_ptr(font);
	font->nk.height = font_size;
	font->nk.width = nk_cnfg_font_text_width;
	return font;
}

NK_API struct nk_cnfg_font* nk_cnfg_font_load_from_compressed_memory(uint8_t* compressed_data, nk_size compressed_size, float height)
{
	uint32_t decompressed_size;
	uint8_t* decompressed_data;

	decompressed_size = nk_decompress_length((uint8_t*)compressed_data);
	decompressed_data = malloc(decompressed_size);
	NK_ASSERT(decompressed_data);
	if (!decompressed_data)
		return 0;
	nk_decompress(decompressed_data, compressed_data,
		(uint32_t)compressed_size);

	return nk_cnfg_font_load_from_memory(decompressed_data, decompressed_size, height);
}

NK_API struct nk_cnfg_font* nk_cnfg_font_load_from_compressed_base85(const char* data_base85, float height)
{
	uint32_t compressed_size;
	uint8_t* compressed_data;
	struct nk_cnfg_font* font;

	NK_ASSERT(data_base85);
	if (!data_base85)
		return 0;

	compressed_size = (((int)nk_strlen(data_base85) + 4) / 5) * 4;
	compressed_data = malloc(compressed_size);
	NK_ASSERT(compressed_data);
	if (!compressed_data)
		return 0;
	nk_decode_85(compressed_data, (const uint8_t*)data_base85);
	font = nk_cnfg_font_load_from_compressed_memory(compressed_data, compressed_size, height);
	free(compressed_data);

	return font;
}

NK_INTERN struct nk_cnfg_font* nk_cnfg_font_load_from_file(const char* font_path, float font_size)
{
	uint8_t* buffer = NULL;
	nk_size buffer_size = 0;
	if (!nk_cnfg_load_file(font_path, &buffer, &buffer_size))
	{
		return NULL;
	}

	struct nk_cnfg_font* font = nk_cnfg_font_load_from_memory(buffer, buffer_size, font_size);

	free(buffer);

	return font;
}

#ifdef NK_INCLUDE_DEFAULT_FONT

NK_GLOBAL const char nk_proggy_clean_ttf_compressed_data_base85[11980 + 1] =
"7])#######hV0qs'/###[),##/l:$#Q6>##5[n42>c-TH`->>#/e>11NNV=Bv(*:.F?uu#(gRU.o0XGH`$vhLG1hxt9?W`#,5LsCp#-i>.r$<$6pD>Lb';9Crc6tgXmKVeU2cD4Eo3R/"
"2*>]b(MC;$jPfY.;h^`IWM9<Lh2TlS+f-s$o6Q<BWH`YiU.xfLq$N;$0iR/GX:U(jcW2p/W*q?-qmnUCI;jHSAiFWM.R*kU@C=GH?a9wp8f$e.-4^Qg1)Q-GL(lf(r/7GrRgwV%MS=C#"
"`8ND>Qo#t'X#(v#Y9w0#1D$CIf;W'#pWUPXOuxXuU(H9M(1<q-UE31#^-V'8IRUo7Qf./L>=Ke$$'5F%)]0^#0X@U.a<r:QLtFsLcL6##lOj)#.Y5<-R&KgLwqJfLgN&;Q?gI^#DY2uL"
"i@^rMl9t=cWq6##weg>$FBjVQTSDgEKnIS7EM9>ZY9w0#L;>>#Mx&4Mvt//L[MkA#W@lK.N'[0#7RL_&#w+F%HtG9M#XL`N&.,GM4Pg;-<nLENhvx>-VsM.M0rJfLH2eTM`*oJMHRC`N"
"kfimM2J,W-jXS:)r0wK#@Fge$U>`w'N7G#$#fB#$E^$#:9:hk+eOe--6x)F7*E%?76%^GMHePW-Z5l'&GiF#$956:rS?dA#fiK:)Yr+`&#0j@'DbG&#^$PG.Ll+DNa<XCMKEV*N)LN/N"
"*b=%Q6pia-Xg8I$<MR&,VdJe$<(7G;Ckl'&hF;;$<_=X(b.RS%%)###MPBuuE1V:v&cX&#2m#(&cV]`k9OhLMbn%s$G2,B$BfD3X*sp5#l,$R#]x_X1xKX%b5U*[r5iMfUo9U`N99hG)"
"tm+/Us9pG)XPu`<0s-)WTt(gCRxIg(%6sfh=ktMKn3j)<6<b5Sk_/0(^]AaN#(p/L>&VZ>1i%h1S9u5o@YaaW$e+b<TWFn/Z:Oh(Cx2$lNEoN^e)#CFY@@I;BOQ*sRwZtZxRcU7uW6CX"
"ow0i(?$Q[cjOd[P4d)]>ROPOpxTO7Stwi1::iB1q)C_=dV26J;2,]7op$]uQr@_V7$q^%lQwtuHY]=DX,n3L#0PHDO4f9>dC@O>HBuKPpP*E,N+b3L#lpR/MrTEH.IAQk.a>D[.e;mc."
"x]Ip.PH^'/aqUO/$1WxLoW0[iLA<QT;5HKD+@qQ'NQ(3_PLhE48R.qAPSwQ0/WK?Z,[x?-J;jQTWA0X@KJ(_Y8N-:/M74:/-ZpKrUss?d#dZq]DAbkU*JqkL+nwX@@47`5>w=4h(9.`G"
"CRUxHPeR`5Mjol(dUWxZa(>STrPkrJiWx`5U7F#.g*jrohGg`cg:lSTvEY/EV_7H4Q9[Z%cnv;JQYZ5q.l7Zeas:HOIZOB?G<Nald$qs]@]L<J7bR*>gv:[7MI2k).'2($5FNP&EQ(,)"
"U]W]+fh18.vsai00);D3@4ku5P?DP8aJt+;qUM]=+b'8@;mViBKx0DE[-auGl8:PJ&Dj+M6OC]O^((##]`0i)drT;-7X`=-H3[igUnPG-NZlo.#k@h#=Ork$m>a>$-?Tm$UV(?#P6YY#"
"'/###xe7q.73rI3*pP/$1>s9)W,JrM7SN]'/4C#v$U`0#V.[0>xQsH$fEmPMgY2u7Kh(G%siIfLSoS+MK2eTM$=5,M8p`A.;_R%#u[K#$x4AG8.kK/HSB==-'Ie/QTtG?-.*^N-4B/ZM"
"_3YlQC7(p7q)&](`6_c)$/*JL(L-^(]$wIM`dPtOdGA,U3:w2M-0<q-]L_?^)1vw'.,MRsqVr.L;aN&#/EgJ)PBc[-f>+WomX2u7lqM2iEumMTcsF?-aT=Z-97UEnXglEn1K-bnEO`gu"
"Ft(c%=;Am_Qs@jLooI&NX;]0#j4#F14;gl8-GQpgwhrq8'=l_f-b49'UOqkLu7-##oDY2L(te+Mch&gLYtJ,MEtJfLh'x'M=$CS-ZZ%P]8bZ>#S?YY#%Q&q'3^Fw&?D)UDNrocM3A76/"
"/oL?#h7gl85[qW/NDOk%16ij;+:1a'iNIdb-ou8.P*w,v5#EI$TWS>Pot-R*H'-SEpA:g)f+O$%%`kA#G=8RMmG1&O`>to8bC]T&$,n.LoO>29sp3dt-52U%VM#q7'DHpg+#Z9%H[K<L"
"%a2E-grWVM3@2=-k22tL]4$##6We'8UJCKE[d_=%wI;'6X-GsLX4j^SgJ$##R*w,vP3wK#iiW&#*h^D&R?jp7+/u&#(AP##XU8c$fSYW-J95_-Dp[g9wcO&#M-h1OcJlc-*vpw0xUX&#"
"OQFKNX@QI'IoPp7nb,QU//MQ&ZDkKP)X<WSVL(68uVl&#c'[0#(s1X&xm$Y%B7*K:eDA323j998GXbA#pwMs-jgD$9QISB-A_(aN4xoFM^@C58D0+Q+q3n0#3U1InDjF682-SjMXJK)("
"h$hxua_K]ul92%'BOU&#BRRh-slg8KDlr:%L71Ka:.A;%YULjDPmL<LYs8i#XwJOYaKPKc1h:'9Ke,g)b),78=I39B;xiY$bgGw-&.Zi9InXDuYa%G*f2Bq7mn9^#p1vv%#(Wi-;/Z5h"
"o;#2:;%d&#x9v68C5g?ntX0X)pT`;%pB3q7mgGN)3%(P8nTd5L7GeA-GL@+%J3u2:(Yf>et`e;)f#Km8&+DC$I46>#Kr]]u-[=99tts1.qb#q72g1WJO81q+eN'03'eM>&1XxY-caEnO"
"j%2n8)),?ILR5^.Ibn<-X-Mq7[a82Lq:F&#ce+S9wsCK*x`569E8ew'He]h:sI[2LM$[guka3ZRd6:t%IG:;$%YiJ:Nq=?eAw;/:nnDq0(CYcMpG)qLN4$##&J<j$UpK<Q4a1]MupW^-"
"sj_$%[HK%'F####QRZJ::Y3EGl4'@%FkiAOg#p[##O`gukTfBHagL<LHw%q&OV0##F=6/:chIm0@eCP8X]:kFI%hl8hgO@RcBhS-@Qb$%+m=hPDLg*%K8ln(wcf3/'DW-$.lR?n[nCH-"
"eXOONTJlh:.RYF%3'p6sq:UIMA945&^HFS87@$EP2iG<-lCO$%c`uKGD3rC$x0BL8aFn--`ke%#HMP'vh1/R&O_J9'um,.<tx[@%wsJk&bUT2`0uMv7gg#qp/ij.L56'hl;.s5CUrxjO"
"M7-##.l+Au'A&O:-T72L]P`&=;ctp'XScX*rU.>-XTt,%OVU4)S1+R-#dg0/Nn?Ku1^0f$B*P:Rowwm-`0PKjYDDM'3]d39VZHEl4,.j']Pk-M.h^&:0FACm$maq-&sgw0t7/6(^xtk%"
"LuH88Fj-ekm>GA#_>568x6(OFRl-IZp`&b,_P'$M<Jnq79VsJW/mWS*PUiq76;]/NM_>hLbxfc$mj`,O;&%W2m`Zh:/)Uetw:aJ%]K9h:TcF]u_-Sj9,VK3M.*'&0D[Ca]J9gp8,kAW]"
"%(?A%R$f<->Zts'^kn=-^@c4%-pY6qI%J%1IGxfLU9CP8cbPlXv);C=b),<2mOvP8up,UVf3839acAWAW-W?#ao/^#%KYo8fRULNd2.>%m]UK:n%r$'sw]J;5pAoO_#2mO3n,'=H5(et"
"Hg*`+RLgv>=4U8guD$I%D:W>-r5V*%j*W:Kvej.Lp$<M-SGZ':+Q_k+uvOSLiEo(<aD/K<CCc`'Lx>'?;++O'>()jLR-^u68PHm8ZFWe+ej8h:9r6L*0//c&iH&R8pRbA#Kjm%upV1g:"
"a_#Ur7FuA#(tRh#.Y5K+@?3<-8m0$PEn;J:rh6?I6uG<-`wMU'ircp0LaE_OtlMb&1#6T.#FDKu#1Lw%u%+GM+X'e?YLfjM[VO0MbuFp7;>Q&#WIo)0@F%q7c#4XAXN-U&VB<HFF*qL("
"$/V,;(kXZejWO`<[5?\?ewY(*9=%wDc;,u<'9t3W-(H1th3+G]ucQ]kLs7df($/*JL]@*t7Bu_G3_7mp7<iaQjO@.kLg;x3B0lqp7Hf,^Ze7-##@/c58Mo(3;knp0%)A7?-W+eI'o8)b<"
"nKnw'Ho8C=Y>pqB>0ie&jhZ[?iLR@@_AvA-iQC(=ksRZRVp7`.=+NpBC%rh&3]R:8XDmE5^V8O(x<<aG/1N$#FX$0V5Y6x'aErI3I$7x%E`v<-BY,)%-?Psf*l?%C3.mM(=/M0:JxG'?"
"7WhH%o'a<-80g0NBxoO(GH<dM]n.+%q@jH?f.UsJ2Ggs&4<-e47&Kl+f//9@`b+?.TeN_&B8Ss?v;^Trk;f#YvJkl&w$]>-+k?'(<S:68tq*WoDfZu';mM?8X[ma8W%*`-=;D.(nc7/;"
")g:T1=^J$&BRV(-lTmNB6xqB[@0*o.erM*<SWF]u2=st-*(6v>^](H.aREZSi,#1:[IXaZFOm<-ui#qUq2$##Ri;u75OK#(RtaW-K-F`S+cF]uN`-KMQ%rP/Xri.LRcB##=YL3BgM/3M"
"D?@f&1'BW-)Ju<L25gl8uhVm1hL$##*8###'A3/LkKW+(^rWX?5W_8g)a(m&K8P>#bmmWCMkk&#TR`C,5d>g)F;t,4:@_l8G/5h4vUd%&%950:VXD'QdWoY-F$BtUwmfe$YqL'8(PWX("
"P?^@Po3$##`MSs?DWBZ/S>+4%>fX,VWv/w'KD`LP5IbH;rTV>n3cEK8U#bX]l-/V+^lj3;vlMb&[5YQ8#pekX9JP3XUC72L,,?+Ni&co7ApnO*5NK,((W-i:$,kp'UDAO(G0Sq7MVjJs"
"bIu)'Z,*[>br5fX^:FPAWr-m2KgL<LUN098kTF&#lvo58=/vjDo;.;)Ka*hLR#/k=rKbxuV`>Q_nN6'8uTG&#1T5g)uLv:873UpTLgH+#FgpH'_o1780Ph8KmxQJ8#H72L4@768@Tm&Q"
"h4CB/5OvmA&,Q&QbUoi$a_%3M01H)4x7I^&KQVgtFnV+;[Pc>[m4k//,]1?#`VY[Jr*3&&slRfLiVZJ:]?=K3Sw=[$=uRB?3xk48@aeg<Z'<$#4H)6,>e0jT6'N#(q%.O=?2S]u*(m<-"
"V8J'(1)G][68hW$5'q[GC&5j`TE?m'esFGNRM)j,ffZ?-qx8;->g4t*:CIP/[Qap7/9'#(1sao7w-.qNUdkJ)tCF&#B^;xGvn2r9FEPFFFcL@.iFNkTve$m%#QvQS8U@)2Z+3K:AKM5i"
"sZ88+dKQ)W6>J%CL<KE>`.d*(B`-n8D9oK<Up]c$X$(,)M8Zt7/[rdkqTgl-0cuGMv'?>-XV1q['-5k'cAZ69e;D_?$ZPP&s^+7])$*$#@QYi9,5P&#9r+$%CE=68>K8r0=dSC%%(@p7"
".m7jilQ02'0-VWAg<a/''3u.=4L$Y)6k/K:_[3=&jvL<L0C/2'v:^;-DIBW,B4E68:kZ;%?8(Q8BH=kO65BW?xSG&#@uU,DS*,?.+(o(#1vCS8#CHF>TlGW'b)Tq7VT9q^*^$$.:&N@@"
"$&)WHtPm*5_rO0&e%K&#-30j(E4#'Zb.o/(Tpm$>K'f@[PvFl,hfINTNU6u'0pao7%XUp9]5.>%h`8_=VYbxuel.NTSsJfLacFu3B'lQSu/m6-Oqem8T+oE--$0a/k]uj9EwsG>%veR*"
"hv^BFpQj:K'#SJ,sB-'#](j.Lg92rTw-*n%@/;39rrJF,l#qV%OrtBeC6/,;qB3ebNW[?,Hqj2L.1NP&GjUR=1D8QaS3Up&@*9wP?+lo7b?@%'k4`p0Z$22%K3+iCZj?XJN4Nm&+YF]u"
"@-W$U%VEQ/,,>>#)D<h#`)h0:<Q6909ua+&VU%n2:cG3FJ-%@Bj-DgLr`Hw&HAKjKjseK</xKT*)B,N9X3]krc12t'pgTV(Lv-tL[xg_%=M_q7a^x?7Ubd>#%8cY#YZ?=,`Wdxu/ae&#"
"w6)R89tI#6@s'(6Bf7a&?S=^ZI_kS&ai`&=tE72L_D,;^R)7[$s<Eh#c&)q.MXI%#v9ROa5FZO%sF7q7Nwb&#ptUJ:aqJe$Sl68%.D###EC><?-aF&#RNQv>o8lKN%5/$(vdfq7+ebA#"
"u1p]ovUKW&Y%q]'>$1@-[xfn$7ZTp7mM,G,Ko7a&Gu%G[RMxJs[0MM%wci.LFDK)(<c`Q8N)jEIF*+?P2a8g%)$q]o2aH8C&<SibC/q,(e:v;-b#6[$NtDZ84Je2KNvB#$P5?tQ3nt(0"
"d=j.LQf./Ll33+(;q3L-w=8dX$#WF&uIJ@-bfI>%:_i2B5CsR8&9Z&#=mPEnm0f`<&c)QL5uJ#%u%lJj+D-r;BoF&#4DoS97h5g)E#o:&S4weDF,9^Hoe`h*L+_a*NrLW-1pG_&2UdB8"
"6e%B/:=>)N4xeW.*wft-;$'58-ESqr<b?UI(_%@[P46>#U`'6AQ]m&6/`Z>#S?YY#Vc;r7U2&326d=w&H####?TZ`*4?&.MK?LP8Vxg>$[QXc%QJv92.(Db*B)gb*BM9dM*hJMAo*c&#"
"b0v=Pjer]$gG&JXDf->'StvU7505l9$AFvgYRI^&<^b68?j#q9QX4SM'RO#&sL1IM.rJfLUAj221]d##DW=m83u5;'bYx,*Sl0hL(W;;$doB&O/TQ:(Z^xBdLjL<Lni;''X.`$#8+1GD"
":k$YUWsbn8ogh6rxZ2Z9]%nd+>V#*8U_72Lh+2Q8Cj0i:6hp&$C/:p(HK>T8Y[gHQ4`4)'$Ab(Nof%V'8hL&#<NEdtg(n'=S1A(Q1/I&4([%dM`,Iu'1:_hL>SfD07&6D<fp8dHM7/g+"
"tlPN9J*rKaPct&?'uBCem^jn%9_K)<,C5K3s=5g&GmJb*[SYq7K;TRLGCsM-$$;S%:Y@r7AK0pprpL<Lrh,q7e/%KWK:50I^+m'vi`3?%Zp+<-d+$L-Sv:@.o19n$s0&39;kn;S%BSq*"
"$3WoJSCLweV[aZ'MQIjO<7;X-X;&+dMLvu#^UsGEC9WEc[X(wI7#2.(F0jV*eZf<-Qv3J-c+J5AlrB#$p(H68LvEA'q3n0#m,[`*8Ft)FcYgEud]CWfm68,(aLA$@EFTgLXoBq/UPlp7"
":d[/;r_ix=:TF`S5H-b<LI&HY(K=h#)]Lk$K14lVfm:x$H<3^Ql<M`$OhapBnkup'D#L$Pb_`N*g]2e;X/Dtg,bsj&K#2[-:iYr'_wgH)NUIR8a1n#S?Yej'h8^58UbZd+^FKD*T@;6A"
"7aQC[K8d-(v6GI$x:T<&'Gp5Uf>@M.*J:;$-rv29'M]8qMv-tLp,'886iaC=Hb*YJoKJ,(j%K=H`K.v9HggqBIiZu'QvBT.#=)0ukruV&.)3=(^1`o*Pj4<-<aN((^7('#Z0wK#5GX@7"
"u][`*S^43933A4rl][`*O4CgLEl]v$1Q3AeF37dbXk,.)vj#x'd`;qgbQR%FW,2(?LO=s%Sc68%NP'##Aotl8x=BE#j1UD([3$M(]UI2LX3RpKN@;/#f'f/&_mt&F)XdF<9t4)Qa.*kT"
"LwQ'(TTB9.xH'>#MJ+gLq9-##@HuZPN0]u:h7.T..G:;$/Usj(T7`Q8tT72LnYl<-qx8;-HV7Q-&Xdx%1a,hC=0u+HlsV>nuIQL-5<N?)NBS)QN*_I,?&)2'IM%L3I)X((e/dl2&8'<M"
":^#M*Q+[T.Xri.LYS3v%fF`68h;b-X[/En'CR.q7E)p'/kle2HM,u;^%OKC-N+Ll%F9CF<Nf'^#t2L,;27W:0O@6##U6W7:$rJfLWHj$#)woqBefIZ.PK<b*t7ed;p*_m;4ExK#h@&]>"
"_>@kXQtMacfD.m-VAb8;IReM3$wf0''hra*so568'Ip&vRs849'MRYSp%:t:h5qSgwpEr$B>Q,;s(C#$)`svQuF$##-D,##,g68@2[T;.XSdN9Qe)rpt._K-#5wF)sP'##p#C0c%-Gb%"
"hd+<-j'Ai*x&&HMkT]C'OSl##5RG[JXaHN;d'uA#x._U;.`PU@(Z3dt4r152@:v,'R.Sj'w#0<-;kPI)FfJ&#AYJ&#//)>-k=m=*XnK$>=)72L]0I%>.G690a:$##<,);?;72#?x9+d;"
"^V'9;jY@;)br#q^YQpx:X#Te$Z^'=-=bGhLf:D6&bNwZ9-ZD#n^9HhLMr5G;']d&6'wYmTFmL<LD)F^%[tC'8;+9E#C$g%#5Y>q9wI>P(9mI[>kC-ekLC/R&CH+s'B;K-M6$EB%is00:"
"+A4[7xks.LrNk0&E)wILYF@2L'0Nb$+pv<(2.768/FrY&h$^3i&@+G%JT'<-,v`3;_)I9M^AE]CN?Cl2AZg+%4iTpT3<n-&%H%b<FDj2M<hH=&Eh<2Len$b*aTX=-8QxN)k11IM1c^j%"
"9s<L<NFSo)B?+<-(GxsF,^-Eh@$4dXhN$+#rxK8'je'D7k`e;)2pYwPA'_p9&@^18ml1^[@g4t*[JOa*[=Qp7(qJ_oOL^('7fB&Hq-:sf,sNj8xq^>$U4O]GKx'm9)b@p7YsvK3w^YR-"
"CdQ*:Ir<($u&)#(&?L9Rg3H)4fiEp^iI9O8KnTj,]H?D*r7'M;PwZ9K0E^k&-cpI;.p/6_vwoFMV<->#%Xi.LxVnrU(4&8/P+:hLSKj$#U%]49t'I:rgMi'FL@a:0Y-uA[39',(vbma*"
"hU%<-SRF`Tt:542R_VV$p@[p8DV[A,?1839FWdF<TddF<9Ah-6&9tWoDlh]&1SpGMq>Ti1O*H&#(AL8[_P%.M>v^-))qOT*F5Cq0`Ye%+$B6i:7@0IX<N+T+0MlMBPQ*Vj>SsD<U4JHY"
"8kD2)2fU/M#$e.)T4,_=8hLim[&);?UkK'-x?'(:siIfL<$pFM`i<?%W(mGDHM%>iWP,##P`%/L<eXi:@Z9C.7o=@(pXdAO/NLQ8lPl+HPOQa8wD8=^GlPa8TKI1CjhsCTSLJM'/Wl>-"
"S(qw%sf/@%#B6;/U7K]uZbi^Oc^2n<bhPmUkMw>%t<)'mEVE''n`WnJra$^TKvX5B>;_aSEK',(hwa0:i4G?.Bci.(X[?b*($,=-n<.Q%`(X=?+@Am*Js0&=3bh8K]mL<LoNs'6,'85`"
"0?t/'_U59@]ddF<#LdF<eWdF<OuN/45rY<-L@&#+fm>69=Lb,OcZV/);TTm8VI;?%OtJ<(b4mq7M6:u?KRdF<gR@2L=FNU-<b[(9c/ML3m;Z[$oF3g)GAWqpARc=<ROu7cL5l;-[A]%/"
"+fsd;l#SafT/f*W]0=O'$(Tb<[)*@e775R-:Yob%g*>l*:xP?Yb.5)%w_I?7uk5JC+FS(m#i'k.'a0i)9<7b'fs'59hq$*5Uhv##pi^8+hIEBF`nvo`;'l0.^S1<-wUK2/Coh58KKhLj"
"M=SO*rfO`+qC`W-On.=AJ56>>i2@2LH6A:&5q`?9I3@@'04&p2/LVa*T-4<-i3;M9UvZd+N7>b*eIwg:CC)c<>nO&#<IGe;__.thjZl<%w(Wk2xmp4Q@I#I9,DF]u7-P=.-_:YJ]aS@V"
"?6*C()dOp7:WL,b&3Rg/.cmM9&r^>$(>.Z-I&J(Q0Hd5Q%7Co-b`-c<N(6r@ip+AurK<m86QIth*#v;-OBqi+L7wDE-Ir8K['m+DDSLwK&/.?-V%U_%3:qKNu$_b*B-kp7NaD'QdWQPK"
"Yq[@>P)hI;*_F]u`Rb[.j8_Q/<&>uu+VsH$sM9TA%?)(vmJ80),P7E>)tjD%2L=-t#fK[%`v=Q8<FfNkgg^oIbah*#8/Qt$F&:K*-(N/'+1vMB,u()-a.VUU*#[e%gAAO(S>WlA2);Sa"
">gXm8YB`1d@K#n]76-a$U,mF<fX]idqd)<3,]J7JmW4`6]uks=4-72L(jEk+:bJ0M^q-8Dm_Z?0olP1C9Sa&H[d&c$ooQUj]Exd*3ZM@-WGW2%s',B-_M%>%Ul:#/'xoFM9QX-$.QN'>"
"[%$Z$uF6pA6Ki2O5:8w*vP1<-1`[G,)-m#>0`P&#eb#.3i)rtB61(o'$?X3B</R90;eZ]%Ncq;-Tl]#F>2Qft^ae_5tKL9MUe9b*sLEQ95C&`=G?@Mj=wh*'3E>=-<)Gt*Iw)'QG:`@I"
"wOf7&]1i'S01B+Ev/Nac#9S;=;YQpg_6U`*kVY39xK,[/6Aj7:'1Bm-_1EYfa1+o&o4hp7KN_Q(OlIo@S%;jVdn0'1<Vc52=u`3^o-n1'g4v58Hj&6_t7$##?M)c<$bgQ_'SY((-xkA#"
"Y(,p'H9rIVY-b,'%bCPF7.J<Up^,(dU1VY*5#WkTU>h19w,WQhLI)3S#f$2(eb,jr*b;3Vw]*7NH%$c4Vs,eD9>XW8?N]o+(*pgC%/72LV-u<Hp,3@e^9UB1J+ak9-TN/mhKPg+AJYd$"
"MlvAF_jCK*.O-^(63adMT->W%iewS8W6m2rtCpo'RS1R84=@paTKt)>=%&1[)*vp'u+x,VrwN;&]kuO9JDbg=pO$J*.jVe;u'm0dr9l,<*wMK*Oe=g8lV_KEBFkO'oU]^=[-792#ok,)"
"i]lR8qQ2oA8wcRCZ^7w/Njh;?.stX?Q1>S1q4Bn$)K1<-rGdO'$Wr.Lc.CG)$/*JL4tNR/,SVO3,aUw'DJN:)Ss;wGn9A32ijw%FL+Z0Fn.U9;reSq)bmI32U==5ALuG&#Vf1398/pVo"
"1*c-(aY168o<`JsSbk-,1N;$>0:OUas(3:8Z972LSfF8eb=c-;>SPw7.6hn3m`9^Xkn(r.qS[0;T%&Qc=+STRxX'q1BNk3&*eu2;&8q$&x>Q#Q7^Tf+6<(d%ZVmj2bDi%.3L2n+4W'$P"
"iDDG)g,r%+?,$@?uou5tSe2aN_AQU*<h`e-GI7)?OK2A.d7_c)?wQ5AS@DL3r#7fSkgl6-++D:'A,uq7SvlB$pcpH'q3n0#_%dY#xCpr-l<F0NR@-##FEV6NTF6##$l84N1w?AO>'IAO"
"URQ##V^Fv-XFbGM7Fl(N<3DhLGF%q.1rC$#:T__&Pi68%0xi_&[qFJ(77j_&JWoF.V735&T,[R*:xFR*K5>>#`bW-?4Ne_&6Ne_&6Ne_&n`kr-#GJcM6X;uM6X;uM(.a..^2TkL%oR(#"
";u.T%fAr%4tJ8&><1=GHZ_+m9/#H1F^R#SC#*N=BA9(D?v[UiFY>>^8p,KKF.W]L29uLkLlu/+4T<XoIB&hx=T1PcDaB&;HH+-AFr?(m9HZV)FKS8JCw;SD=6[^/DZUL`EUDf]GGlG&>"
"w$)F./^n3+rlo+DB;5sIYGNk+i1t-69Jg--0pao7Sm#K)pdHW&;LuDNH@H>#/X-TI(;P>#,Gc>#0Su>#4`1?#8lC?#<xU?#@.i?#D:%@#HF7@#LRI@#P_[@#Tkn@#Xw*A#]-=A#a9OA#"
"d<F&#*;G##.GY##2Sl##6`($#:l:$#>xL$#B.`$#F:r$#JF.%#NR@%#R_R%#Vke%#Zww%#_-4&#3^Rh%Sflr-k'MS.o?.5/sWel/wpEM0%3'/1)K^f1-d>G21&v(35>V`39V7A4=onx4"
"A1OY5EI0;6Ibgr6M$HS7Q<)58C5w,;WoA*#[%T*#`1g*#d=#+#hI5+#lUG+#pbY+#tnl+#x$),#&1;,#*=M,#.I`,#2Ur,#6b.-#;w[H#iQtA#m^0B#qjBB#uvTB##-hB#'9$C#+E6C#"
"/QHC#3^ZC#7jmC#;v)D#?,<D#C8ND#GDaD#KPsD#O]/E#g1A5#KA*1#gC17#MGd;#8(02#L-d3#rWM4#Hga1#,<w0#T.j<#O#'2#CYN1#qa^:#_4m3#o@/=#eG8=#t8J5#`+78#4uI-#"
"m3B2#SB[8#Q0@8#i[*9#iOn8#1Nm;#^sN9#qh<9#:=x-#P;K2#$%X9#bC+.#Rg;<#mN=.#MTF.#RZO.#2?)4#Y#(/#[)1/#b;L/#dAU/#0Sv;#lY$0#n`-0#sf60#(F24#wrH0#%/e0#"
"TmD<#%JSMFove:CTBEXI:<eh2g)B,3h2^G3i;#d3jD>)4kMYD4lVu`4m`:&5niUA5@(A5BA1]PBB:xlBCC=2CDLXMCEUtiCf&0g2'tN?PGT4CPGT4CPGT4CPGT4CPGT4CPGT4CPGT4CP"
"GT4CPGT4CPGT4CPGT4CPGT4CPGT4CP-qekC`.9kEg^+F$kwViFJTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5KTB&5o,^<-28ZI'O?;xp"
"O?;xpO?;xpO?;xpO?;xpO?;xpO?;xpO?;xpO?;xpO?;xpO?;xpO?;xpO?;xpO?;xp;7q-#lLYI:xvD=#";

#endif /* NK_INCLUDE_DEFAULT_FONT */

NK_API void nk_cnfg_set_font(struct nk_context* ctx, struct nk_cnfg_font* font)
{
	nk_style_set_font(ctx, &font->nk);
}

#ifdef NK_INCLUDE_DEFAULT_FONT
NK_API struct nk_cnfg_font* nk_cnfg_font_load_default(float pixel_height)
{
	return nk_cnfg_font_load_from_compressed_base85(nk_proggy_clean_ttf_compressed_data_base85, pixel_height);
}
#endif

NK_API void nk_cnfg_font_destroy(struct nk_cnfg_font* f)
{
	free(f->ttf_buffer);
	free(f->atlas_bitmap);
	free(f);
}

NK_INTERN void nk_cnfg_render_character(struct nk_cnfg_font* f, float posX, float posY, char character, uint32_t color)
{
	if (character < f->first_codepoint || character >= (f->first_codepoint + f->num_characters))
		return;

	stbtt_aligned_quad quad;
	stbtt_GetPackedQuad(f->packed_chars, f->atlas_size.x, f->atlas_size.y, character - f->first_codepoint, &posX, &posY, &quad, 1);

	for (int y_pixel = (int)quad.y0; y_pixel < (int)quad.y1; y_pixel++)
	{
		for (int x_pixel = (int)quad.x0; x_pixel < (int)quad.x1; x_pixel++)
		{
			int atlas_x = (int)(quad.s0 * f->atlas_size.x) + (x_pixel - (int)quad.x0);
			int atlas_y = (int)(quad.t0 * f->atlas_size.y) + (y_pixel - (int)quad.y0);

			uint32_t atlas_packed_color = f->atlas_bitmap[atlas_y * f->atlas_size.x + atlas_x];
			struct nk_color atlas_color;
			NK_CNFG_COLOR_SPLIT(atlas_color, atlas_packed_color);

			if (atlas_color.a > 0)
			{
				uint32_t blended_color = CNFGBlendAlpha(color, atlas_color.a);
				CNFGColor(blended_color);
				CNFGTackPixel(x_pixel, y_pixel);
			}
		}
	}
}

NK_API void nk_cnfg_render_string(struct nk_cnfg_font* f, int posX, int posY, const char* text, uint32_t color)
{
	float scaledAscent = (float)(f->ascent - f->line_gap) * f->scale;
	float xpos = (float)posX;
	float ypos = (float)posY + scaledAscent;
	int text_len = 0;
	nk_rune unicode = 0;
	nk_rune next = 0;
	int glyph_len = 0;
	int next_glyph_len = 0;

	if (!text || !*text)
		return;

	glyph_len = nk_utf_decode(text, &unicode, strlen(text));
	if (!glyph_len)
		return;

	while (text_len < strlen(text) && glyph_len)
	{
		if (unicode < f->first_codepoint || unicode >= (f->first_codepoint + f->num_characters))
		{
			break;
		}

		stbtt_packedchar b = f->packed_chars[unicode - f->first_codepoint];

		nk_cnfg_render_character(f, xpos, ypos, unicode, color);

		xpos += b.xadvance;

		next_glyph_len = nk_utf_decode(text + text_len + glyph_len, &next, strlen(text) - text_len);
		text_len += glyph_len;
		glyph_len = next_glyph_len;
		unicode = next;
	}
}

short scissor_x = 0, scissor_y = 0;
short scissor_w = 0, scissor_h = 0;
int scissor_clipping_enabled = 0;

NK_INTERN void nk_cnfg_scissor_cmd(const struct nk_command_scissor* cmd, struct nk_context* ctx)
{
	scissor_x = (short)cmd->x;
	scissor_y = (short)cmd->y;
	scissor_w = (short)cmd->w;
	scissor_h = (short)cmd->h;
	scissor_clipping_enabled = 1;
}

NK_INTERN int nk_cnfg_point_in_scissor(short x, short y)
{
	if (!scissor_clipping_enabled)
		return 1;
	return (x >= scissor_x && x <= (scissor_x + scissor_w) &&
		y >= scissor_y && y <= (scissor_y + scissor_h));
}

NK_INTERN int nk_cnfg_rect_in_scissor(short x, short y, short w, short h)
{
	if (!scissor_clipping_enabled)
		return 1;

	// Check if the rectangle overlaps with the scissor region
	short x2 = x + w;
	short y2 = y + h;
	return !(x2 < scissor_x || x > (scissor_x + scissor_w) ||
		y2 < scissor_y || y > (scissor_y + scissor_h));
}

NK_INTERN void nk_cnfg_line_cmd(const struct nk_command_line* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_point_in_scissor(cmd->begin.x, cmd->begin.y) || !nk_cnfg_point_in_scissor(cmd->end.x, cmd->end.y))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackThickSegment(cmd->begin.x, cmd->begin.y, cmd->end.x, cmd->end.y, cmd->line_thickness);
}

NK_INTERN void nk_cnfg_curve_cmd(const struct nk_command_curve* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_point_in_scissor(cmd->begin.x, cmd->begin.y) || !nk_cnfg_point_in_scissor(cmd->end.x, cmd->end.y))
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
	if (!nk_cnfg_rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackThickSegment(cmd->x, cmd->y, cmd->x + cmd->w, cmd->y, cmd->line_thickness);
	CNFGTackThickSegment(cmd->x, cmd->y + cmd->h, cmd->x + cmd->w, cmd->y + cmd->h, cmd->line_thickness);
	CNFGTackThickSegment(cmd->x, cmd->y, cmd->x, cmd->y + cmd->h, cmd->line_thickness);
	CNFGTackThickSegment(cmd->x + cmd->w, cmd->y, cmd->x + cmd->w, cmd->y + cmd->h, cmd->line_thickness);
}

NK_INTERN void nk_cnfg_rect_filled_cmd(const struct nk_command_rect_filled* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackRectangle(cmd->x, cmd->y, cmd->x + cmd->w, cmd->y + cmd->h);
}

NK_INTERN void nk_cnfg_rect_multi_color_cmd(const struct nk_command_rect_multi_color* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
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
	CNFGTackCircle(cmd->x + cmd->w / 2, cmd->y + cmd->h / 2, cmd->w / 2, 64);
}

NK_INTERN void nk_cnfg_circle_filled_cmd(const struct nk_command_circle_filled* cmd, struct nk_context* ctx)
{
	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackFilledCircle(cmd->x + cmd->w / 2, cmd->y + cmd->h / 2, cmd->w / 2, 64);
}

NK_INTERN void nk_cnfg_arc_cmd(const struct nk_command_arc* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_point_in_scissor(cmd->cx, cmd->cy))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackArc(cmd->cx, cmd->cy, cmd->r, cmd->a[0], cmd->a[1], 64);
}

NK_INTERN void nk_cnfg_arc_filled_cmd(const struct nk_command_arc_filled* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_point_in_scissor(cmd->cx, cmd->cy))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackFilledArc(cmd->cx, cmd->cy, cmd->r, cmd->a[0], cmd->a[1], 64); 
}

NK_INTERN void nk_cnfg_triangle_cmd(const struct nk_command_triangle* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_point_in_scissor(cmd->a.x, cmd->a.y) ||  !nk_cnfg_point_in_scissor(cmd->b.x, cmd->b.y) || !nk_cnfg_point_in_scissor(cmd->c.x, cmd->c.y))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);
	CNFGTackSegment(cmd->a.x, cmd->a.y, cmd->b.x, cmd->b.y);
	CNFGTackSegment(cmd->b.x, cmd->b.y, cmd->c.x, cmd->c.y);
	CNFGTackSegment(cmd->c.x, cmd->c.y, cmd->a.x, cmd->a.y);
}

NK_INTERN void nk_cnfg_triangle_filled_cmd(const struct nk_command_triangle_filled* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_point_in_scissor(cmd->a.x, cmd->a.y) ||  !nk_cnfg_point_in_scissor(cmd->b.x, cmd->b.y) ||  !nk_cnfg_point_in_scissor(cmd->c.x, cmd->c.y))
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
		if (!nk_cnfg_point_in_scissor(cmd->points[i].x, cmd->points[i].y))
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
		if (!nk_cnfg_point_in_scissor(cmd->points[i].x, cmd->points[i].y))
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
		if (!nk_cnfg_point_in_scissor(cmd->points[i].x, cmd->points[i].y))
			return;
	}

	uint32_t color = NK_CNFG_COLOR(cmd->color);
	CNFGColor(color);

	for (int i = 0; i != cmd->point_count - 1; i++)
	{
		CNFGTackThickSegment(cmd->points[i].x, cmd->points[i].y, cmd->points[i + 1].x, cmd->points[i + 1].y, cmd->line_thickness);
	}
}

NK_INTERN void nk_cnfg_text_cmd(const struct nk_command_text* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->foreground);
	struct nk_cnfg_font* f = (struct nk_cnfg_font*)ctx->style.font->userdata.ptr;
	nk_cnfg_render_string(f, cmd->x, cmd->y, cmd->string, color);
}

NK_INTERN void nk_cnfg_image_cmd(const struct nk_command_image* cmd, struct nk_context* ctx)
{
	if (!nk_cnfg_rect_in_scissor(cmd->x, cmd->y, cmd->w, cmd->h))
		return;

	uint32_t color = NK_CNFG_COLOR(cmd->col);
	CNFGBlitImage(cmd->img.handle.ptr, cmd->x, cmd->y, cmd->w, cmd->h);
}

struct nk_colorf* nk_global_bg_color;
NK_API void nk_cnfg_set_bg_color_ref(struct nk_colorf* ref)
{
	nk_global_bg_color = ref;
}

NK_API void nk_cnfg_render(struct nk_context* ctx)
{
	const struct nk_command* cmd;

	struct nk_color color = nk_rgb_cf(*nk_global_bg_color);
	CNFGBGColor = NK_CNFG_COLOR(color);
	CNFGClearFrame();

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

	nk_clear(ctx);
	CNFGSwapBuffers();
}

NK_API void nk_cnfg_input_key(struct nk_context* ctx, int keycode, int bDown)
{
	switch (keycode)
	{
		case CNFG_KEY_LEFT_ARROW:
		{
			nk_input_key(ctx, NK_KEY_LEFT, bDown);
		} break;
		case CNFG_KEY_RIGHT_ARROW:
		{
			nk_input_key(ctx, NK_KEY_RIGHT, bDown);
		} break;
		case CNFG_KEY_TOP_ARROW:
		{
			nk_input_key(ctx, NK_KEY_UP, bDown);
		} break;
		case CNFG_KEY_BOTTOM_ARROW:
		{
			nk_input_key(ctx, NK_KEY_DOWN, bDown);
		} break;
		case CNFG_KEY_ENTER:
		{
			nk_input_key(ctx, NK_KEY_ENTER, bDown);
		} break;
		case CNFG_KEY_BACKSPACE:
		{
			nk_input_key(ctx, NK_KEY_BACKSPACE, bDown);
		} break;
		default:
		{

		} break;
	}
}

NK_API void nk_cnfg_input_button(struct nk_context* ctx, int x, int y, int button, int bDown)
{
	switch (button)
	{
		case 1:
		{
			if (!bDown)
			{
				nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, 0);
			}
			nk_input_button(ctx, NK_BUTTON_LEFT, x, y, bDown);
		} break;
		case 2:
		{
			nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, bDown);
		} break;
		case 3:
		{
			nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, bDown);
		} break;
		default:
		{
		} break;
	}
}

NK_API void nk_cnfg_input_motion(struct nk_context* ctx, int x, int y)
{
	nk_input_motion(ctx, x, y);
}

NK_API void nk_cnfg_input_scroll(struct nk_context* ctx, float scroll_x, float scroll_y)
{
	struct nk_vec2 scroll = nk_vec2(scroll_x, scroll_y);
	nk_input_scroll(ctx, scroll);
}

NK_API void nk_cnfg_input_char(struct nk_context* ctx, char c)
{
	nk_input_char(ctx, c);
}

NK_API int nk_cnfg_input_destroy(struct nk_context* ctx)
{
	return 0;
}

#ifdef NK_INCLUDE_DEFAULT_FONT
struct nk_cnfg_font* default_font;
#endif
NK_API void nk_cnfg_init(const char* title, int width, int height, struct nk_context* ctx)
{
	nk_init_default(ctx, NULL);

	nk_style_default(ctx);

#ifdef NK_INCLUDE_DEFAULT_FONT
	default_font = nk_cnfg_font_load_default(13.0f);
	nk_cnfg_set_font(ctx, default_font);
#endif

	CNFGSetup(title, width, height);
}

#endif