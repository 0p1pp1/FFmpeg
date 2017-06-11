/*
 * ISDB subtitle decoder
 * Copyright (c) 2017  0p1pp1
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <iconv.h>
#include <stdint.h>
#include <string.h>

#include "avcodec.h"

#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/crc.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#define ISDBSUB_DATA_ID 0x80
#define ISDBSUB_DU_TYPE_TXT 0x20
#define ISDBSUB_UNIT_SEP 0x1F
#define ISDBSUB_MGMNT_TIMEOUT (180 * 1000)
#define ISDBSUB_NO_DGID -1
#define ISDBSUB_MAX_LANG 2 /* ARIB TR-B14/B15 */

#define IS_HORIZONTAL_LAYOUT(format) \
    ((format) == ISDBSUB_FMT_960H || (format) == ISDBSUB_FMT_720H)
#define LAYOUT_GET_WIDTH(format) \
    (((format) == ISDBSUB_FMT_960H || (format) == ISDBSUB_FMT_960V) ? 960 : 720)
#define LAYOUT_GET_HEIGHT(format) \
    (((format) == ISDBSUB_FMT_960H || (format) == ISDBSUB_FMT_960V) ? 540 : 480)

#define MPEGTS_MAX_PTS (((2LL<<33) + 45)/90)

/* FIXME: should be RGBA, not ABGR ? */
#define RGBA(r,g,b,a) (((unsigned)(255 - (a)) << 24) | ((b) << 16) | ((g) << 8) | (r))

static const AVCRC *Crc_table;

typedef uint32_t rgba;
static rgba Default_clut[128] =
{
    //0-7
    RGBA(0,0,0,255), RGBA(255,0,0,255), RGBA(0,255,0,255), RGBA(255,255,0,255),
    RGBA(0,0,255,255), RGBA(255,0,255,255), RGBA(0,255,255,255), RGBA(255,255,255,255),
    //8-15
    RGBA(0,0,0,0), RGBA(170,0,0,255), RGBA(0,170,0,255), RGBA(170,170,0,255),
    RGBA(0,0,170,255), RGBA(170,0,170,255), RGBA(0,170,170,255), RGBA(170,170,170,255),
    //16-23
    RGBA(0,0,85,255), RGBA(0,85,0,255), RGBA(0,85,85,255), RGBA(0,85,170,255),
    RGBA(0,85,255,255), RGBA(0,170,85,255), RGBA(0,170,255,255), RGBA(0,255,85,255),
    //24-31
    RGBA(0,255,170,255), RGBA(85,0,0,255), RGBA(85,0,85,255), RGBA(85,0,170,255),
    RGBA(85,0,255,255), RGBA(85,85,0,255), RGBA(85,85,85,255), RGBA(85,85,170,255),
    //32-39
    RGBA(85,85,255,255), RGBA(85,170,0,255), RGBA(85,170,85,255), RGBA(85,170,170,255),
    RGBA(85,170,255,255), RGBA(85,255,0,255), RGBA(85,255,85,255), RGBA(85,255,170,255),
    //40-47
    RGBA(85,255,255,255), RGBA(170,0,85,255), RGBA(170,0,255,255), RGBA(170,85,0,255),
    RGBA(170,85,85,255), RGBA(170,85,170,255), RGBA(170,85,255,255), RGBA(170,170,85,255),
    //48-55
    RGBA(170,170,255,255), RGBA(170,255,0,255), RGBA(170,255,85,255), RGBA(170,255,170,255),
    RGBA(170,255,255,255), RGBA(255,0,85,255), RGBA(255,0,170,255), RGBA(255,85,0,255),
    //56-63
    RGBA(255,85,85,255), RGBA(255,85,170,255), RGBA(255,85,255,255), RGBA(255,170,0,255),
    RGBA(255,170,85,255), RGBA(255,170,170,255), RGBA(255,170,255,255), RGBA(255,255,85,255),
    //64
    RGBA(255,255,170,255),
    // 65-127 are caliculated later.
};

static const uint8_t * const Default_macro[16] =
{
    "\x1B\x24\x42\x1B\x29\x4A\x1B\x2A\x30\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x24\x42\x1B\x29\x31\x1B\x2A\x30\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x24\x42\x1B\x29\x20\x41\x1B\x2A\x30\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x32\x1B\x29\x34\x1B\x2A\x35\x1B\x2B\x20\x70\x0F\x1B\x7D",

    "\x1B\x28\x32\x1B\x29\x33\x1B\x2A\x35\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x32\x1B\x29\x20\x41\x1B\x2A\x35\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x20\x41\x1B\x29\x20\x42\x1B\x2A\x20\x43\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x20\x44\x1B\x29\x20\x45\x1B\x2A\x20\x46\x1B\x2B\x20\x70\x0F\x1B\x7D",

    "\x1B\x28\x20\x47\x1B\x29\x20\x48\x1B\x2A\x20\x49\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x20\x4A\x1B\x29\x20\x4B\x1B\x2A\x20\x4C\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x20\x4D\x1B\x29\x20\x4E\x1B\x2A\x20\x4F\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x24\x42\x1B\x29\x20\x42\x1B\x2A\x30\x1B\x2B\x20\x70\x0F\x1B\x7D",

    "\x1B\x24\x42\x1B\x29\x20\x43\x1B\x2A\x30\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x24\x42\x1B\x29\x20\x44\x1B\x2A\x30\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x31\x1B\x29\x30\x1B\x2A\x4A\x1B\x2B\x20\x70\x0F\x1B\x7D",
    "\x1B\x28\x4A\x1B\x29\x32\x1B\x2A\x20\x41\x1B\x2B\x20\x70\x0F\x1B\x7D"
};


struct b24str_state
{
  int gl;                       /* index of the group invoked to GL */
  int gr;                       /* index of the group invoked to GR */
  int ss;                       /* flag if in SS2 or SS3.  2:SS2, 3:SS3 */

  struct group
  {
    unsigned char mb;          /* how many bytes one character consists of. */
    // code for character sets
#define CODE_ASCII ('\x40')
#define CODE_ASCII2 ('\x4A')
#define CODE_JISX0208 ('\x42')
#define CODE_JISX0213_1 ('\x51')
#define CODE_JISX0213_2 ('\x50')
#define CODE_JISX0201_KATA ('\x49')
#define CODE_MOSAIC_C ('\x34')
#define CODE_MOSAIC_D ('\x35')
#define CODE_EXT ('\x3B')
#define CODE_X_HIRA ('\x30')
#define CODE_X_HIRA_P ('\x37')
#define CODE_X_KATA ('\x31')
#define CODE_X_KATA_P ('\x38')
#define CODE_X_DRCS_MB ('\x40')
#define CODE_X_DRCS_MIN ('\x41')
#define CODE_X_DRCS_MAX ('\x4F')
#define CODE_X_MACRO ('\x70')
    unsigned char code;        /* character set that this group designates */
  } g[4];
};

struct isdbsub_layout
{
    enum isdbsub_format {
        ISDBSUB_FMT_960H = 0x08,
        ISDBSUB_FMT_960V,
        ISDBSUB_FMT_720H,
        ISDBSUB_FMT_720V,
    } format;
    int is_profile_c;  // profile C: "1seg". see ARIB TR-B14 3-4

    // clipping area.
    struct disp_area {
        int x, y;
        int w, h;
    } display_area;

    // for tracking pen position
    int font_size; // valid values: {16, 20, 24, 30, 36} (TR-B14/B15)
    struct fscale { // in [percent]
        int fscx, fscy;
    } font_scale; // 1/2x1/2, 1/2*1, 1*1, 1*2, 2*1, 2*2
    struct spacing {
        int col, row;
    } cell_spacing;

    // internal use for tracking pen position/line break.
    // Although texts are laid out by libass,
    // we need to track pen position by ourselves
    // in order to calculate line breaking positions and charcter/line spacing.
    int prev_char_sep;
    int prev_line_desc;
    int prev_line_bottom; // offset from display_area.y, not from top-margin.
    int line_desc;
    int linesep_upper;
    int line_height;
    int line_width; // pen position x
    int prev_break_idx; // ctx->text.buf[prev_break_idx] holds the previous "\N"
    int shift_baseline; // special case where baseline should be shifted down ?

    int block_offset_h;  // text[0].hspacing / 2
    int block_offset_v;  // line[0].lspacing_upper

    int repeat_count; // -1: none, 0: until EOL, 1...i: repeat the next char i times
    int in_combining; // bool
    struct scroll_param {
        enum {SCROLL_DIR_NONE, SCROLL_DIR_COLUMN, SCROLL_DIR_ROW} direction;
        int rollout;  // bool
        int speed;  // in pixel/sec
    } scroll;
};


typedef struct isdbsub_state {
    int auto_display; // bool. forced to be displayed w/o user interaction
    int rollup_mode;  // bool

    uint8_t need_init; // bool
    uint8_t clut_high_idx; // color = default_clut[high_idx << 8 | low_idx]

    uint32_t fg_color;
    uint32_t bg_color;
    uint32_t mat_color;

    struct isdbsub_layout layout_state;
    struct b24str_state text_state;
} ISDBSubState;


typedef struct ISDBSubContext {
    AVClass *class;
    int last_mngmnt_id; // "data group id" of the last subtitle management data
    int64_t last_mngmnt_pts; // time when the last mgmnt data was received
    int64_t pts;
    int64_t duration;

    enum {
        ISDBSUB_TMD_FREE,
        ISDBSUB_TMD_REALTIME,
        ISDBSUB_TMD_OFFSET
    } timing_mode;

    struct timecode {
        int hour, min, sec, ms;
    } offset, start; // currently unused.

    ISDBSubState default_states[ISDBSUB_MAX_LANG];

    int lang_tag; // language tag of the currently decoding subtitle text data
    ISDBSubState current_state; //modified default_state[lang_tag]


    iconv_t iconv;
    struct my_str {
        char *buf;
        size_t len;
        size_t used;
        size_t txt_tail; // tail of the text, excluding trailing control sequences.
    } text;

    // for output
    int is_style_inited;
    char *script_info;
    char *events;
} ISDBSubContext;


static void memdump(ISDBSubContext *ctx, const char *buf, int buf_size)
{
    int i;

    for (i=0; i < buf_size; i++) {
        av_log(ctx, AV_LOG_DEBUG, "%02hhx ", buf[i]);
        if (i % 16 == 15)
            av_log(ctx, AV_LOG_DEBUG, "\n");
    }
    if (i % 16)
        av_log(ctx, AV_LOG_DEBUG, "\n");
}


static char *pts_to_str(int64_t t, char buf[16])
{
    int ms10, sec, min, hour;

    if (t == AV_NOPTS_VALUE) {
        av_strlcpy(buf, "NOPTS", 16);
        return buf;
    }

    ms10 = (t % 1000) / 10;
    t /= 1000;
    sec = t % 60;
    t /= 60;
    min = t % 60;
    t /= 60;
    hour = t;
    buf[0] = 0;
    av_strlcatf(buf, 16, "%02d:%02d:%02d.%02d", hour, min, sec, ms10);

    return buf;
}

struct margins {
    int l, r, v;
};

static void get_margins(ISDBSubContext *ctx, struct margins *m)
{
    struct isdbsub_layout *lstate = &ctx->current_state.layout_state;

    if (IS_HORIZONTAL_LAYOUT(lstate->format)) {
        m->l = lstate->display_area.x + lstate->block_offset_h;
        m->r = LAYOUT_GET_WIDTH(lstate->format)
                - (lstate->display_area.x + lstate->display_area.w);
        m->v = lstate->display_area.y + lstate->block_offset_v;
        if (lstate->is_profile_c)
            m->v = 0;
    } else {
        m->l = lstate->display_area.y + lstate->block_offset_v;
        m->r = LAYOUT_GET_HEIGHT(lstate->format)
                - (lstate->display_area.y + lstate->display_area.h);
        m->v = LAYOUT_GET_WIDTH(lstate->format)
                - (lstate->display_area.x + lstate->display_area.w)
                + lstate->block_offset_h;
    }
}

static void reserve_buf(ISDBSubContext *ctx, size_t len)
{
    size_t blen;

    if (ctx->text.len >= ctx->text.used + len)
        return;

    blen = ((ctx->text.used + len + 127) >> 7) << 7;
    ctx->text.buf = av_realloc(ctx->text.buf, blen);
    if (!ctx->text.buf) {
        av_log(ctx, AV_LOG_WARNING, "out of memory for ctx->text.\n");
        return;
    }
    ctx->text.len = blen;
    av_log(ctx, AV_LOG_DEBUG, "expanded ctx->text(%lu)\n", blen);
}

