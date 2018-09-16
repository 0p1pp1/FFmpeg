/*
 * DVB protocol
 * Copyright (c) 2015 0p1pp1
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

/**
 * @file
 * DVB protocol
 */

#include "avformat.h"
#include "url.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"

#include <fcntl.h>
#include <limits.h>  /* for PATH_MAX */
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct DVBContext {
    const AVClass *class;
    URLContext *hd;

    char *conf_file_path;
    struct dtv_property prop[DTV_IOCTL_MAX_MSGS];
    struct dtv_properties props;
    int progid;

    int adap_no;
    int fe_no;
    int demux_no;

    int buf_sz;

    int fe_fd;
    int demux_fd;
    int dvr_fd;
} DVBContext;

static const AVOption dvb_options[] = {
    { "adapter", "adapter device number as N in /dev/dvb/adapterN", offsetof(DVBContext, adap_no), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT64_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "frontend", "frontend device number as N in /dev/dvb/adapterX/frontendN", offsetof(DVBContext, fe_no), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT64_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "demux", "demuxer device number as N in /dev/dvb/adapterX/demuxN", offsetof(DVBContext, demux_no), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT64_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "buffer_size", "kernel buffer size of demuxer device", offsetof(DVBContext, buf_sz), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT64_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { "channel_file", "path to the channel configuration file", offsetof(DVBContext, conf_file_path), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "progid", "program id of the channel", offsetof(DVBContext, progid), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 0xffff, AV_OPT_FLAG_READONLY | AV_OPT_FLAG_EXPORT },
    { NULL }
};

/* wait for a status report of locked */
static int check_fe_status(int fd_frontend)
{
    struct pollfd pfd[1];
    fe_status_t   festatus;
    int           i;

    pfd[0].fd = fd_frontend;
    pfd[0].events = POLLPRI;

    festatus = 0;
    i = 0;
    while (1) {
        int ret;

        ret = poll(pfd, 1, 1000);
        if (ret <= 0)
            return -1;

        if (pfd[0].revents & POLLPRI) {
            festatus = 0;
            ret = ioctl(fd_frontend, FE_READ_STATUS, &festatus);
            if (ret == -1 || festatus & FE_TIMEDOUT)
                return -1;
            if (festatus & FE_HAS_LOCK)
                break;

            /*
             * since some(all?) FE drivers don't use FE_TIMEDOUT,
             * check whether lock is lost for 2+ seconds.
             */
            if (++ i > 20)
                return -2;
            usleep(100000);
        }
    }
    return 0;
}

static FILE *get_conf_file(const char *fname)
{
    FILE *fp;
    const char *env;
    char *path;
    const int plen = PATH_MAX + 1;

    if (fname[0] == '/')
        return fopen(fname, "r");

    path = av_malloc(plen);
    if (!path)
        return NULL;
    path[0] = '\0';
    env = getenv("XDG_DATA_HOME");
    if (env && *env)
        av_strlcpy(path, env, plen);
    else {
        env = getenv("HOME");
        if (env && *env)
            snprintf(path, plen, "%s/.local/share", env);
    }

    if (path[0]) {
        av_strlcat(path, "/ffmpeg/", plen);
        av_strlcat(path, fname, plen);
        fp = fopen(path, "r");
        if (fp || errno != ENOENT) {
            av_free(path);
	        return fp;
        }
    }

    env = getenv("XDG_DATA_DIRS");
    if (!env || !*env)
        env = "/usr/local/share:/usr/share";
    fp = NULL;
    do {
        const char *p = strchr(env, ':');

        /* empty item */
        if (p == env)
            goto next;

        if (!p)
            p = env + strlen(env);

        if ((p - env) + strlen("/ffmpeg/") + strlen(fname) >= plen)
            goto next;

        av_strlcpy(path, env, plen);
        av_strlcat(path, "/ffmpeg/", plen);
        av_strlcat(path, fname, plen);
        fp = fopen(path, "r");
        if (fp || errno != ENOENT)
            break;

next:
        env = p;
        if (*env != '\0')
            env ++;
    } while (*env);

    av_free(path);
    return fp;
}

#define S2API_SYMDEF(x) {#x, x}

static const struct symbol_def_s
{
  const char *name;
  const int id;
} s2api_symbols[] = {
    S2API_SYMDEF (DTV_UNDEFINED), S2API_SYMDEF (DTV_TUNE),
    S2API_SYMDEF (DTV_CLEAR), S2API_SYMDEF (DTV_FREQUENCY),
    S2API_SYMDEF (DTV_MODULATION), S2API_SYMDEF (DTV_BANDWIDTH_HZ),
    S2API_SYMDEF (DTV_INVERSION), S2API_SYMDEF (DTV_DISEQC_MASTER),
    S2API_SYMDEF (DTV_SYMBOL_RATE), S2API_SYMDEF (DTV_INNER_FEC),
    S2API_SYMDEF (DTV_VOLTAGE), S2API_SYMDEF (DTV_TONE),
    S2API_SYMDEF (DTV_PILOT), S2API_SYMDEF (DTV_ROLLOFF),
    S2API_SYMDEF (DTV_DISEQC_SLAVE_REPLY),
    S2API_SYMDEF (DTV_FE_CAPABILITY_COUNT), S2API_SYMDEF (DTV_FE_CAPABILITY),
    S2API_SYMDEF (DTV_DELIVERY_SYSTEM),
/*
    S2API_SYMDEF (DTV_ISDBT_PARTIAL_RECEPTION),
    S2API_SYMDEF (DTV_ISDBT_SOUNR_BROADCASTING),
    S2API_SYMDEF (DTV_ISDBT_SB_SUBCHANNEL_ID),
    S2API_SYMDEF (DTV_ISDBT_SB_SEGMENT_IDX),
    S2API_SYMDEF (DTV_ISDBT_SB_SEGMENT_COUNT),
    S2API_SYMDEF (DTV_ISDBT_LAYERA_FEC),
    S2API_SYMDEF (DTV_ISDBT_LAYERA_MODULATION),
    S2API_SYMDEF (DTV_ISDBT_LAYERA_SEGMENT_COUNT),
    S2API_SYMDEF (DTV_ISDBT_LAYERA_TIME_INTERLEAVING),
    S2API_SYMDEF (DTV_ISDBT_LAYERB_FEC),
    S2API_SYMDEF (DTV_ISDBT_LAYERB_MODULATION),
    S2API_SYMDEF (DTV_ISDBT_LAYERB_SEGMENT_COUNT),
    S2API_SYMDEF (DTV_ISDBT_LAYERB_TIME_INTERLEAVING),
    S2API_SYMDEF (DTV_ISDBT_LAYERC_FEC),
    S2API_SYMDEF (DTV_ISDBT_LAYERC_MODULATION),
    S2API_SYMDEF (DTV_ISDBT_LAYERC_SEGMENT_COUNT),
    S2API_SYMDEF (DTV_ISDBT_LAYERC_TIME_INTERLEAVING),
 */
    S2API_SYMDEF (DTV_API_VERSION),
    S2API_SYMDEF (DTV_CODE_RATE_HP), S2API_SYMDEF (DTV_CODE_RATE_LP),
    S2API_SYMDEF (DTV_GUARD_INTERVAL), S2API_SYMDEF (DTV_TRANSMISSION_MODE),
    S2API_SYMDEF (DTV_HIERARCHY),
/*
    S2API_SYMDEF (DTV_ISDBT_LAYER_ENABLED), S2API_SYMDEF (DTV_ISDBS_TS_ID),
 */
    {"DTV_ISDBS_TS_ID", 42},
    S2API_SYMDEF (PILOT_ON), S2API_SYMDEF (PILOT_OFF),
    S2API_SYMDEF (PILOT_AUTO),
    S2API_SYMDEF (ROLLOFF_35), S2API_SYMDEF (ROLLOFF_20),
    S2API_SYMDEF (ROLLOFF_25), S2API_SYMDEF (ROLLOFF_AUTO),
    S2API_SYMDEF (SYS_UNDEFINED), S2API_SYMDEF (SYS_DVBC_ANNEX_AC),
    S2API_SYMDEF (SYS_DVBC_ANNEX_B), S2API_SYMDEF (SYS_DVBT),
    S2API_SYMDEF (SYS_DSS), S2API_SYMDEF (SYS_DVBS),
    S2API_SYMDEF (SYS_DVBS2), S2API_SYMDEF (SYS_DVBH),
    S2API_SYMDEF (SYS_ISDBT), S2API_SYMDEF (SYS_ISDBS),
    S2API_SYMDEF (SYS_ISDBC), S2API_SYMDEF (SYS_ATSC),
    S2API_SYMDEF (SYS_ATSCMH), S2API_SYMDEF (SYS_DMBTH),
    S2API_SYMDEF (SYS_CMMB), S2API_SYMDEF (SYS_DAB),
    S2API_SYMDEF (SEC_VOLTAGE_13), S2API_SYMDEF (SEC_VOLTAGE_18),
    S2API_SYMDEF (SEC_VOLTAGE_OFF),
    {NULL, 0}
};

static int s2api_id (const char *name)
{
    const struct symbol_def_s *p;

    if (name == NULL)
        return 0;
    if (*name == '\0')
        return 0;

    for (p = s2api_symbols; p->name != NULL; p++)
        if (!strcmp(name, p->name))
            return p->id;
    return 0;
}

/* format of channel config file (1 line per 1 channel)
 *  <channel name>:<PROPERTIES>:<program_id>
 *  ( <PROPERTIES> := <prop_name>=<value>|<prop_name>=<value>|.... )
 *    <prop_name>s are DTV_FREQUENCY etc. and defined in linux/dvb/frontend.h.
 *    same format with the ones used in the ISDB-patched mplayer/gstreamer.
 */
static int search_channel(DVBContext *s, const char *chname, const char *fname)
{
    FILE *f;
    char buf[512];

    f = get_conf_file(fname);
    if (!f) {
        av_log(s->hd, AV_LOG_ERROR,
               "%s: Failed to find/open channel conf file:%s.\n",
               __func__, fname);
        return AVERROR(EIO);
    }

    while (fgets(buf, sizeof(buf), f)) {
        char *p, *q;
        int has_tune = 0, has_freq = 0;

        if (strlen(buf) == 0 || buf[0] == '#')
            continue;
        /*
         * FIXME: If a channel name (at the head of buf) is enconded
         +        in a multi-byte charset, it can contains ':'.
         */
        p = strchr(buf, ':');
        q = strrchr(buf, ':');
        if (!p || !q || p == buf || p == q) {
            av_log(s->hd, AV_LOG_WARNING,
                   "search_channel: bad formatted line in the conf file.\n");
            continue;
        }
        *p++ = '\0';
        *q++ = '\0';
        if (strcmp(chname, buf))
            continue;

        s->props.num = 0;
        s->props.props = &s->prop[0];
        s->progid = strtoul(q, NULL, 10);
        if (s->progid <= 0 || s->progid >= 0xffff)
            break;

        /* parse DTV properties */
        for (p = av_strtok(p, "|", &q); p; p = av_strtok(NULL, "|", &q)) {
            const char *prop_conf = "%63[^|=]=%255[^|=]";
            char pname[64], val[256];
            uint32_t cmd;

            if (sscanf(p, prop_conf, pname, val) != 2)
                break;
            if (pname[0] == '\0' || val[0] == '\0')
                break;

            cmd = s2api_id(pname);
            if (cmd == 0)
                cmd = strtoul(pname, NULL, 0);
            if (cmd == 0)
                break;
            s->prop[s->props.num].cmd = cmd;

            if (val[0] == '[') {
                /* format "[len]xx,yy,zz...." */
                int i, len;
                char *r;
                len = strtoul(val + 1, &r, 0);
                if (*r != ']' || len > 32)
                    break;
                s->prop[s->props.num].u.buffer.len = len;
                for (i = 0; i < len; i++) {
                    int d;
                    ++ r;
                    if (*r == '\0')
                        break;
                    d = strtoul(r, &r, 0);
                    if ((*r != ',' && *r != '\0') || d > 255)
                        break;
                    s->prop[s->props.num].u.buffer.data[i] = d;
                }
                if (i < len)
                    break;
            } else if (av_isdigit(val[0]))
                s->prop[s->props.num].u.data = strtoul(val, NULL, 0);
            else
                s->prop[s->props.num].u.data = s2api_id(val);

            if (cmd == DTV_FREQUENCY)
                has_freq = 1;
            else if (cmd == DTV_TUNE)
                has_tune = 1;

            s->props.num ++;
            if (s->props.num >= DTV_IOCTL_MAX_MSGS)
                break;
        }

        if (!p && !has_tune && s->props.num < DTV_IOCTL_MAX_MSGS) {
            s->prop[s->props.num].cmd = DTV_TUNE;
            s->prop[s->props.num].u.data = 1;
            s->props.num ++;
            has_tune = 1;
        }
        if (p || !has_freq || !has_tune)
            s->props.num = 0;
        break;
    }

    fclose(f);
    if (s->props.num == 0) {
        av_log(s, AV_LOG_ERROR,
               "%s: Found the channel def of \"%s\", but in wrong format.\n",
               __func__, chname);
        return AVERROR_INVALIDDATA;
    }

    {
        int i;
        for (i = 0; i < s->props.num; i++)
            av_log(s, AV_LOG_DEBUG, "%s: prop[%d] cmd:%d data:0x%08x\n",
                   __func__, i, s->prop[i].cmd, s->prop[i].u.data);
    }
    return 0;
}

#define GET_UINT(dst, src) \
    do {\
        errno = 0;\
        (dst) = strtoul((src), NULL, 10);\
        if (errno)\
            goto invalid_url;\
    } while (0)\

/**
 * url syntax: dvb://[cardno@]channel_name[?option=val...]
 * option: 'adapter=n'        : set the adapter device number (same as cardno)
 *         'frontend=n'       : set the frontend device number
 *         'demux=n'          : set the demux device number
 *         'buffer_size=n'    : set kernel buffer size of the demux device
 *         'channel_file=PATH': set channel configuration file path
 *
 * default:
 *         'adapter', 'frontend' := 0
 *         'demux'               := same as 'frontend'
 *         'buffer_size'         := kernel default
 *         'channel_file'        := $XDG_DATA_{HOME,DIRS}/ffmpeg/channels.conf
 */
static int dvb_open(URLContext *h, const char *uri, int flags)
{
    DVBContext *s = h->priv_data;
    char auth[16], name[128], opts[256], devname[64];
    char *op, *p;
    const char *chfile;
    struct dmx_pes_filter_params filter;
    int ret;

    memset(auth, 0, sizeof(auth));
    memset(name, 0, sizeof(auth));
    memset(opts, 0, sizeof(auth));
    av_url_split(NULL, 0, auth, sizeof(auth), name, sizeof(name), NULL,
                 opts, sizeof(opts), uri);
    if (!name[0])
        goto invalid_url;

    if (auth[0])
        GET_UINT(s->adap_no, auth);

    if (opts[0] && opts[0] != '?')
        goto invalid_url;

    chfile = s->conf_file_path;
    op = av_strtok(opts + 1, "&", &p);
    while (op) {
        char *val;

        val = strchr(op, '=');
        if (!val)
            goto invalid_url;
        *val++ = '\0';

        if (!strcmp(op, "adapter"))
            GET_UINT(s->adap_no, val);
        else if (!strcmp(op, "frontend"))
            GET_UINT(s->fe_no, val);
        else if (!strcmp(op, "demux"))
            GET_UINT(s->demux_no, val);
        else if (!strcmp(op, "buffer_size"))
            GET_UINT(s->buf_sz, val);
        else if (!strcmp(op, "channel_file"))
            chfile = val;
        else
            goto invalid_url;
        op = av_strtok(NULL, "&", &p);
    }
    if (s->demux_no == -1)
        s->demux_no = s->fe_no;

    if (!chfile || !*chfile)
        chfile = getenv("FFMPEG_DVB_CH_FILE");
    if (!chfile || !*chfile)
        chfile = "channels.conf";
    ret = search_channel(s, name, chfile);
    if (ret)
        return ret;

    ret = snprintf(devname, sizeof(devname),
                   "/dev/dvb/adapter%d/frontend%d", s->adap_no, s->fe_no);
    if (ret < 0 || ret >= sizeof(devname))
        goto cleanup_devs;
    s->fe_fd = open(devname, O_RDWR);
    if (s->fe_fd < 0)
        goto cleanup_devs;
    ret = ioctl(s->fe_fd, FE_SET_PROPERTY, &s->props);
    if (ret == -1)
        goto cleanup_devs;
    ret = check_fe_status(s->fe_fd);
    if (ret)
        goto cleanup_devs;
    av_log(h, AV_LOG_DEBUG, "locked to %s.\n", name);

    ret = snprintf(devname, sizeof(devname),
                   "/dev/dvb/adapter%d/demux%d", s->adap_no, s->demux_no);
    if (ret < 0 || ret >= sizeof(devname))
        goto cleanup_devs;
    s->demux_fd = open(devname, O_RDWR);
    if (s->demux_fd < 0)
        goto cleanup_devs;

    ret = snprintf(devname, sizeof(devname),
                   "/dev/dvb/adapter%d/dvr0", s->adap_no);
    if (ret < 0 || ret >= sizeof(devname))
        goto cleanup_devs;
    s->dvr_fd = open(devname, O_RDONLY | O_NONBLOCK);
    if (s->dvr_fd < 0)
        goto cleanup_devs;

    filter.pid = 0x2000;
    filter.input = DMX_IN_FRONTEND;
    filter.output = DMX_OUT_TS_TAP;
    filter.pes_type = DMX_PES_OTHER;
    filter.flags = DMX_IMMEDIATE_START;
    ret = ioctl(s->demux_fd, DMX_SET_PES_FILTER, &filter);
    if (ret < 0)
        goto cleanup_devs;

    if (s->buf_sz > 0) {
        ret = ioctl(s->demux_fd, DMX_SET_BUFFER_SIZE, s->buf_sz);
        if (ret < 0)
            av_log(h, AV_LOG_WARNING,
                   "dvb_open: failed to set buffer size.(%d)\n", ret);
    }

    h->is_streamed = 1;
    return 0;

invalid_url:
    av_log(h, AV_LOG_ERROR, "dvb_open: Invalid URL/options.\n");
    return AVERROR(EINVAL);

cleanup_devs:
    if (errno == 0)
      errno = EINVAL;
    ret = errno;
    if (s->demux_fd >= 0) {
        ioctl(s->demux_fd, DMX_STOP);
        close(s->demux_fd);
    }
    if (s->dvr_fd >= 0)
        close(s->dvr_fd);
    if (s->fe_fd >= 0)
        close(s->fe_fd);
    av_log(h, AV_LOG_ERROR, "dvb_open: Failed to open the DVB devices.\n");
    return AVERROR(ret);
}

static uint8_t dummy_buf[4196];

static int dvb_read(URLContext *h, uint8_t *buf, int len)
{
    DVBContext *s = h->priv_data;

    int ret;
    struct pollfd pfd;
    int timeout;

    timeout = (h->flags & AVIO_FLAG_NONBLOCK) ? 0 : 100;
    pfd.fd = s->dvr_fd;
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, timeout);
    if (ret == 0 || (ret < 0 && errno == EAGAIN))
        return AVERROR(EAGAIN);
    if (ret < 0 && errno == EINTR)
        return AVERROR(EINTR);
    if (ret < 0 || !(pfd.revents & POLLIN))
        return -1;

    ret = read(s->dvr_fd, buf, len);
    if (ret < 0 && errno == EOVERFLOW) {
        while (read(s->dvr_fd, dummy_buf, sizeof(dummy_buf)) > 0);
        av_log(h, AV_LOG_WARNING,
               "Too sluggish reads. Kernel buffer overflowed.\n");
        ret = 0;
    }
    if (ret < 0)
        av_log(h, AV_LOG_ERROR, "dvb_read failed. %s\n", strerror(errno));
    return ret;
}

static int dvb_close(URLContext *h)
{
    DVBContext *s = h->priv_data;

    ioctl(s->demux_fd, DMX_STOP);
    close(s->demux_fd);
    close(s->dvr_fd);
    close(s->fe_fd);

    return 0;
}


static const AVClass dvb_context_class = {
    .class_name = "dvb",
    .item_name  = av_default_item_name,
    .option     = dvb_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const URLProtocol ff_dvb_protocol = {
    .name                      = "dvb",
    .url_open                  = dvb_open,
    .url_read                  = dvb_read,
    .url_close                 = dvb_close,
    .priv_data_size            = sizeof(DVBContext),
    .priv_data_class           = &dvb_context_class,
};