static int append_str(ISDBSubContext *ctx, const char *str)
{
    size_t tlen = strlen(str);

    reserve_buf(ctx, tlen + 1); // +1 for terminating '\0'
    memcpy(ctx->text.buf + ctx->text.used, str, tlen);
    ctx->text.used += tlen;
    ctx->text.buf[ctx->text.used] = '\0';
    return 0;
}

static int prepend_str(ISDBSubContext *ctx, const char *str)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    size_t tlen = strlen(str);

    reserve_buf(ctx, tlen + 1); // +1 for terminating '\0'
    memmove(ctx->text.buf + tlen, ctx->text.buf, ctx->text.used);
    memcpy(ctx->text.buf, str, tlen);
    if (ls->prev_break_idx)
        ls->prev_break_idx += tlen;
    ctx->text.used += tlen;
    ctx->text.buf[ctx->text.used] = '\0';
    return 0;
}

static void insert_str(ISDBSubContext *ctx, const char *txt, int begin)
{
    int end = ctx->text.used;
    size_t len = strlen(txt);

    if (len == 0 || len > 128)
        return;

    reserve_buf(ctx, len + 1); // +1 for terminating '\0'
    memmove(ctx->text.buf + begin + len, ctx->text.buf + begin, end - begin);
    memcpy(ctx->text.buf + begin, txt, len);
    ctx->text.txt_tail += len;
    ctx->text.used += len;
    ctx->text.buf[ctx->text.used] = '\0';
}

static void advance(ISDBSubContext *ctx)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    int cscale;
    int h;
    int asc, desc;
    int csp;

    if (IS_HORIZONTAL_LAYOUT(ls->format)) {
        cscale = ls->font_scale.fscx;
        h = ls->font_size * ls->font_scale.fscy / 100;
        if (ls->font_scale.fscy == 200) {
            desc = ls->cell_spacing.row / 2;
            asc = ls->cell_spacing.row * 2 - desc + h;
        } else {
            desc = ls->cell_spacing.row * ls->font_scale.fscy / 200;
            asc = ls->cell_spacing.row * ls->font_scale.fscy / 100 - desc + h;
        }
        if (asc > ls->line_height + ls->linesep_upper) {
            if (h > ls->line_height)
                ls->line_height = h;
            ls->linesep_upper = asc - ls->line_height;
        } else if (h > ls->line_height) {
            ls->linesep_upper = ls->line_height + ls->linesep_upper - h;
            ls->line_height = h;
        }

        if (ls->prev_line_bottom == 0 && ls->linesep_upper > ls->block_offset_v)
            ls->block_offset_v = ls->linesep_upper;
        if (ls->font_scale.fscy != 50)
            ls->shift_baseline = 0;
    } else {
        int lsp;
        cscale = ls->font_scale.fscy;
        h = ls->font_size * ls->font_scale.fscx / 100;
        lsp = ls->cell_spacing.row * ls->font_scale.fscx / 100;
        desc = h / 2 + lsp / 2;
        asc = h - h / 2 + lsp - lsp / 2;
        if (asc > ls->line_height + ls->linesep_upper) {
            if (h - h / 2 > ls->line_height)
                ls->line_height = h - h / 2;
            ls->linesep_upper = asc - ls->line_height;
        } else if (h - h / 2 > ls->line_height) {
            ls->linesep_upper = ls->line_height + ls->linesep_upper - h + h / 2;
            ls->line_height = h - h / 2;
        }

        if (ls->prev_line_bottom == 0 && ls->linesep_upper > ls->block_offset_h)
            ls->block_offset_h = ls->linesep_upper;
        ls->shift_baseline = 0;
    }
    if (desc > ls->line_desc)
        ls->line_desc = desc;

    csp = ls->cell_spacing.col * cscale / 100;
    ls->line_width += ls->font_size * cscale / 100 + csp;
    ls->prev_char_sep = csp;
}

static void fixup_linesep(ISDBSubContext *ctx)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    char tmp[16];
    int lsp;

    if (ls->prev_break_idx <= 0)
        return;
    // adjust baseline if all chars in the line are 50% tall of one font size.
    if (ls->shift_baseline && IS_HORIZONTAL_LAYOUT(ls->format)) {
        int delta = ls->cell_spacing.row / 4;
        ls->linesep_upper += delta;
        ls->line_desc -= delta;
        av_log(ctx, AV_LOG_VERBOSE, "baseline shifted down %dpx.\n", delta);
    }

    // not the first line
    tmp[0]='\0';
    lsp = ls->prev_line_desc + ls->linesep_upper;
    av_strlcatf(tmp, sizeof(tmp), "{\\lsp%d}", lsp);
    insert_str(ctx, tmp, ls->prev_break_idx);
}

static void do_line_break(ISDBSubContext *ctx)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    int csp;
    char tmp[32];

    if (IS_HORIZONTAL_LAYOUT(ls->format))
        csp = ls->cell_spacing.col * ls->font_scale.fscx / 100;
    else
        csp = ls->cell_spacing.col * ls->font_scale.fscy / 100;

    if (ls->line_width == 0) {
        if (ls->prev_line_bottom == 0) {
            if (IS_HORIZONTAL_LAYOUT(ls->format))
                ls->block_offset_h = csp / 2;
            else
                ls->block_offset_v = csp / 2;
        }

        // avoid empty lines by prepending a space,
        // as ASS halves line-spacing of empty lines.
        append_str(ctx, "\xe3\x80\x80");
        advance(ctx);
    }
    fixup_linesep(ctx);

    ls->prev_break_idx = ctx->text.used;
    tmp[0] = '\0';
    av_strlcatf(tmp, sizeof(tmp), "\\N{\\lsp0\\fsp%d}", ls->cell_spacing.col);
    append_str(ctx, tmp);
    ls->prev_line_desc = ls->line_desc;
    ls->prev_line_bottom  += ls->linesep_upper + ls->line_height + ls->line_desc;
    ls->prev_char_sep = csp;
    ls->line_height = 0;
    ls->line_width = 0;
    ls->line_desc = 0;
    ls->linesep_upper = 0;
    ls->shift_baseline = 0;
}

// a new char will be appended. check h-spacing and line-breaking.
static void pre_advance(ISDBSubContext *ctx)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    int csp, cscale;
    int w;
    int blocklen;
    int delta;

    if (IS_HORIZONTAL_LAYOUT(ls->format)) {
        cscale = ls->font_scale.fscx;
        w = ls->font_size * cscale / 100;
        csp = ls->cell_spacing.col * cscale / 100;
        if (ls->prev_line_bottom == 0 && ls->line_width == 0)
            ls->block_offset_h = csp / 2;
        blocklen = ls->display_area.w - ls->block_offset_h;
    } else {
        cscale = ls->font_scale.fscy;
        w = ls->font_size * cscale / 100;
        csp = ls->cell_spacing.col * cscale / 100;
        if (ls->prev_line_bottom == 0 && ls->line_width == 0)
            ls->block_offset_v = csp / 2;
        blocklen = ls->display_area.h - ls->block_offset_v;
    }
    if (csp != ls->prev_char_sep)
        delta = ((ls->prev_char_sep + 1) / 2 + csp / 2) - ls->prev_char_sep;
    else
        delta = 0;

    // check line break;
    // note: at the head of a line, line breaking is useless.
    if (ls->line_width != 0 &&
        ls->line_width + delta + w > blocklen) {
        av_log(ctx, AV_LOG_DEBUG, "auto line break at %lu.\n", ctx->text.used);
        do_line_break(ctx);
    } else if (ls->line_width != 0 && delta != 0) {
        char tmp[32];

        // need to compensate the wrong charsep of the previously added char.
        // hack: insert zero-width space with the compensating \fsp. (can be <0)
        //  note that \fsp is scaled by \fscx in ASS.
        tmp[0] = '\0';
        av_strlcatf(tmp, sizeof(tmp), "{\\fsp%d}\xe2\x80\x8b{\\fsp%d}",
                        delta * 100 / cscale, ls->cell_spacing.col);
        append_str(ctx, tmp);
        ls->line_width += delta;
    }
}

static void forward_position(ISDBSubContext *ctx, int rows, int cols)
{
    int i;

    for (i = 0; i < rows; i++)
        do_line_break(ctx);
    for (i = 0; i < cols; i++) {
        pre_advance(ctx);
        append_str(ctx, "\xe3\x80\x80");
        advance(ctx);
    }
    if (rows > 0 && cols > 0)
        ctx->current_state.layout_state.shift_baseline = 1;
}

// NOTE: state->layout_state.format must be set before calling this func.
static void init_layout(struct isdbsub_layout *l)
{
    l->font_size = 36;
    l->display_area.x = 0;
    l->display_area.y = 0;
    switch (l->format) {
    case ISDBSUB_FMT_960H:
        l->display_area.w = 960;
        l->display_area.h = 540;
        l->cell_spacing.col = 4;
        l->cell_spacing.row = 24;
        break;
    case ISDBSUB_FMT_960V:
        l->display_area.w = 960;
        l->display_area.h = 540;
        l->cell_spacing.col = 12;
        l->cell_spacing.row = 24;
        break;
    case ISDBSUB_FMT_720H:
        l->display_area.w = 720;
        l->display_area.h = 480;
        l->cell_spacing.col = 4;
        l->cell_spacing.row = 16;
        break;
    case ISDBSUB_FMT_720V:
        l->display_area.w = 720;
        l->display_area.h = 480;
        l->cell_spacing.col = 8;
        l->cell_spacing.row = 24;
        break;
    }
    // profile C uses a fixed format,
    //  which does not define specific position or size and
    //  just requires to display texts in either 16x3 or 12x4 characters.
    // we use ISDBSUB_FMT_960H for the base format.
    if (l->is_profile_c) {
        l->display_area.x = 160;
        l->display_area.y = 360;
        l->display_area.w = 640;
        l->display_area.h = 180;
    }
}

static void reset_state(ISDBSubState *state)
{
    struct isdbsub_layout *l = &state->layout_state;
    struct b24str_state *s = &state->text_state;

    state->need_init = 1;
    state->clut_high_idx = 0;
    state->fg_color = Default_clut[7];  // white
    state->bg_color = Default_clut[8];  // translucent
    state->mat_color = Default_clut[8]; // FIXME: should be set in init_layout()?

    l->block_offset_h = l->cell_spacing.col / 2;
    l->block_offset_v = l->cell_spacing.row / 2;

    l->font_scale.fscx = 100;
    l->font_scale.fscy = 100;

    l->prev_char_sep = 0;
    l->prev_line_desc = 0;
    l->prev_line_bottom = 0; // 0 means blcok_offset & pen_pos. are not defined yet.
    l->line_height = 0;
    l->line_width = 0;
    l->line_desc = 0;
    l->linesep_upper = 0;
    l->prev_break_idx = 0;

    l->shift_baseline = 0;
    l->repeat_count = -1;
    l->in_combining = 0;
    l->scroll.direction = SCROLL_DIR_NONE;

    s->gl = 0; // G0
    s->gr = 2; // G2
    s->ss = 0; // not in SS{2,3}

    s->g[0].mb = 2;
    s->g[0].code = CODE_JISX0208;
    s->g[1].mb = 1;
    s->g[1].code = CODE_ASCII;
    s->g[2].mb = 1;
    s->g[2].code = CODE_X_HIRA;
    s->g[3].mb = 1;
    s->g[3].code = CODE_X_MACRO;

    // profile C uses different default.
    if (l->is_profile_c) {
        s->g[3].mb = 1;
        s->g[3].code = CODE_X_DRCS_MIN;
        s->gl = 3;
        s->gr = 0;
    }
}

static void clear_text(ISDBSubContext *ctx)
{
    ctx->text.used = 0;
    ctx->text.txt_tail = 0;
}

static void set_format(ISDBSubContext *ctx)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;

    clear_text(ctx);
    reset_state(&ctx->current_state);

    av_free(ctx->script_info);
    ctx->script_info = av_asprintf("[Script Info]\nScript Type: v4.00+\n"
        "PlayDepth: 24\nPlayResX: %d\nPlayResY: %d\nLanguage: ja\n"
        "WrapStyle: 2\nStreamingMode: 1\n\n",
        LAYOUT_GET_WIDTH(ls->format), LAYOUT_GET_HEIGHT(ls->format));
}

static void advance_by_pixels(ISDBSubContext *ctx, int csp)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    int cscale;
    int csep_orig;
    char tmp[32];

    if (IS_HORIZONTAL_LAYOUT(ls->format))
        cscale = ls->font_scale.fscx;
    else
        cscale = ls->font_scale.fscy;
    csep_orig = ls->cell_spacing.col * cscale / 100;

    tmp[0] = '\0';
    av_strlcatf(tmp, sizeof(tmp), "{\\fsp%d}\xe2\x80\x8b{\\fsp%d}",
        csp * 100 / cscale, ls->cell_spacing.col);
    append_str(ctx, tmp);
    ls->line_width += csp;
    ls->prev_char_sep = csep_orig;
    av_log(ctx, AV_LOG_DEBUG, "advanced %d pixel using fsp.\n", csp);
}

// move pen position to (col, row) relative to display area's top left.
//  Note 1: In vertical layout, coordinates are rotated 90 deg.
//          on the display area's top right.
//  Note 2: the cell includes line/char spacings in both sides.
static void move_penpos(ISDBSubContext *ctx, int col, int row)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    int csp_l, col_ofs;
    int cur_bottom;
    int cell_height;
    int cell_desc;

    av_log(ctx, AV_LOG_DEBUG, "move pen pos. to (%d, %d).\n", col, row);
    if (IS_HORIZONTAL_LAYOUT(ls->format)) {
        // convert pen pos. to upper left of the cell.
        cell_height = (ls->font_size + ls->cell_spacing.row)
                        * ls->font_scale.fscy / 100;
        if (ls->font_scale.fscy == 200)
            cell_desc = ls->cell_spacing.row / 2;
        else
            cell_desc = ls->cell_spacing.row * ls->font_scale.fscy / 200;
        row -= cell_height;

        csp_l = ls->cell_spacing.col * ls->font_scale.fscx / 200;
        if (ls->line_width == 0 && ls->prev_line_bottom == 0)
            ls->block_offset_h = csp_l;
        col_ofs = ls->block_offset_h;
    } else {
        cell_height = (ls->font_size + ls->cell_spacing.row)
                        * ls->font_scale.fscx / 100;
        cell_desc = cell_height / 2;
        row -= cell_height - cell_desc;

        csp_l = ls->cell_spacing.col * ls->font_scale.fscy / 200;
        if (ls->line_width == 0 && ls->prev_line_bottom == 0)
            ls->block_offset_v = csp_l;
        col_ofs = ls->block_offset_v;
    }

    cur_bottom = ls->prev_line_bottom +
                    ls->linesep_upper + ls->line_height + ls->line_desc;
    // allow adjusting +- cell_height/2 at maximum
    //     to align to the current line bottom.
    if (row + cell_height / 2 > cur_bottom) {
        // (col, row) is below the current line bottom

        do_line_break(ctx); // ls->prev_line_bottom == cur_bottom
        ls->linesep_upper = row + cell_height - cell_desc - ls->prev_line_bottom;
        ls->line_height = 0;

        advance_by_pixels(ctx, col + csp_l - col_ofs);
    } else if (row + cell_height * 3 / 2 > cur_bottom &&
                col + csp_l > col_ofs + ls->line_width) {
        // append to the current line...
        advance_by_pixels(ctx, col + csp_l - (col_ofs + ls->line_width));
    } else {
        av_log(ctx, AV_LOG_INFO, "backward move not supported.\n");
        return;
    }
}

static void set_position(ISDBSubContext *ctx, unsigned int p1, unsigned int p2)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    int cw, ch;
    int col, row;


    if (IS_HORIZONTAL_LAYOUT(ls->format)) {
        cw = (ls->font_size + ls->cell_spacing.col) * ls->font_scale.fscx / 100;
        ch = (ls->font_size + ls->cell_spacing.row) * ls->font_scale.fscy / 100;
        // pen position is at bottom left
        col = p2 * cw;
        row = p1 * ch + ch;
    } else {
        cw = (ls->font_size + ls->cell_spacing.col) * ls->font_scale.fscy / 100;
        ch = (ls->font_size + ls->cell_spacing.row) * ls->font_scale.fscx / 100;
        // pen position is at upper center,
        // but in -90deg rotated coordinates, it is at middle left.
        col = p2 * cw;
        row = p1 * ch + ch / 2;
    }
    move_penpos(ctx, col, row);
}

static void set_color(ISDBSubContext *ctx, int whatcolor, uint32_t color)
{
    char fmt[32];

    if (whatcolor < 1 || whatcolor > 5)
        return;

    fmt[0] = '\0';
    av_strlcatf(fmt, sizeof(fmt), "{\\%dc&H%06X&\\%da&H%02X&}",
                whatcolor, color & 0xffffff, whatcolor, color >> 24);
    append_str(ctx, fmt);
    av_log(ctx, AV_LOG_DEBUG, "(%d)-th color set to 0x%08X.\n",
           whatcolor, color);
}

static void set_font_scale(ISDBSubContext *ctx)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    char fmt[24];

    fmt[0] = '\0';
    av_strlcatf(fmt, sizeof(fmt), "{\\fscx%d\\fscy%d}",
                ls->font_scale.fscx, ls->font_scale.fscy);
    append_str(ctx, fmt);
    av_log(ctx, AV_LOG_DEBUG, "font scale: (%d, %d)\n",
           ls->font_scale.fscx, ls->font_scale.fscy);
}

static const uint8_t *get_csi_params(const uint8_t *q,
                                     unsigned int *p1, unsigned int *p2)
{
    if (!p1)
        return NULL;

    *p1 = 0;
    for (; *q >= 0x30 && *q <= 0x39; q++) {
        *p1 *= 10;
        *p1 += *q - 0x30;
    }
    if (!p2)
        return q;
    *p2 = 0;
    if (*q != 0x20 && *q != 0x3B)
        return NULL;
    for (q++; *q >= 0x30 && *q <= 0x39; q++) {
        *p2 *= 10;
        *p2 += *q - 0x30;
    }
    return q;
}

static void append_event(ISDBSubContext *ctx);

static const uint8_t *proc_ctl(ISDBSubContext *ctx,
                               const uint8_t *buf, int buf_size)
{
    ISDBSubState *state = &ctx->current_state;
    struct b24str_state *ts = &state->text_state;
    struct isdbsub_layout *ls = &state->layout_state;
    uint8_t code;
    unsigned int p1, p2;
    int i, mb;
    const uint8_t *q;

    while (buf_size > 0) {
        code = buf[0];
        if ((code & 0x60) != 0)
            break;

        buf++;
        buf_size--;
        p1 = p2 = 0;
        switch (code) {
        case 0x00: // NULL
            break;

        // cursor
        case 0x09: // APF
            // append a space
            forward_position(ctx, 0, 1);
            break;
        case 0x16: // PAPF (0x40..0x7F)
            if (buf_size < 1)
                return NULL;
            p1 = buf[0] & 0x3F;
            buf++;
            buf_size--;
            forward_position(ctx, 0, p1);
            break;
        case 0x0D: // APR
            forward_position(ctx, 1, 0);
            av_log(ctx, AV_LOG_DEBUG, "explicit line break.\n");
            break;
        case 0x1C: // APS (0x40..0x7F, 0x40..0x7F)
            if (buf_size < 2)
                return NULL;
            p1 = buf[0] & 0x3F;
            p2 = buf[1] & 0x3F;
            buf += 2;
            buf_size -= 2;
            av_log(ctx, AV_LOG_DEBUG, "aps: (%d, %d).\n", p1, p2);
            set_position(ctx, p1, p2);
            break;

        case 0x0C: // CS
            if (!state->rollup_mode) {
                init_layout(ls);
                reset_state(state);
                clear_text(ctx);
                av_log(ctx, AV_LOG_DEBUG, "screen cleared.\n");
            }
            break;

        // iso2022 text state
        case 0x0E: // LS1
            ts->ss = 0;
            ts->gl = 1;
            break;
        case 0x0F: // LS0
            ts->ss = 0;
            ts->gl = 0;
            break;
        case 0x19: // SS2
            ts->ss = 2;
            break;
        case 0x1D: // SS3
            ts->ss = 3;
            break;
        case 0x1B: // ESC(....)
            if (buf_size < 1)
                return NULL;
            p1 = buf[0];
            buf++;
            buf_size--;
            mb = 1;
            switch (p1) {
            case 0x6E: // LS2
            case 0x6F: // LS3
                ts->gl = 2 + (p1 & 0x01);
                break;
            case 0x7E: // LS1R
            case 0x7D: // LS2R
            case 0x7C: // LS3R
                ts->gr = 0x7F - p1;
                break;

            // 0x28-0x2B (F)  designate 1B set to G0-G3
            // 0x24 [0x29-2B] (F) designate 2B set to G0-G3
            // 0x28-0x2B 0x20 (F) designate 1B DRCS/MACRO to G0-G3
            // 0x24 0x28-0x2B 0x20 (F) designate 2B DRCS to G0-G3
            case 0x24:
                if (buf_size < 1)
                    return NULL;
                mb = 2;
                p1 = buf[0];
                if (p1 >= 0x28 && p1 <= 0x2B) {
                    buf++;
                    buf_size--;
                } else
                    p1 = 0x28;
                // fall through
            case 0x28:
            case 0x29:
            case 0x2A:
            case 0x2B:
                if (buf_size < 1)
                    return NULL;
                p2 = buf[0];
                buf++;
                buf_size--;
                if (p2 == 0x20) {
                    if (buf_size < 1)
                        return NULL;
                    p2 = buf[0];
                    buf++;
                    buf_size--;
                }
                ts->g[p1 - 0x28].mb = mb;
                ts->g[p1 - 0x28].code = p2;
                break;
            default:
                av_log(ctx, AV_LOG_VERBOSE,
                       "unknown escape sequence: 0x%02hhx\n", p1);
            }
            break;

        // color
        case 0x80: // BKF
        case 0x81: // RDF
        case 0x82: // GRF
        case 0x83: // YLF
        case 0x84: // BLF
        case 0x85: // MGF
        case 0x86: // CNF
        case 0x87: // WHF
            p1 = (state->clut_high_idx << 4) | (code & 0x0F);
            state->fg_color = Default_clut[p1];
            set_color(ctx, 1, state->fg_color);
            break;
        case 0x90: // COL(0x48..0x7F) or (0x20, 0x40..0x4F)
            if (buf_size < 1)
                return NULL;
            p1 = buf[0];
            buf++;
            buf_size--;
            if (p1 == 0x20) {
                if (buf_size < 1)
                    return NULL;
                state->clut_high_idx = (buf[0] & 0x0F);
                buf++;
                buf_size--;
                break;
            }
            p2 = (state->clut_high_idx << 4) | (p1 & 0x0F);
            if ((p1 & 0xF0) == 0x40) {
                state->fg_color = Default_clut[p2];
                set_color(ctx, 1, state->fg_color);
            }
            break;

        // character size
        case 0x88: // SSZ
            ls->font_scale.fscx = 50;
            ls->font_scale.fscy = 50;
            set_font_scale(ctx);
            break;
        case 0x89: // MSZ
            ls->font_scale.fscx = 50;
            ls->font_scale.fscy = 100;
            set_font_scale(ctx);
            break;
        case 0x8A: // NSZ
            ls->font_scale.fscx = 100;
            ls->font_scale.fscy = 100;
            set_font_scale(ctx);
            break;
        case 0x8B: // SZX ({0x41,0x44,0x45})
            if (buf_size < 1)
                return NULL;
            p1 = buf[0];
            buf++;
            buf_size--;
            ls->font_scale.fscx = 100;
            ls->font_scale.fscy = 100;
            if ((p1 & 0xFB) == 0x41)
                ls->font_scale.fscy = 200;
            if ((p1 & 0xFE) == 0x44)
                ls->font_scale.fscx = 200;
            set_font_scale(ctx);
            break;

        case 0x98: // RPC (0x40..0x7F)
            if (buf_size < 1)
                return NULL;
            p1 = buf[0] & 0x3F;
            buf++;
            buf_size--;
            ls->repeat_count = p1;
            av_log(ctx, AV_LOG_DEBUG, "repeat char %d times.\n", p1);
            break;

        case 0x99: // SPL
            append_str(ctx, "{\\u0}");
            break;
        case 0x9A: // STL
            append_str(ctx, "{\\u1}");
            break;

        case 0x9B: // CSI
            for (i = 0; i < buf_size; i++)
                if (buf[i] >= 0x40 && buf[i] <= 0x6F)
                    break;
            if (i == buf_size) {
                buf += buf_size;
                buf_size = 0;
                break;
            }
            switch (buf[i]) {
            // SWF1 (0x30..0x39, ...., 0x20, 0x53)
            // -SWF2 ({0x38,0x3F?}, 0x3B, 0x30..0x32, 0x3B, 0x30..0x39, ...., [0x3B, 0x30..0x39,...,] 0x20, 0x53)
            case 0x53:
                q = get_csi_params(buf, &p1, NULL);
                if (!q || *q != 0x20 || p1 < 7 || p1 > 10)
                    break;
                ls->format = ISDBSUB_FMT_960H + (p1 - 7);
                set_format(ctx); // calls reset_state, thus sate->need_init = 1
                av_log(ctx, AV_LOG_DEBUG, "new format: %d\n", ls->format);
                break;
            // SDF (0x30..0x39,..., 0x3B, 0x30..0x39,..., 0x20, 0x56)
            case 0x56:
                q = get_csi_params(buf, &p1, &p2);
                if (!q || *q != 0x20)
                    break;
                ls->display_area.w = p1;
                ls->display_area.h = p2;
                state->need_init = 1;
                av_log(ctx, AV_LOG_DEBUG,
                       "new disp. area size: (%u, %u)\n", p1, p2);
                break;
            // SDP (0x30..0x39,..., 0x3B, 0x30..0x39,..., 0x20, 0x5F)
            case 0x5F:
                q = get_csi_params(buf, &p1, &p2);
                if (!q || *q != 0x20)
                    break;
                ls->display_area.x = p1;
                ls->display_area.y = p2;
                state->need_init = 1;
                av_log(ctx, AV_LOG_DEBUG,
                       "new disp. area pos: (%u, %u)\n", p1, p2);
                break;

            // RCS (0x30..0x39,...., 0x20, 0x6E)
            case 0x6E:
                q = get_csi_params(buf, &p1, NULL);
                if (!q || *q != 0x20 || p1 > 15)
                    break;
                state->mat_color = Default_clut[state->clut_high_idx << 4 | p1];
                set_color(ctx, 5, state->mat_color);
                break;

            // SSM (0x30..0x39,..., 0x3B, 0x30..0x39,..., 0x20, 0x57)
            case 0x57:
                q = get_csi_params(buf, &p1, &p2);
                if (!q || *q != 0x20 || p1 != p2)
                    break;
                if (p1 == 16 || p1 == 20 || p1 == 24 || p1 == 30 || p1 == 36) {
                    char fs[8];

                    ls->font_size = p1;
                    fs[0] = '\0';
                    av_strlcatf(fs, sizeof(fs), "{\\fs%d}", p1);
                    append_str(ctx, fs);
                    if (ls->line_width != 0)
                        ls->shift_baseline = 0;
                    av_log(ctx, AV_LOG_DEBUG, "font size:%d\n", p1);
                }
                break;
            // SHS (0x30..0x39,..., 0x20, 0x58)
            // SVS (0x30..0x39,..., 0x20, 0x59)
            case 0x58:
            case 0x59:
                q = get_csi_params(buf, &p1, NULL);
                if (!q || *q != 0x20)
                    break;
                if (code == 0x58)
                    ls->cell_spacing.col = p1;
                else {
                    ls->cell_spacing.row = p1;
                    if (ls->line_width != 0)
                        ls->shift_baseline = 0;
                }
                av_log(ctx, AV_LOG_DEBUG, "%c-spacing:%u.\n",
                       (code == 0x58) ? 'h' : 'v', p1);
                // no output here.  automatically inserted later in proc_char().
                break;

            // ORN (0x30, 0x20, 0x63) or (0x31, 0x3B, 0x30..0x39 * 4, 0x20, 0x63)
            case 0x63:
                q = get_csi_params(buf, &p1, &p2);
                if (!q)
                    break;
                if (p1 == 0) // no ornaments
                    append_str(ctx, "{\\bord0\\3a&HFF&\\3c&H000000&}");
                else if (p1 == 1 && *q == 0x20) { // outline
                    int idx = (p2 / 100) << 4 | (p2 % 100);
                    if (idx < 0 || idx > 127)
                        break;
                    append_str(ctx, "{\\bord1}");
                    set_color(ctx, 3, Default_clut[idx]);
                }
                break;

            // SCR (0x30..0x34, 0x3B, 0x30..0x39..., 0x20, 0x67)
            case 0x67:
                q = get_csi_params(buf, &p1, &p2);
                if (!q || *q != 0x20 || p1 > 4)
                    break;
                if (p1 != 0)
                    clear_text(ctx);
                ls->scroll.direction = (p1 + 1) / 2;
                ls->scroll.rollout = (p1 == 2 || p1 == 4);
                if (p2 == 0)
                    p2 = 1;
                ls->scroll.speed = p2;
                break;

            // ACPS (0x30..0x39,...., 0x3B, 0x30..0x39,...., 0x20, 0x61)
            case 0x61:
                q = get_csi_params(buf, &p1, &p2);
                if (!q || *q != 0x20)
                    break;
                if ((signed) p1 < ls->display_area.x
                    || (signed) p1 >= ls->display_area.x + ls->display_area.w
                    || (signed) p2 < ls->display_area.y
                    || (signed) p2 >= ls->display_area.y + ls->display_area.h) {
                    av_log(ctx, AV_LOG_VERBOSE,
                           "invalid parameters (%u, %u) in ACPS.\n", p1, p2);
                    break;
                }
                if (IS_HORIZONTAL_LAYOUT(ls->format))
                    move_penpos(ctx, p1 - ls->display_area.x,
                                p2 - ls->display_area.y);
                else
                    move_penpos(ctx, ls->display_area.w - (p1 - ls->display_area.x),
                                p2 - ls->display_area.y);
                break;

            // -CCC ({0x30, 0x32..0x34}, 0x20, 0x54)
            // -PLD (0x5B)
            // -PLU (0x5C)
            // -GSM (0x30..0x39,..., 0x3B, 0x30..0x39,..., 0x20, 0x42)
            // -GAA ({0x30,0x31}, 0x20, 0x5D)
            // -SRC (0x30..0x33, 0x3B, 0x30..0x39 * 4, 0x20, 0x5E)
            // -TCC (0x30..0x3A, 0x3B, 0x30..0x33, 0x3B, 0x30..0x39,..., 0x20, 0x62)
            // -CFS (0x30..0x39,..., 0x20, 0x65)
            // -MDF (0x30..0x33, 0x20, 0x64)
            // -XCS (0x30..0x31, 0x20, 0x66)
            // PRA (0x30..0x39,..., 0x20, 0x68)
            // -ACS (0x30, 0x20, 0x69)....(0x9B, 0x31,0x20,0x69) {(0x9B,{0x32,0x34},0x20,0x69)...(0x9B,{0x33,0x35},0x20, 0x69)}+
            // -SCS (0x6F)(0x9B,...).....(0x9B,0x6F)
            default:
                av_log(ctx, AV_LOG_VERBOSE, "invalid/un-supported CSI. "
                       "terminating code:0x%02hhx len:%d\n", buf[i], i);
            }
            i++;
            buf += i;
            buf_size -= i;
            break;

        // non-supported control codes
        case 0x9D: // -TIME (0x20, 0x40..0x7F) or (0x28, 0x40..0x43) or (0x29, ...., 0x40..0x43)
            if (buf_size < 2)
                return NULL;
            if (buf[0] == 0x20 || buf[0] == 0x28) {
                if (buf[0] == 0x20) {
                    // wait. copy & split out as another event
                    ctx->duration = (buf[1] - 0x40) * 100;
                    append_event(ctx);
                }
                buf += 2;
                buf_size -= 2;
                break;
            }
            if (buf[0] != 0x29)
                return NULL;
            while (buf_size > 0 && (buf[0] & 0xFC) != 0x40) {
                buf++;
                buf_size--;
            }
            if ((buf[0] & 0xFC) != 0x40)
                return NULL;
            // buf_size > 0
            buf++;
            buf_size--;
            av_log(ctx, AV_LOG_VERBOSE,
                   "un-supported control code: 0x%02hhx\n", code);
            break;

        case 0x95: // -MACRO (0x4F) or (0x40..0x41, 0x21..0x7E, ...., 0x95, 0x4F)
            if (buf_size < 1)
                return NULL;
            if (buf[0] == 0x40 || buf[0] == 0x41) {
                while (buf_size > 1 && buf[0] != 0x95) {
                    buf++;
                    buf_size--;
                }
                if (buf[0] != 0x95)
                    return NULL;
                // buf_size > 1
                buf++;
                buf_size--;
            }
            if (buf[0] != 0x4F)
                return NULL;
            buf++;
            buf_size--;
            av_log(ctx, AV_LOG_VERBOSE,
                   "un-supported control code: 0x%02hhx\n", code);
            break;

        case 0x92: // -CDC ({0x40,0x4F}) or (0x20, 0x40..0x4A)
            if (buf_size >= 2 && buf[0] == 0x20) {
                buf++;
                buf_size--;
            }
            // fall through
        case 0x91: // -FLC ({0x40,0x47,0x4F})
        case 0x93: // -POL (0x40..0x42)
        case 0x94: // -WHM ({0x40,0x44,0x45})
        case 0x97: // -HLC (0x40..0x4F)
            if (buf_size < 1)
                return NULL;
            buf++;
            buf_size--;
            // fall through
        case 0x08: // APB
        case 0x0A: // APD
        case 0x0B: // -APU
        default: // un-used/unknown control codes
            av_log(ctx, AV_LOG_VERBOSE,
                   "unknown/ignored control code: 0x%02hhx\n", code);
            break;
        }
    }
    return buf;
}

// called at the start of sub-text or after each SWF/SDP/SDF occurrences.
// Define some style overrides at the start of Dialog line outputs,
// which are not defined in [style] nor ASS default.
// Note that SDP/SDF don't reset the other style parameters.
// Note2: multiple calls of this func safely overwrite the previous defs.
static void setup_line_head(ISDBSubContext *ctx)
{
    ISDBSubState *state = &ctx->current_state;
    struct isdbsub_layout *ls = &state->layout_state;
    char item[256];
    int cscale;

    if (IS_HORIZONTAL_LAYOUT(ls->format))
        cscale = ls->font_scale.fscx;
    else
        cscale = ls->font_scale.fscy;

    item[0] = '\0';
    // lsp will be corrected later.
    // Note: ASS scales \fsp by font scale of the preceeding char.
    av_strlcatf(item, sizeof(item), "{\\lsp0\\fsp%d}", ls->cell_spacing.col);
    ls->prev_char_sep = ls->cell_spacing.col * cscale / 100;

    av_strlcatf(item, sizeof(item), "{\\fs%d}", ls->font_size);
    if (ls->font_scale.fscx != 100)
        av_strlcatf(item, sizeof(item), "{\\fscx%d}", ls->font_scale.fscx);
    if (ls->font_scale.fscy != 100)
        av_strlcatf(item, sizeof(item), "{\\fscy%d}", ls->font_scale.fscy);

    if (state->fg_color != 0x00FFFFFF) {
        if ((state->fg_color & 0xFFFFFF) != 0xFFFFFF)
            av_strlcatf(item, sizeof(item), "{\\1c&H%06X&}",
                state->fg_color & 0xFFFFFF);
        if ((state->fg_color >> 24) != 0x00)
            av_strlcatf(item, sizeof(item), "{\\1a&H%02X&}",
                state->fg_color >> 24);
    }
    if (state->bg_color != 0xFF000000) {
        if ((state->bg_color & 0xFFFFFF) != 0)
            av_strlcatf(item, sizeof(item), "{\\4c&H%06X&}",
                state->bg_color & 0xFFFFFF);
        if ((state->bg_color >> 24) != 0xFF)
            av_strlcatf(item, sizeof(item), "{\\4a&H%02X&}",
                state->bg_color >> 24);
    }
    if (state->mat_color != 0xFF000000) {
        if ((state->mat_color & 0xFFFFFF) != 0)
            av_strlcatf(item, sizeof(item), "{\\5c&H%06X&}",
                state->mat_color & 0xFFFFFF);
        if ((state->mat_color >> 24) != 0xFF)
            av_strlcatf(item, sizeof(item), "{\\5a&H%02X&}",
                state->mat_color >> 24);
    }

    prepend_str(ctx, item);
    state->need_init = 0;
}

static const uint8_t *
proc_char(ISDBSubContext *ctx, const uint8_t *buf, int buf_size);

static void process_txt_du(ISDBSubContext *ctx, const uint8_t *buf, int buf_size)
{
    const char *p, *q;

    ctx->current_state = ctx->default_states[ctx->lang_tag];
    reset_state(&ctx->current_state);
    iconv(ctx->iconv, NULL, NULL, NULL, NULL);

    p = buf;
    while (buf_size > 0) {
        q = proc_ctl(ctx, p, buf_size); // update ctx->current_state
        if (!q)
            break;
        buf_size -= q - p;
        p = q;
        q = proc_char(ctx, p, buf_size); // append to ctx->text
        ctx->text.txt_tail = ctx->text.used;
        if (!q)
            break;
        buf_size -= q - p;
        p = q;
        if (ctx->current_state.need_init)
            setup_line_head(ctx);
    }
}


static void process_mngmnt_dg(ISDBSubContext *ctx, const uint8_t *buf,
                                int buf_size)
{
    const uint8_t *p = buf;
    int lang_num;
    int du_loop_len;
    int i;

    ctx->timing_mode = *(p++) >> 6;
    if (ctx->timing_mode == ISDBSUB_TMD_OFFSET) {
        ctx->offset.hour = (p[0] >> 4) * 10  + (p[0] & 0x0f);
        ctx->offset.min = (p[1] >> 4) * 10  + (p[1] & 0x0f);
        ctx->offset.sec = (p[2] >> 4) * 10  + (p[2] & 0x0f);
        ctx->offset.ms = (p[3] >> 4) * 100  + (p[3] & 0x0f) * 10 + (p[4] >> 4);
        p += 5;
    }
    lang_num = *(p++);
    for (i = 0; i < lang_num; i++) {
        int lang_tag = *p >> 5;
        int disp_mode = *p & 0x0f;

        p++;
        if ((disp_mode & 0x0c) == 0x0c)
            p++; // skip display condition
        p += 3; // skip lang code
        if (lang_tag < ISDBSUB_MAX_LANG) {
            ISDBSubState *state = &ctx->default_states[lang_tag];
            int format;
            state->auto_display = disp_mode >> 3;
            state->rollup_mode = (*p & 0x03);
            format = *p >> 4;
            state->layout_state.is_profile_c = (format == 0x0F);
            if (state->layout_state.is_profile_c)
                format = ISDBSUB_FMT_960H;
            if (format < ISDBSUB_FMT_960H || format > ISDBSUB_FMT_720V) {
                av_log(ctx, AV_LOG_INFO,
                       "illegal format:0x%02x in sub. mngment data.\n", format);
                format = ISDBSUB_FMT_960H; // fallback
            }
            state->layout_state.format = format;
            if ((*p & 0x0c) != 0x00)
                av_log(ctx, AV_LOG_INFO,
                       "char encoding:%d not supported.\n", ((*p & 0x0c) >> 2));
            init_layout(&state->layout_state);
            reset_state(state);
            clear_text(ctx);
        }
        p++;
    }
    ctx->current_state = ctx->default_states[ctx->lang_tag];
    set_format(ctx);

    du_loop_len = AV_RB24(p);
    p += 3;
    if (p + du_loop_len > buf + buf_size)
        du_loop_len = buf + buf_size - p;
    while (du_loop_len >= 5) {
        int du_size = AV_RB24(p + 2);

        if (p[0] != ISDBSUB_UNIT_SEP || du_loop_len < 5 + du_size)
            break;
        du_loop_len -= (5 + du_size);

        // ignore DRCS. only txt data units.
        if (p[1] == ISDBSUB_DU_TYPE_TXT) {
            memdump(ctx, p + 5, du_size);
            process_txt_du(ctx, p + 5, du_size); // control seq. only
            // copy back SWF, SDP, SDF,...
            ctx->default_states[ctx->lang_tag] = ctx->current_state;
        }
        p += 5 + du_size;
    }
}


static const uint8_t * const combining1[] = {
    "\xcc\x81", // U+0301 combinng accute accent
    "\xcc\x80", // U+0300 combining grave accent
    "\xcc\x88", // U+0308 combining diaeresis
    "\xcc\x82", // U+0302 combining circumflex accent
    "\xcc\x84", // U+0304 combining macron
    "\xcc\xb2", // U+0332 combining low line
};
static const uint8_t * const hira_symbols[9] = {
    "\xe3\x82\x9d" /* U+309D */, "\xe3\x82\x9e" /* U+309E */,
    "\xe3\x83\xbc" /* U+30FC */, "\xe3\x80\x82" /* U+3002 */,
    "\xe3\x80\x8c" /* U+300C */, "\xe3\x80\x8d" /* U+300D */,
    "\xe3\x80\x81" /* U+3001 */, "\xe3\x83\xbb" /* U+30FB */,
};
static const uint8_t * const kata_symbols[9] = {
    "\xe3\x83\xbd" /* U+30FD */, "\xe3\x83\xbe" /* U+30FE */,
    "\xe3\x83\xbc" /* U+30FC */, "\xe3\x80\x82" /* U+3002 */,
    "\xe3\x80\x8c" /* U+300C */, "\xe3\x80\x8d" /* U+300D */,
    "\xe3\x80\x81" /* U+3001 */, "\xe3\x83\xbb" /* U+30FB */,
};
/* ARIB STD B24 table.7-11 -> UTF-8 */
static const uint8_t * const trans_ext85[] = {
    /*   1- 10 */
    "\xe3\x90\x82", "\xf0\xa0\x85\x98", "\xe4\xbb\xbd", "\xe4\xbb\xbf", "\xe4\xbe\x9a",
    "\xe4\xbf\x89", "\xe5\x82\x9c", "\xe5\x84\x9e", "\xe5\x86\xbc", "\xe3\x94\x9f",
    /*  11- 20 */
    "\xe5\x8c\x87", "\xe5\x8d\xa1", "\xe5\x8d\xac", "\xe5\xa9\xb9", "\xf0\xa0\xae\xb7",
    "\xe5\x91\x8d", "\xe5\x92\x96", "\xe5\x92\x9c", "\xe5\x92\xa9", "\xe5\x94\x8e",
    /*  21- 30 */
    "\xe5\x95\x8a", "\xe5\x99\xb2", "\xe5\x9b\xa4", "\xe5\x9c\xb3", "\xe5\x9c\xb4",
    "\xef\xa8\x90", "\xe5\xa2\x80", "\xe5\xa7\xa4", "\xe5\xa8\xa3", "\xe5\xa9\x95",
    /*  31- 40 */
    "\xe5\xaf\xac", "\xef\xa8\x91", "\xe3\x9f\xa2", "\xe5\xba\xac", "\xe5\xbc\xb4",
    "\xe5\xbd\x85", "\xe5\xbe\xb7", "\xe6\x80\x97", "\xe6\x81\xb5", "\xe6\x84\xb0",
    /*  41- 50 */
    "\xe6\x98\xa4", "\xe6\x9b\x88", "\xe6\x9b\x99", "\xe6\x9b\xba", "\xe6\x9b\xbb",
    "\xee\x82\x88", "\xe6\xa2\x81", "\xe6\xa4\x91", "\xe6\xa4\xbb", "\xe6\xa9\x85",
    /*  51- 60 */
    "\xe6\xaa\x91", "\xe6\xab\x9b", "\xf0\xa3\x8f\x8c", "\xf0\xa3\x8f\xbe", "\xf0\xa3\x97\x84",
    "\xe6\xaf\xb1", "\xe6\xb3\xa0", "\xe6\xb4\xae", "\xef\xa9\x85", "\xe6\xb6\xbf",
    /*  61- 70 */
    "\xe6\xb7\x8a", "\xe6\xb7\xb8", "\xef\xa9\x86", "\xe6\xbd\x9e", "\xe6\xbf\xb9",
    "\xe7\x81\xa4", "\xe7\x85\x95", "\xf0\xa4\x8b\xae", "\xe7\x85\x87", "\xe7\x87\x81",
    /*  71- 80 */
    "\xe7\x88\x80", "\xe7\x8e\x9f", "\xe7\x8e\xa8", "\xe7\x8f\x89", "\xe7\x8f\x96",
    "\xe7\x90\x9b", "\xe7\x90\xa1", "\xef\xa9\x8a", "\xe7\x90\xa6", "\xe7\x90\xaa",
    /*  81- 90 */
    "\xe7\x90\xac", "\xe7\x90\xb9", "\xe7\x91\x8b", "\xe3\xbb\x9a", "\xe7\x95\xb5",
    "\xe7\x96\x81", "\xe7\x9d\xb2", "\xe4\x82\x93", "\xe7\xa3\x88", "\xe7\xa3\xa0",
    /*  91-100 */
    "\xe7\xa5\x87", "\xe7\xa6\xae", "\xe7\xa5\x93", "\xe8\xa2\x82", "\xe8\xa5\xa6",
    "\xe7\xa7\x9a", "\xe7\xa8\x9e", "\xe7\xad\xbf", "\xe7\xb0\xb1", "\xe4\x89\xa4",
    /* 101-110 */
    "\xe7\xb6\x8b", "\xe7\xbe\xa1", "\xe8\x84\x98", "\xe8\x84\xba", "\xe8\x88\x98",
    "\xe8\x8a\xae", "\xe8\x91\x9b", "\xe8\x93\x9c", "\xe8\x93\xac", "\xe8\x95\x99",
    /* 111-120 */
    "\xe8\x97\x8e", "\xe8\x9d\x95", "\xe8\x9f\xac", "\xe8\xa0\x8b", "\xe8\xa3\xb5",
    "\xe8\xa7\x92", "\xe8\xab\xb6", "\xe8\xb7\x8e", "\xe8\xbe\xbb", "\xe8\xbf\xb6",
    /* 121-130 */
    "\xe9\x83\x9d", "\xe9\x84\xa7", "\xe9\x84\xad", "\xe9\x86\xb2", "\xe9\x88\xb3",
    "\xe9\x8a\x88", "\xe9\x8c\xa1", "\xe9\x8d\x88", "\xe9\x96\x92", "\xe9\x9b\x9e",
    /* 131-137 */
    "\xe9\xa4\x83", "\xe9\xa5\x80", "\xe9\xab\x99", "\xe9\xaf\x96", "\xe9\xb7\x97",
    "\xe9\xba\xb4", "\xe9\xba\xb5",
};

static const uint8_t * const trans_ext90[][96] = {
  { /* row 90 */
    /*   1- 10 */
    "\xe2\x9b\x8c", "\xe2\x9b\x8d", "\xe2\x9d\x97", "\xe2\x9b\x8f", "\xe2\x9b\x90",
    "\xe2\x9b\x91", "", "\xe2\x9b\x92", "\xe2\x9b\x95", "\xe2\x9b\x93",
    /*  11- 20 */
    "\xe2\x9b\x94", "", "", "", "", "",
    "\xf0\x9f\x85\xbf", "\xf0\x9f\x86\x8a", "", "", "\xe2\x9b\x96",
    /*  21- 30 */
    "\xe2\x9b\x97", "\xe2\x9b\x98", "\xe2\x9b\x99", "\xe2\x9b\x9a", "\xe2\x9b\x9b",
    "\xe2\x9b\x9c", "\xe2\x9b\x9d", "\xe2\x9b\x9e", "\xe2\x9b\x9f", "\xe2\x9b\xa0",
    /*  31- 40 */
    "\xe2\x9b\xa1", "\xe2\xad\x95", "\xe3\x89\x88", "\xe3\x89\x89", "\xe3\x89\x8a",
    "\xe3\x89\x8b", "\xe3\x89\x8c", "\xe3\x89\x8d", "\xe3\x89\x8e", "\xe3\x89\x8f",
    /*  41- 50 */
    "", "", "", "", "\xe2\x92\x91",
    "\xe2\x92\x92", "\xe2\x92\x93", "\xf0\x9f\x85\x8a", "\xf0\x9f\x85\x8c", "\xf0\x9f\x84\xbf",
    /*  51- 60 */
    "\xf0\x9f\x85\x86", "\xf0\x9f\x85\x8b", "\xf0\x9f\x88\x90", "\xf0\x9f\x88\x91", "\xf0\x9f\x88\x92",
    "\xf0\x9f\x88\x93", "\xf0\x9f\x85\x82", "\xf0\x9f\x88\x94", "\xf0\x9f\x88\x95", "\xf0\x9f\x88\x96",
    /*  61- 70 */
    "\xf0\x9f\x85\x8d", "\xf0\x9f\x84\xb1", "\xf0\x9f\x84\xbd", "\xe2\xac\x9b", "\xe2\xac\xa4",
    "\xf0\x9f\x88\x97", "\xf0\x9f\x88\x98", "\xf0\x9f\x88\x99", "\xf0\x9f\x88\x9a", "\xf0\x9f\x88\x9b",
    /*  71- 80 */
    "\xe2\x9a\xbf", "\xf0\x9f\x88\x9c", "\xf0\x9f\x88\x9d", "\xf0\x9f\x88\x9e", "\xf0\x9f\x88\x9f",
    "\xf0\x9f\x88\xa0", "\xf0\x9f\x88\xa1", "\xf0\x9f\x88\xa2", "\xf0\x9f\x88\xa3", "\xf0\x9f\x88\xa4",
    /*  81- 84 */
    "\xf0\x9f\x88\xa5", "\xf0\x9f\x85\x8e", "\xe3\x8a\x99", "\xf0\x9f\x88\x80",
  },
  { /* row 91 */
    /*   1- 10 */
    "\xe2\x9b\xa3", "\xe2\xad\x96", "\xe2\xad\x97", "\xe2\xad\x98", "\xe2\xad\x99",
    "\xe2\x98\x93", "\xe3\x8a\x8b", "\xe3\x80\x92", "\xe2\x9b\xa8", "\xe3\x89\x86",
    /*  11- 20 */
    "\xe3\x89\x85", "\xe2\x9b\xa9", "\xe0\xbf\x96", "\xe2\x9b\xaa", "\xe2\x9b\xab",
    "\xe2\x9b\xac", "\xe2\x99\xa8", "\xe2\x9b\xad", "\xe2\x9b\xae", "\xe2\x9b\xaf",
    /*  21- 30 */
    "\xe2\x9a\x93", "\xe2\x9c\x88", "\xe2\x9b\xb0", "\xe2\x9b\xb1", "\xe2\x9b\xb2",
    "\xe2\x9b\xb3", "\xe2\x9b\xb4", "\xe2\x9b\xb5", "\xf0\x9f\x85\x97", "\xe2\x92\xb9",
    /*  31- 40 */
    "\xe2\x93\x88", "\xe2\x9b\xb6", "\xf0\x9f\x85\x9f", "\xf0\x9f\x86\x8b", "\xf0\x9f\x86\x8d",
    "\xf0\x9f\x86\x8c", "\xf0\x9f\x85\xb9", "\xe2\x9b\xb7", "\xe2\x9b\xb8", "\xe2\x9b\xb9",
    /*  41- 49 */
    "\xe2\x9b\xba", "\xf0\x9f\x85\xbb", "\xe2\x98\x8e", "\xe2\x9b\xbb", "\xe2\x9b\xbc",
    "\xe2\x9b\xbd", "\xe2\x9b\xbe", "\xf0\x9f\x85\xbc", "\xe2\x9b\xbf",
  },
  { /* row 92 */
    /*   1- 10 */
    "\xe2\x9e\xa1", "\xe2\xac\x85", "\xe2\xac\x86", "\xe2\xac\x87", "\xe2\xac\xaf",
    "\xe2\xac\xae", "\xe5\xb9\xb4", "\xe6\x9c\x88", "\xe6\x97\xa5", "\xe5\x86\x86",
    /*  11- 20 */
    "\xe3\x8e\xa1", "\xe3\x8e\xa5", "\xe3\x8e\x9d", "\xe3\x8e\xa0", "\xe3\x8e\xa4",
    "\xf0\x9f\x84\x80", "\xe2\x92\x88", "\xe2\x92\x89", "\xe2\x92\x8a", "\xe2\x92\x8b",
    /*  21- 30 */
    "\xe2\x92\x8c", "\xe2\x92\x8d", "\xe2\x92\x8e", "\xe2\x92\x8f", "\xe2\x92\x90",
    "\xe6\xb0\x8f", "\xe5\x89\xaf", "\xe5\x85\x83", "\xe6\x95\x85", "\xe5\x89\x8d",
    /*  31- 40 */
    "\xe6\x96\xb0", "\xf0\x9f\x84\x81", "\xf0\x9f\x84\x82", "\xf0\x9f\x84\x83", "\xf0\x9f\x84\x84",
    "\xf0\x9f\x84\x85", "\xf0\x9f\x84\x86", "\xf0\x9f\x84\x87", "\xf0\x9f\x84\x88", "\xf0\x9f\x84\x89",
    /*  41- 50 */
    "\xf0\x9f\x84\x8a", "\xe3\x88\xb3", "\xe3\x88\xb6", "\xe3\x88\xb2", "\xe3\x88\xb1",
    "\xe3\x88\xb9", "\xe3\x89\x84", "\xe2\x96\xb6", "\xe2\x97\x80",  "\xe3\x80\x96",
    /*  51- 60 */
    "\xe3\x80\x97", "\xe2\x9f\x90", "\xc2\xb2", "\xc2\xb3", "\xf0\x9f\x84\xad",
    /* no unicode chars for musical score symbols (56...85)  */
    "?", "?", "?", "?", "?",
    /*  61- 70 */
    "?", "?", "?", "?", "?",  "?", "?", "?", "?", "?",
    /*  71- 80 */
    "?", "?", "?", "?", "?",  "?", "?", "?", "?", "?",
    /*  81- 90 */
    "?", "?", "?", "?", "?",
    "\xf0\x9f\x84\xac", "\xf0\x9f\x84\xab", "\xe3\x89\x87", "\xf0\x9f\x86\x90", "\xf0\x9f\x88\xa6",
    /*  91     */
    "\xe2\x84\xbb",
  },
  { /* row 93 */
    /*   1- 10 */
    "\xe3\x88\xaa", "\xe3\x88\xab", "\xe3\x88\xac", "\xe3\x88\xad", "\xe3\x88\xae",
    "\xe3\x88\xaf", "\xe3\x88\xb0", "\xe3\x88\xb7", "\xe3\x8d\xbe", "\xe3\x8d\xbd",
    /*  11- 20 */
    "\xe3\x8d\xbc", "\xe3\x8d\xbb", "\xe2\x84\x96", "\xe2\x84\xa1", "\xe3\x80\xb6",
    "\xe2\x9a\xbe", "\xf0\x9f\x89\x80", "\xf0\x9f\x89\x81", "\xf0\x9f\x89\x82", "\xf0\x9f\x89\x83",
    /*  21- 30 */
    "\xf0\x9f\x89\x84", "\xf0\x9f\x89\x85", "\xf0\x9f\x89\x86", "\xf0\x9f\x89\x87", "\xf0\x9f\x89\x88",
    "\xf0\x9f\x84\xaa", "\xf0\x9f\x88\xa7", "\xf0\x9f\x88\xa8", "\xf0\x9f\x88\xa9", "\xf0\x9f\x88\x94",
    /*  31- 40 */
    "\xf0\x9f\x88\xaa", "\xf0\x9f\x88\xab", "\xf0\x9f\x88\xac", "\xf0\x9f\x88\xad", "\xf0\x9f\x88\xae",
    "\xf0\x9f\x88\xaf", "\xf0\x9f\x88\xb0", "\xf0\x9f\x88\xb1", "\xe2\x84\x93", "\xe3\x8e\x8f",
    /*  41- 50 */
    "\xe3\x8e\x90", "\xe3\x8f\x8a", "\xe3\x8e\x9e", "\xe3\x8e\xa2", "\xe3\x8d\xb1",
    "", "", "\xc2\xbd", "\xe2\x86\x89", "\xe2\x85\x93",
    /*  51- 60 */
    "\xe2\x85\x94", "\xc2\xbc", "\xc2\xbe", "\xe2\x85\x95", "\xe2\x85\x96",
    "\xe2\x85\x97", "\xe2\x85\x98", "\xe2\x85\x99", "\xe2\x85\x9a", "\xe2\x85\x90",
    /*  61- 70 */
    "\xe2\x85\x9b", "\xe2\x85\x91", "\xe2\x85\x92", "\xe2\x98\x80", "\xe2\x98\x81",
    "\xe2\x98\x82", "\xe2\x9b\x84", "\xe2\x98\x96", "\xe2\x98\x97", "\xe2\x9b\x89",
    /*  71- 80 */
    "\xe2\x9b\x8a", "\xe2\x99\xa6", "\xe2\x99\xa5", "\xe2\x99\xa3","\xe2\x99\xa0",
    "\xe2\x9b\x8b", "\xe2\xa8\x80", "\xe2\x80\xbc", "\xe2\x81\x89", "\xe2\x9b\x85",
    /*  81- 90 */
    "\xe2\x98\x94", "\xe2\x9b\x86", "\xe2\x98\x83", "\xe2\x9b\x87", "\xe2\x9a\xa1",
    "\xe2\x9b\x87", "", "\xe2\x9a\x9e", "\xe2\x9a\x9f", "\xe2\x99\xac",
    /*  91     */
    "\xe2\x98\x8e",
  },
  { /* row 94 */
    /*   1- 10 */
    "\xe2\x85\xa0", "\xe2\x85\xa1", "\xe2\x85\xa2", "\xe2\x85\xa3", "\xe2\x85\xa4",
    "\xe2\x85\xa5", "\xe2\x85\xa6", "\xe2\x85\xa7", "\xe2\x85\xa8", "\xe2\x85\xa9",
    /*  11- 20 */
    "\xe2\x85\xaa", "\xe2\x85\xab", "\xe2\x91\xb0", "\xe2\x91\xb1", "\xe2\x91\xb2",
    "\xe2\x91\xb3", "\xe2\x91\xb4", "\xe2\x91\xb5", "\xe2\x91\xb6", "\xe2\x91\xb7",
    /*  21- 30 */
    "\xe2\x91\xb8", "\xe2\x91\xb9", "\xe2\x91\xba", "\xe2\x91\xbb", "\xe2\x91\xbc",
    "\xe2\x91\xbd", "\xe2\x91\xbe", "\xe2\x91\xbf", "\xe3\x89\x91", "\xe3\x89\x92",
    /*  31- 40 */
    "\xe3\x89\x93", "\xe3\x89\x94", "\xf0\x9f\x84\x90", "\xf0\x9f\x84\x91", "\xf0\x9f\x84\x92",
    "\xf0\x9f\x84\x93", "\xf0\x9f\x84\x94", "\xf0\x9f\x84\x95", "\xf0\x9f\x84\x96", "\xf0\x9f\x84\x97",
    /*  41- 50 */
    "\xf0\x9f\x84\x98", "\xf0\x9f\x84\x99", "\xf0\x9f\x84\x9a", "\xf0\x9f\x84\x9b", "\xf0\x9f\x84\x9c",
    "\xf0\x9f\x84\x9d", "\xf0\x9f\x84\x9e", "\xf0\x9f\x84\x9f", "\xf0\x9f\x84\xa0", "\xf0\x9f\x84\xa1",
    /*  51- 60 */
    "\xf0\x9f\x84\xa2", "\xf0\x9f\x84\xa3", "\xf0\x9f\x84\xa4", "\xf0\x9f\x84\xa5", "\xf0\x9f\x84\xa6",
    "\xf0\x9f\x84\xa7", "\xf0\x9f\x84\xa8", "\xf0\x9f\x84\xa9", "\xe3\x89\x95", "\xe3\x89\x96",
    /*  61- 70 */
    "\xe3\x89\x97", "\xe3\x89\x98", "\xe3\x89\x99", "\xe3\x89\x9a", "\xe2\x91\xa0",
    "\xe2\x91\xa1", "\xe2\x91\xa2", "\xe2\x91\xa3", "\xe2\x91\xa4", "\xe2\x91\xa5",
    /*  71- 80 */
    "\xe2\x91\xa6", "\xe2\x91\xa7", "\xe2\x91\xa8", "\xe2\x91\xa9", "\xe2\x91\xaa",
    "\xe2\x91\xab", "\xe2\x91\xac", "\xe2\x91\xad", "\xe2\x91\xae", "\xe2\x91\xaf",
    /*  81- 90 */
    "\xe2\x9d\xb6", "\xe2\x9d\xb7", "\xe2\x9d\xb8", "\xe2\x9d\xb9", "\xe2\x9d\xba",
    "\xe2\x9d\xbb", "\xe2\x9d\xbc", "\xe2\x9d\xbd", "\xe2\x9d\xbe", "\xe2\x9d\xbf",
    /*  91- 93 */
    "\xe2\x93\xab", "\xe2\x93\xac", "\xe3\x89\x9b",
  }
};

static unsigned char append_arib_char (ISDBSubContext *ctx, uint8_t c1, uint8_t c2)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    struct b24str_state *ts = &ctx->current_state.text_state;
    int gidx;
    uint8_t code;
    size_t ilen, olen, ret;
    char indata[3], *obuf, *ibuf;
    const uint8_t *p;
    uint8_t *q;
    int len;

    ibuf = indata;
    // make room for a new char. (at least 6 + 1 bytes)
    reserve_buf(ctx, 7);

    obuf = ctx->text.buf + ctx->text.used;
    olen = ctx->text.len - ctx->text.used;

    if (ts->ss > 1)
        gidx = ts->ss;
    else if (c1 & 0x80)
        gidx = ts->gr;
    else
        gidx = ts->gl;
    code = ts->g[gidx].code;

    // some hacks
    if ((c1 & 0x7F) == 0x20 || (c1 & 0x7F) == 0x7F) // SP or DEL
        code = CODE_ASCII;
    if (code == CODE_JISX0208 && (c1 & 0x7F) >= 0x75)
        code = CODE_EXT;

    switch (code) {
    case CODE_ASCII:
    case CODE_ASCII2:
        ls->in_combining = 0;
        c1 &= 0x7F;
        if (c1 != 0x7F)
            ctx->text.buf[ctx->text.used++] = c1; // no conversion needed
        else {
            // replace DELL with black-square (U+25A0)
            memcpy(ctx->text.buf + ctx->text.used, "\xe2\x96\xa0", 3);
            ctx->text.used += 3;
        }
        break;

    case CODE_JISX0208:
    case CODE_JISX0213_1:
        c1 |= 0x80;
        c2 |= 0x80;
        // non-spacing char?
        if ((c1 == 0xA1 && c2 >= 0xAD && c2 <= 0xB2) ||
            (c1 == 0xA2 && c2 == 0xFE)) {
            ls->in_combining = 1;
            // U+20DD (0xe2839d) combining enclosing circle
            p = (c1 == 0xA1) ? combining1[c2 - 0xAD] :
                                    (const uint8_t *) "\xe2\x83\x9d";
            len = strlen(p);
            memcpy(ctx->text.buf + ctx->text.used, p, len);
            ctx->text.used += len;
            break;
        }
        ls->in_combining = 0;
        ibuf[0] = c1;
        ibuf[1] = c2;// ibuf[]: EUC-JP2013 kanji
        ilen = 2;
        ret = iconv(ctx->iconv, &ibuf, &ilen, &obuf, &olen);
        if (ret != (size_t) -1)
            ctx->text.used += obuf - &ctx->text.buf[ctx->text.used];
        else
            av_log(ctx, AV_LOG_VERBOSE,
                   "bad data as EUC-JP(%d). sp:%lu 0x%02hhx 0x%02hhx\n",
                   errno, olen, c1, c2);
        break;
    case CODE_JISX0213_2:
        ls->in_combining = 0;
        c1 |= 0x80;
        c2 |= 0x80;
        ilen = 3;
        ibuf[0] = '\x8f';
        ibuf[0] = c1;
        ibuf[1] = c2;
        ret = iconv(ctx->iconv, &ibuf, &ilen, &obuf, &olen);
        if (ret != (size_t) -1)
            ctx->text.used += obuf - &ctx->text.buf[ctx->text.used];
        else
            av_log(ctx, AV_LOG_VERBOSE,
                   "bad data as EUC-JP. 0x8f 0x%02hhx 0x%02hhx\n", c1, c2);
        break;

    case CODE_JISX0201_KATA:
        ls->in_combining = 0;
        c1 &= 0x7F;
        // map to U+FF60 + (c1 - 0x20) = U+FF00 + (c1 + 0x40)
        c1 += 0x40;
        q = ctx->text.buf + ctx->text.used;
        q[0] = 0xef;
        q[1] = 0xbc + (c1 >> 6);
        q[2] = 0x80 + (c1 & 0x3F);
        ctx->text.used += 3;
        break;

    case CODE_X_HIRA:
    case CODE_X_HIRA_P:
    case CODE_X_KATA:
    case CODE_X_KATA_P:
        ls->in_combining = 0;
        c1 &= 0x7F;
        if (c1 < 0x77) {
            q = ctx->text.buf + ctx->text.used;
            // hira: {U+3040+ (c1 - 0x20)} -> {U+3000 + (c1 + 0x20)}
            // kata: {U+30A0 + (c1 - 0x20)} -> {U+3000 + (c1 + 0x80)}
            c1 += (code == CODE_X_HIRA || code == CODE_X_HIRA_P) ? 0x20 : 0x80;
            q[0] = 0xe3;
            q[1] = 0x80 + (c1 >> 6);
            q[2] = 0x80 + (c1 & 0x3F);
            ctx->text.used += 3;
        } else {
            c1 -= 0x77;
            p = (code == CODE_X_HIRA || code == CODE_X_HIRA_P) ?
                    hira_symbols[c1] : kata_symbols[c1];
            len = strlen(p);
            memcpy(ctx->text.buf + ctx->text.used, p, len);
            ctx->text.used += len;
        }
        break;

    case CODE_EXT: // symbols defined in row 85.86, 90..94
        ls->in_combining = 0;
        c1 &= 0x7F;
        c2 &= 0x7F;
        switch (c1) {
        case 0x75: // row 85-86
        case 0x76:
            c2 -= 0x21;
            if (c1 == 0x76)
                c2 += 94;
            if (c2 < 137) {
                p = trans_ext85[c2];
                len = strlen(p);
                memcpy(ctx->text.buf + ctx->text.used, p, len);
                ctx->text.used += len;
            } else
                av_log(ctx, AV_LOG_VERBOSE, "bad data as extra char.\n");
            break;
        case 0x7A: // row 90-94
        case 0x7B:
        case 0x7C:
        case 0x7D:
        case 0x7E:
            c1 -= 0x7A;
            c2 -= 0x21;
            p = trans_ext90[c1][c2];
            len = strlen(p);
            if (len > 0) {
                memcpy(ctx->text.buf + ctx->text.used, p, len);
                ctx->text.used += len;
            } else
                av_log(ctx, AV_LOG_VERBOSE, "bad data as extra char.\n");
            break;

        default:
            av_log(ctx, AV_LOG_VERBOSE,
                   "un-supported data 0x%02hhx 0x%02hhx.\n", c1, c2);
        }
        break;

    // non-spacing mosaic chars... just skip.
    // don't touch in_combining, as other non-spacing char's may follow/precede...
    case CODE_MOSAIC_C:
    case CODE_MOSAIC_D:
        break;

    case CODE_X_MACRO:
        break; // processed later

    default:
        ls->in_combining = 0;
        // replace this unknown char with U+FFFD
        memcpy(ctx->text.buf + ctx->text.used, "\xef\xbf\xbd", 3);
        ctx->text.used += 3;
        av_log(ctx, AV_LOG_VERBOSE,
            "unknown charset:0x%02hhx data 0x%02hhx 0x%02hhx.\n", code, c1, c2);
        break;
    }
    ctx->text.buf[ctx->text.used] = '\0';
    return code;
}

static const uint8_t *proc_char(ISDBSubContext *ctx,
                                const uint8_t *buf, int buf_size)
{
    struct isdbsub_layout *ls = &ctx->current_state.layout_state;
    struct b24str_state *ts = &ctx->current_state.text_state;
    unsigned int begin, end;
    char *ch = NULL;

    begin = end = ctx->text.used;

    while (buf_size > 0 && (buf[0] & 0x60) != 0) {
        uint8_t c1, c2 = 0;
        unsigned char ctype;

        c1 = buf[0];
        buf ++;
        buf_size --;

        if ((c1 & 0x7F) != 0x20 && (c1 & 0x7F) != 0x7F) { // not (SP or DEL)
            // multi-byte?
            if ((ts->ss > 0 && ts->g[ts->ss].mb > 1)
                || (!ts->ss && (c1 & 0x80) && ts->g[ts->gr].mb > 1)
                || (!ts->ss && !(c1 & 0x80) && ts->g[ts->gl].mb > 1)) {
                if (buf_size < 1)
                    return NULL;
                c2 = buf[0];
                buf ++;
                buf_size --;
            }
        }

        if (!ls->in_combining) {
            pre_advance(ctx);
            begin = end = ctx->text.used;
        }
        ctype = append_arib_char(ctx, c1, c2);
        ts->ss = 0;
        if (ctype == 0)
            continue;
        if (ctype == CODE_X_MACRO) {
            av_log(ctx, AV_LOG_DEBUG, "macro 0x%02hhx.\n", c1);
            if ((c1 & 0x70) == 0x60) {
                c1 &= 0x0F;
                proc_ctl(ctx, Default_macro[c1], strlen(Default_macro[c1]));
            }
            continue;
        }

        // if non-spacing sequence has terminated,..
        if (!ls->in_combining && end - begin > 0 && ctx->text.used > end) {
            char tmp[8]; // UTF-8 char is 6bytes at maximum....
            unsigned int len;

           len = ctx->text.used - end;
            if (len > sizeof(tmp)) // for safety.
                len = sizeof(tmp);
            memcpy(tmp, ctx->text.buf + end, len);
            memmove(ctx->text.buf + begin + len, ctx->text.buf + begin, end - begin);
            memcpy(ctx->text.buf + begin, tmp, len);
            av_log(ctx, AV_LOG_DEBUG,
                "moved the terminating spacing char to the head(%d).\n", begin);
         }
        end = ctx->text.used;
        if (ls->in_combining)
            continue;

        // ls->in_combining == 0
        advance(ctx);

        // repeat
        if (ls->repeat_count >= 0) {
            // save the repeating char first.
            ch = av_mallocz(end - begin + 1);
            if (ch)
                memcpy(ch, ctx->text.buf + begin, end - begin);
            else {
                av_log(ctx, AV_LOG_WARNING,
                       "out of memory for repeating char.\n");
                ls->repeat_count = -1;
            }
        }

        if (ls->repeat_count == 0) {
            pre_advance(ctx);
            while (ls->line_width != 0) {
                append_str(ctx, ch);
                advance(ctx);
                // prepare for the next one.
                pre_advance(ctx);
            }
        } else if (ls->repeat_count > 0) {
            while (--ls->repeat_count > 0) {
                pre_advance(ctx);
                append_str(ctx, ch);
                advance(ctx);
            }
        }
        if (ls->repeat_count > -1)
            av_free(ch);

        ls->repeat_count = -1;
    }
    if (buf_size <= 0)
        return NULL;
    // buf_size > 0 => assert((buf[0] & 0x60) == 0);
    return buf;
}

static void append_event(ISDBSubContext *ctx)
{
    ISDBSubState *state = &ctx->current_state;
    struct isdbsub_layout *l = &state->layout_state;
    char start[16], end[16];
    struct margins m;
    char effect[36], clipping[64];
    char *dialog;
    char c0 = 0;

    if (ctx->pts == AV_NOPTS_VALUE || ctx->text.buf == NULL || !ctx->text.used)
        return;
    fixup_linesep(ctx);

    av_log(ctx, AV_LOG_DEBUG, "append_event: %lu\n", ctx->text.used);

    if (ctx->duration >= 0 && ctx->duration < 100)
        ctx->duration = 100;

    pts_to_str(ctx->pts, start);
    pts_to_str(ctx->pts + ctx->duration, end);

    effect[0] = '\0';
    get_margins(ctx, &m);
    if (state->rollup_mode) {
        if (IS_HORIZONTAL_LAYOUT(ctx->current_state.layout_state.format))
            av_strlcatf(effect, sizeof(effect), "Scroll up;;;");
        else
            av_strlcatf(effect, sizeof(effect), "Banner;;;;"); // LtoR
    } else if ((IS_HORIZONTAL_LAYOUT(l->format) &&
                l->scroll.direction == SCROLL_DIR_COLUMN) ||
               (!IS_HORIZONTAL_LAYOUT(l->format) &&
                l->scroll.direction == SCROLL_DIR_ROW))
        av_strlcatf(effect, sizeof(effect), "Banner;%d;%d;0;%d",
                        1000 / l->scroll.speed,
                        (l->scroll.direction == SCROLL_DIR_ROW),
                        !l->scroll.rollout);
    else if (l->scroll.direction != SCROLL_DIR_NONE)
        av_strlcatf(effect, sizeof(effect), "Scroll up;%d;%d;%d;%d",
                        IS_HORIZONTAL_LAYOUT(l->format) ? m.v : m.l,
                        l->display_area.y + l->display_area.h,
                        1000 / l->scroll.speed,
                        !l->scroll.rollout);

    clipping[0] = '\0';
    av_strlcatf(clipping, sizeof(clipping), "{\\clip(%d,%d,%d,%d)}",
        l->display_area.x, l->display_area.y,
        l->display_area.x + l->display_area.w,
        l->display_area.y + l->display_area.h);

    // control sequences for the next event may be appended.
    if (ctx->text.txt_tail != ctx->text.used) {
        c0 = ctx->text.buf[ctx->text.txt_tail];
        ctx->text.buf[ctx->text.txt_tail] = '\0';
    }

    dialog = av_asprintf("Dialogue: %s,%s,%s,%d,%d,%d,%s,%s%s\n",
        l->is_profile_c ? "prof_c" :
            (IS_HORIZONTAL_LAYOUT(l->format) ? "hstyle" : "vstyle"),
        pts_to_str(ctx->pts, start), pts_to_str(ctx->pts + ctx->duration, end),
        m.l, m.r, m.v, effect, clipping, ctx->text.buf);

    if (ctx->events) {
        char *orig = ctx->events;
        ctx->events = av_asprintf("%s%s", ctx->events, dialog);
        av_free(orig);
        av_free(dialog);
    } else
        ctx->events = dialog;

    if (ctx->duration > 0 && ctx->duration < INT_MAX)
        ctx->pts += ctx->duration;
    ctx->duration = INT_MAX;
    state->need_init = 1;

    if (ctx->text.txt_tail != ctx->text.used) {
        ctx->text.buf[ctx->text.txt_tail] = c0;
        memmove(ctx->text.buf, ctx->text.buf + ctx->text.txt_tail,
                ctx->text.used - ctx->text.txt_tail);
        ctx->text.used -= ctx->text.txt_tail;
        ctx->text.txt_tail = 0;
    }
}

static void process_sub_dg(ISDBSubContext *ctx, const uint8_t *buf, int buf_size)
{
    const uint8_t *p = buf;
    int du_loop_len;

    if (ctx->pts == AV_NOPTS_VALUE) {
        av_log(ctx, AV_LOG_VERBOSE, "no timestamp on subtitle text data.");
        return;
    }
    clear_text(ctx);
    setup_line_head(ctx);

    ctx->timing_mode = *(p++) >> 6;
    // subtitle data should be TMD_FREE (ARIB TR-B15).

    if (ctx->timing_mode != ISDBSUB_TMD_FREE) {
        ctx->start.hour = (p[0] >> 4) * 10  + (p[0] & 0x0f);
        ctx->start.min = (p[1] >> 4) * 10  + (p[1] & 0x0f);
        ctx->start.sec = (p[2] >> 4) * 10  + (p[2] & 0x0f);
        ctx->start.ms = (p[3] >> 4) * 100  + (p[3] & 0x0f) * 10 + (p[4] >> 4);
        if (ctx->timing_mode == ISDBSUB_TMD_OFFSET) {
            ctx->start.ms += ctx->offset.ms;
            if (ctx->start.ms >= 1000) {
                ctx->start.ms -= 1000;
                ctx->start.sec ++;
            }
            ctx->start.sec += ctx->offset.sec;
            if (ctx->start.sec >= 60) {
                ctx->start.sec -= 60;
                ctx->start.min ++;
            }
            ctx->start.min += ctx->offset.min;
            if (ctx->start.min >= 60) {
                ctx->start.min -= 60;
                ctx->start.hour ++;
            }
            ctx->start.hour += ctx->offset.hour;
        }
        p += 5;
    }

    du_loop_len = AV_RB24(p);
    p += 3;
    if (p + du_loop_len > buf + buf_size)
        du_loop_len = buf + buf_size - p;
    while (du_loop_len >= 5) {
        int du_size = AV_RB24(p + 2);

        if (p[0] != ISDBSUB_UNIT_SEP || du_loop_len < 5 + du_size)
            break;
        du_loop_len -= (5 + du_size);

        // ignore DRCS. only txt data units.
        if (p[1] == ISDBSUB_DU_TYPE_TXT) {
            memdump(ctx, p + 5, du_size);
            process_txt_du(ctx, p + 5, du_size);
        }
        p += 5 + du_size;
    }
    append_event(ctx);
}


static int add_ass_str(AVSubtitle *sub, const char *txt)
{
    AVSubtitleRect **rects;

    if (!txt || !txt[0])
        return AVERROR(EINVAL);

    rects = av_realloc_array(sub->rects, (sub->num_rects+1), sizeof(*sub->rects));
    if (!rects)
        return AVERROR(ENOMEM);
    sub->rects = rects;
    rects[sub->num_rects]       = av_mallocz(sizeof(*rects[0]));
    if (!rects[sub->num_rects])
        return AVERROR(ENOMEM);
    rects[sub->num_rects]->type = SUBTITLE_ASS;
    rects[sub->num_rects]->ass = strdup(txt);
    sub->num_rects++;
    return 0;
}

static void do_output(ISDBSubContext *ctx, AVSubtitle *sub)
{
    int header = 0;

    if (!ctx->script_info && !ctx->events)
        return;
    av_log(ctx, AV_LOG_DEBUG, "making output.\n");

    if (ctx->script_info) {
        add_ass_str(sub, ctx->script_info);
        av_freep(&ctx->script_info);
        header = 1;
    }
    if (!ctx->is_style_inited) { // once
#define STYLE_PARAMS_FONT \
    ":lang=ja:charset=3000:spacing=dual"
#define STYLE_PARAMS_COMMON \
    "&H00FFFFFF, &HFF000000, &HFF000000, &HFF000000, 1, 0, 0, 1\n"

        add_ass_str(sub, "[V4+ Styles]\n"
            "Format: Name, Fontname, Fontsize, Alignment, "
            "PrimaryColour, OutlineColour, BackColour, ClippingColour, "
            "BorderStyle, Outline, Shadow, treat_fontname_as_pattern\n"
            "Style: vstyle, @" STYLE_PARAMS_FONT ", 36, 9, " STYLE_PARAMS_COMMON
            "Style: hstyle, " STYLE_PARAMS_FONT ", 36, 7, " STYLE_PARAMS_COMMON
            "Style: prof_c, " STYLE_PARAMS_FONT ", 36, 2, " STYLE_PARAMS_COMMON);

#undef STYLE_PARAMS_FONT
#undef STYLE_PARAMS_COMMON
        ctx->is_style_inited = 1;
        header = 1;
    }
    if (ctx->events || header) {
        if (header)
            add_ass_str(sub, "\n[Events]\n"
                "Format: Style, Start, End, MarginL, MarginR, MarginV, "
                "Effect, Text\n");
        add_ass_str(sub, ctx->events);
        av_freep(&ctx->events);
    }

    for (int i = 0; i < sub->num_rects; i++)
        av_log(ctx, AV_LOG_DEBUG, "ass output[%d]:\n    %s\n",
               i, sub->rects[i]->ass);
}


static av_cold int isdbsub_init(AVCodecContext *avctx)
{
    ISDBSubContext *ctx = avctx->priv_data;
    int i;

    for (i = 65; i < 73; i++)
        Default_clut[i] = Default_clut[i - 65] & ~RGBA(0,0,0,128);
    for (i = 73; i < 128; i++)
        Default_clut[i] = Default_clut[i - 64] & ~RGBA(0,0,0,128);

    Crc_table = av_crc_get_table(AV_CRC_16_CCITT);

    ctx->last_mngmnt_id = ISDBSUB_NO_DGID;
    ctx->last_mngmnt_pts = AV_NOPTS_VALUE;
    ctx->iconv = iconv_open("UTF-8", "EUC-JISX0213");
    if (ctx->iconv == (iconv_t) -1)
        return -1;
    return 0;
}

static av_cold int isdbsub_close(AVCodecContext *avctx)
{
    ISDBSubContext *ctx = avctx->priv_data;

    iconv_close(ctx->iconv);

    av_free(ctx->text.buf);
    av_free(ctx->script_info);
    av_free(ctx->events);

    return 0;
}

static int isdbsub_decode_frame(AVCodecContext *avctx,
                                void *data, int *got_sub_ptr, AVPacket *avpkt)
{
    ISDBSubContext *ctx = avctx->priv_data;
    AVSubtitle *sub = data;
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;

    int64_t orig_pts;
    const uint8_t *p, *p_end;

    if (buf_size <= 10 || buf[0] != ISDBSUB_DATA_ID || buf[1] != 0xff) {
        av_log(avctx, AV_LOG_INFO, "incomplete or broken packet as ISDB sub");
        return AVERROR_INVALIDDATA;
    }

    av_log(ctx, AV_LOG_DEBUG, "ISDB sub packet:\n");
    memdump(ctx, buf, buf_size);

    orig_pts = avpkt->pts;
    ctx->pts = orig_pts;
    ctx->duration = INT_MAX; // duration unknown

    if (ctx->pts != AV_NOPTS_VALUE && ctx->last_mngmnt_pts != AV_NOPTS_VALUE) {
        int64_t t = orig_pts;
        char t1[16], t2[16];

        // check pts timeout, but taking PTS wrap-around into account.
        if (t < ctx->last_mngmnt_pts &&
            (ctx->last_mngmnt_pts - t) > (MPEGTS_MAX_PTS >> 1)) {
            av_log(ctx, AV_LOG_INFO, "PTS wrap-around %s -> %s\n",
                pts_to_str(ctx->last_mngmnt_pts, t1), pts_to_str(t, t2));
            t += MPEGTS_MAX_PTS;
        }
        if (t < ctx->last_mngmnt_pts ||
            t - ctx->last_mngmnt_pts > ISDBSUB_MGMNT_TIMEOUT) {
            av_log(ctx, AV_LOG_VERBOSE,
                "Subtitle Management DataGroup time-out. %s -> %s\n",
                pts_to_str(ctx->last_mngmnt_pts, t1), pts_to_str(t, t2));
            ctx->last_mngmnt_pts = AV_NOPTS_VALUE;
        }
    }

    p = buf + 3 + (buf[2] & 0x0f);
    p_end = buf + buf_size;

    while (p_end - p >= 7) {
        int dg_id = p[0] >> 2;
        int dg_size = AV_RB16(p + 3);
        uint32_t crc = 0;

        if (p + 5 + dg_size + 2 > p_end ||
            (crc = av_crc(Crc_table, 0, p, 5 + dg_size + 2)) != 0) {
            av_log(ctx, AV_LOG_INFO, "incomplete or broken packet. "
                "ofs:%ld l:%d crc:0x%04hx\n", p - buf, 5 + dg_size + 2, crc);
            return -1;
        }
        p += 5;

        if ((dg_id & 0x0f) == 0) { // subtile management data group
            char t1[16];

            if (dg_id != ctx->last_mngmnt_id
                || ctx->last_mngmnt_pts == AV_NOPTS_VALUE)
                process_mngmnt_dg(ctx, p, dg_size);
            else
                av_log(ctx, AV_LOG_TRACE,
                    "Skip the same subtitle management data group.\n");

            ctx->last_mngmnt_id = dg_id;
            ctx->last_mngmnt_pts = orig_pts;
            av_log(ctx, AV_LOG_DEBUG,
                "last_mngmnt_pts set to %s\n", pts_to_str(orig_pts, t1));
        } else if ((dg_id & 0x0f) == ctx->lang_tag + 1) { // subtile data group
            if (ctx->last_mngmnt_id == ISDBSUB_NO_DGID)
                av_log(ctx, AV_LOG_TRACE, "no management data group received yet.\n");
            else if ((dg_id & 0xf0) == ctx->last_mngmnt_id)
                process_sub_dg(ctx, p, dg_size);
        } else
            av_log(ctx, AV_LOG_DEBUG,
                "Subtitle data group id 0x%02x, length %d\n", dg_id, dg_size);

        p += dg_size + 2;
    }

    if (orig_pts != AV_NOPTS_VALUE && ctx->last_mngmnt_pts == AV_NOPTS_VALUE) {
        ctx->last_mngmnt_pts = orig_pts;
        av_log(ctx, AV_LOG_VERBOSE,
               "replaced empty last_mngmnt_pts with the next received PTS.\n");
    }

    do_output(ctx, sub);
    *got_sub_ptr = sub->num_rects > 0;
    return avpkt->size;
}



static const AVOption options[] = {
    {"lang_tag", "select a sub-track of multi-lang stream",
        offsetof(ISDBSubContext, lang_tag),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, ISDBSUB_MAX_LANG - 1,
        AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_SUBTITLE_PARAM},
    {NULL}
};

static const AVClass isdbsub_class = {
    .class_name = "ISDB Sub Decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


AVCodec ff_isdbsub_decoder = {
    .name           = "isdbsub",
    .long_name      = NULL_IF_CONFIG_SMALL("ISDB subtitles"),
    .type           = AVMEDIA_TYPE_SUBTITLE,
    .id             = AV_CODEC_ID_ISDB_SUBTITLE,
    .priv_data_size = sizeof(ISDBSubContext),
    .init           = isdbsub_init,
    .close          = isdbsub_close,
    .decode         = isdbsub_decode_frame,
    .priv_class     = &isdbsub_class,
};
