/**
 * \file pcm.c
 * \author Jaroslav Kysela <perex@suse.cz>
 * \author Abramo Bagnara <abramo@alsa-project.org>
 * \date 2000-2001
 *
 * PCM Interface is designed to write or read digital audio frames. A
 * frame is the data unit converted into/from sound in one time unit
 * (1/rate seconds), by example if you set your playback PCM rate to
 * 44100 you'll hear 44100 frames per second. The size in bytes of a
 * frame may be obtained from bits needed to store a sample and
 * channels count.
 */
/*
 *  PCM Interface - main file
 *  Copyright (c) 1998 by Jaroslav Kysela <perex@suse.cz>
 *  Copyright (c) 2000 by Abramo Bagnara <abramo@alsa-project.org>
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
  
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <limits.h>
#include <dlfcn.h>
#include "pcm_local.h"
#include "list.h"

/**
 * \brief get identifier of PCM handle
 * \param pcm a PCM handle
 * \return ascii identifier of PCM handle
 *
 * Returns the ASCII identifier of given PCM handle. It's the same
 * identifier as for snd_pcm_open().
 */
const char *snd_pcm_name(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->name;
}

/**
 * \brief get type of PCM handle
 * \param pcm a PCM handle
 * \return type of PCM handle
 *
 * Returns the type #snd_pcm_type_t of given PCM handle.
 */
snd_pcm_type_t snd_pcm_type(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->type;
}

/**
 * \brief get stream for a PCM handle
 * \param pcm a PCM handle
 * \return stream of PCM handle
 *
 * Returns the type #snd_pcm_stream_t of given PCM handle.
 */
snd_pcm_stream_t snd_pcm_stream(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->stream;
}

/**
 * \brief close PCM handle
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 *
 * Closes the specified PCM handle and frees all associated
 * resources.
 */
int snd_pcm_close(snd_pcm_t *pcm)
{
	int ret = 0;
	int err;
	assert(pcm);
	if (pcm->setup) {
		if (pcm->mode & SND_PCM_NONBLOCK || 
		    pcm->stream == SND_PCM_STREAM_CAPTURE)
			snd_pcm_drop(pcm);
		else
			snd_pcm_drain(pcm);
		err = snd_pcm_hw_free(pcm);
		if (err < 0)
			ret = err;
	}
	if ((err = pcm->ops->close(pcm->op_arg)) < 0)
		ret = err;
	pcm->setup = 0;
	if (pcm->name)
		free(pcm->name);
	free(pcm);
	return 0;
}	

/**
 * \brief set nonblock mode
 * \param pcm PCM handle
 * \param nonblock 0 = block, 1 = nonblock mode
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_nonblock(snd_pcm_t *pcm, int nonblock)
{
	int err;
	assert(pcm);
	if ((err = pcm->ops->nonblock(pcm->op_arg, nonblock)) < 0)
		return err;
	if (nonblock)
		pcm->mode |= SND_PCM_NONBLOCK;
	else
		pcm->mode &= ~SND_PCM_NONBLOCK;
	return 0;
}

/**
 * \brief set async mode
 * \param pcm PCM handle
 * \param sig Signal to raise: < 0 disable, 0 default (SIGIO)
 * \param pid Process ID to signal: 0 current
 * \return zero on success otherwise a negative error code
 *
 * A signal is raised every period.
 */
int snd_pcm_async(snd_pcm_t *pcm, int sig, pid_t pid)
{
	int err;
	assert(pcm);
	err = pcm->ops->async(pcm->op_arg, sig, pid);
	if (err < 0)
		return err;
	if (sig)
		pcm->async_sig = sig;
	else
		pcm->async_sig = SIGIO;
	if (pid)
		pcm->async_pid = pid;
	else
		pcm->async_pid = getpid();
	return 0;
}

/**
 * \brief Obtain general (static) information for PCM handle
 * \param pcm PCM handle
 * \param info Information container
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_info(snd_pcm_t *pcm, snd_pcm_info_t *info)
{
	assert(pcm && info);
	return pcm->ops->info(pcm->op_arg, info);
}

/** \brief Install one PCM hardware configuration choosen from a configuration space and #snd_pcm_prepare it
 * \param pcm PCM handle
 * \param params Configuration space definition container
 * \return zero on success otherwise a negative error code
 *
 * The configuration is choosen fixing single parameters in this order:
 * first access, first format, first subformat, min channels, min rate, 
 * min period time, max buffer size, min tick time
 */
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
	int err;
	assert(pcm && params);
	err = _snd_pcm_hw_params(pcm, params);
	if (err >= 0)
		err = snd_pcm_prepare(pcm);
	return err;
}

/** \brief Remove PCM hardware configuration and free associated resources
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_hw_free(snd_pcm_t *pcm)
{
	int err;
	assert(pcm->setup);
	assert(snd_pcm_state(pcm) <= SND_PCM_STATE_PREPARED);
	if (pcm->mmap_channels) {
		err = snd_pcm_munmap(pcm);
		if (err < 0)
			return err;
	}
	err = pcm->ops->hw_free(pcm->op_arg);
	pcm->setup = 0;
	return err;
}

/** \brief Install PCM software configuration defined by params
 * \param pcm PCM handle
 * \param params Configuration container
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{
	int err;
	err = pcm->ops->sw_params(pcm->op_arg, params);
	if (err < 0)
		return err;
	pcm->start_mode = snd_pcm_sw_params_get_start_mode(params);
	pcm->xrun_mode = snd_pcm_sw_params_get_xrun_mode(params);
	pcm->tstamp_mode = snd_pcm_sw_params_get_tstamp_mode(params);
	pcm->period_step = params->period_step;
	pcm->sleep_min = params->sleep_min;
	pcm->avail_min = params->avail_min;
	pcm->xfer_align = params->xfer_align;
	pcm->silence_threshold = params->silence_threshold;
	pcm->silence_size = params->silence_size;
	pcm->boundary = params->boundary;
	return 0;
}

/**
 * \brief Obtain status (runtime) information for PCM handle
 * \param pcm PCM handle
 * \param status Status container
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_status(snd_pcm_t *pcm, snd_pcm_status_t *status)
{
	assert(pcm && status);
	return pcm->fast_ops->status(pcm->fast_op_arg, status);
}

/**
 * \brief Return PCM state
 * \param pcm PCM handle
 * \return PCM state #snd_pcm_state_t of given PCM handle
 */
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->fast_ops->state(pcm->fast_op_arg);
}

/**
 * \brief Obtain delay in frames for a running PCM handle
 * \param pcm PCM handle
 * \param delayp Returned delay
 * \return zero on success otherwise a negative error code
 *
 * Delay is distance between current application frame position and
 * sound frame position.
 * It's positive and less than buffer size in normal situation,
 * negative on playback underrun and greater than buffer size on
 * capture overrun.
 */
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->delay(pcm->fast_op_arg, delayp);
}

/**
 * \brief Prepare PCM for use
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_prepare(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->prepare(pcm->fast_op_arg);
}

/**
 * \brief Reset PCM position
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 *
 * Reduce PCM delay to 0.
 */
int snd_pcm_reset(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->reset(pcm->fast_op_arg);
}

/**
 * \brief Start a PCM
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_start(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->start(pcm->fast_op_arg);
}

/**
 * \brief Stop a PCM dropping pending frames
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_drop(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->drop(pcm->fast_op_arg);
}

/**
 * \brief Stop a PCM preserving pending frames
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 *
 * For playback wait for all pending frames to be played and then stop
 * the PCM.
 * For capture stop PCM permitting to retrieve residual frames.
 */
int snd_pcm_drain(snd_pcm_t *pcm)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->drain(pcm->fast_op_arg);
}

/**
 * \brief Pause/resume PCM
 * \param pcm PCM handle
 * \param pause 0 = resume, 1 = pause
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_pause(snd_pcm_t *pcm, int enable)
{
	assert(pcm);
	assert(pcm->setup);
	return pcm->fast_ops->pause(pcm->fast_op_arg, enable);
}

/**
 * \brief Move application frame position backward
 * \param pcm PCM handle
 * \param frames wanted displacement in frames
 * \return a positive number for actual displacement otherwise a
 * negative error code
 */
snd_pcm_sframes_t snd_pcm_rewind(snd_pcm_t *pcm, snd_pcm_uframes_t frames)
{
	assert(pcm);
	assert(pcm->setup);
	assert(frames > 0);
	return pcm->fast_ops->rewind(pcm->fast_op_arg, frames);
}

/**
 * \brief Write interleaved frames to a PCM
 * \param pcm PCM handle
 * \param buffer frames containing buffer
 * \param size frames to be written
 * \return a positive number of frames actually written otherwise a
 * negative error code
 */ 
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || buffer);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_INTERLEAVED);
	return _snd_pcm_writei(pcm, buffer, size);
}

/**
 * \brief Write non interleaved frames to a PCM
 * \param pcm PCM handle
 * \param bufs frames containing buffers (one for each channel)
 * \param size frames to be written
 * \return a positive number of frames actually written otherwise a
 * negative error code
 */ 
snd_pcm_sframes_t snd_pcm_writen(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || bufs);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_NONINTERLEAVED);
	return _snd_pcm_writen(pcm, bufs, size);
}

/**
 * \brief Read interleaved frames from a PCM
 * \param pcm PCM handle
 * \param buffer frames containing buffer
 * \param size frames to be written
 * \return a positive number of frames actually read otherwise a
 * negative error code
 */ 
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || buffer);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_INTERLEAVED);
	return _snd_pcm_readi(pcm, buffer, size);
}

/**
 * \brief Read non interleaved frames to a PCM
 * \param pcm PCM handle
 * \param bufs frames containing buffers (one for each channel)
 * \param size frames to be written
 * \return a positive number of frames actually read otherwise a
 * negative error code
 */ 
snd_pcm_sframes_t snd_pcm_readn(snd_pcm_t *pcm, void **bufs, snd_pcm_uframes_t size)
{
	assert(pcm);
	assert(size == 0 || bufs);
	assert(pcm->setup);
	assert(pcm->access == SND_PCM_ACCESS_RW_NONINTERLEAVED);
	return _snd_pcm_readn(pcm, bufs, size);
}

/**
 * \brief Link two PCMs
 * \param pcm1 first PCM handle
 * \param pcm2 first PCM handle
 * \return zero on success otherwise a negative error code
 *
 * The two PCMs will start/stop/prepare in sync.
 */ 
int snd_pcm_link(snd_pcm_t *pcm1, snd_pcm_t *pcm2)
{
	int fd1 = _snd_pcm_link_descriptor(pcm1);
	int fd2 = _snd_pcm_link_descriptor(pcm2);
	if (fd1 < 0 || fd2 < 0)
		return -ENOSYS;
	if (ioctl(fd1, SNDRV_PCM_IOCTL_LINK, fd2) < 0) {
		SYSERR("SNDRV_PCM_IOCTL_LINK failed");
		return -errno;
	}
	return 0;
}

/**
 * \brief Remove a PCM from a linked group
 * \param pcm PCM handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_unlink(snd_pcm_t *pcm)
{
	int fd;
	fd = _snd_pcm_link_descriptor(pcm);
	if (ioctl(fd, SNDRV_PCM_IOCTL_UNLINK) < 0) {
		SYSERR("SNDRV_PCM_IOCTL_UNLINK failed");
		return -errno;
	}
	return 0;
}

/**
 * \brief get count of poll descriptors for PCM handle
 * \param pcm PCM handle
 * \return count of poll descriptors
 */
int snd_pcm_poll_descriptors_count(snd_pcm_t *pcm)
{
	assert(pcm);
	return 1;
}


/**
 * \brief get poll descriptors
 * \param pcm PCM handle
 * \param pfds array of poll descriptors
 * \param space space in the poll descriptor array
 * \return count of filled descriptors
 */
int snd_pcm_poll_descriptors(snd_pcm_t *pcm, struct pollfd *pfds, unsigned int space)
{
	assert(pcm);
	if (space >= 1) {
		pfds->fd = pcm->poll_fd;
		pfds->events = pcm->stream == SND_PCM_STREAM_PLAYBACK ? POLLOUT : POLLIN;
	}
	return 1;
}

#ifndef DOC_HIDDEN
#define STATE(v) [SND_PCM_STATE_##v] = #v
#define STREAM(v) [SND_PCM_STREAM_##v] = #v
#define READY(v) [SND_PCM_READY_##v] = #v
#define XRUN(v) [SND_PCM_XRUN_##v] = #v
#define SILENCE(v) [SND_PCM_SILENCE_##v] = #v
#define TSTAMP(v) [SND_PCM_TSTAMP_##v] = #v
#define ACCESS(v) [SND_PCM_ACCESS_##v] = #v
#define START(v) [SND_PCM_START_##v] = #v
#define HW_PARAM(v) [SND_PCM_HW_PARAM_##v] = #v
#define SW_PARAM(v) [SND_PCM_SW_PARAM_##v] = #v
#define FORMAT(v) [SND_PCM_FORMAT_##v] = #v
#define SUBFORMAT(v) [SND_PCM_SUBFORMAT_##v] = #v 

#define FORMATD(v, d) [SND_PCM_FORMAT_##v] = d
#define SUBFORMATD(v, d) [SND_PCM_SUBFORMAT_##v] = d 

static const char *snd_pcm_stream_names[] = {
	STREAM(PLAYBACK),
	STREAM(CAPTURE),
};

static const char *snd_pcm_state_names[] = {
	STATE(OPEN),
	STATE(SETUP),
	STATE(PREPARED),
	STATE(RUNNING),
	STATE(XRUN),
	STATE(PAUSED),
};

static const char *snd_pcm_access_names[] = {
	ACCESS(MMAP_INTERLEAVED), 
	ACCESS(MMAP_NONINTERLEAVED),
	ACCESS(MMAP_COMPLEX),
	ACCESS(RW_INTERLEAVED),
	ACCESS(RW_NONINTERLEAVED),
};

static const char *snd_pcm_format_names[] = {
	FORMAT(S8),
	FORMAT(U8),
	FORMAT(S16_LE),
	FORMAT(S16_BE),
	FORMAT(U16_LE),
	FORMAT(U16_BE),
	FORMAT(S24_LE),
	FORMAT(S24_BE),
	FORMAT(U24_LE),
	FORMAT(U24_BE),
	FORMAT(S32_LE),
	FORMAT(S32_BE),
	FORMAT(U32_LE),
	FORMAT(U32_BE),
	FORMAT(FLOAT_LE),
	FORMAT(FLOAT_BE),
	FORMAT(FLOAT64_LE),
	FORMAT(FLOAT64_BE),
	FORMAT(IEC958_SUBFRAME_LE),
	FORMAT(IEC958_SUBFRAME_BE),
	FORMAT(MU_LAW),
	FORMAT(A_LAW),
	FORMAT(IMA_ADPCM),
	FORMAT(MPEG),
	FORMAT(GSM),
	FORMAT(SPECIAL),
};

static const char *snd_pcm_format_descriptions[] = {
	FORMATD(S8, "Signed 8 bit"), 
	FORMATD(U8, "Unsigned 8 bit"),
	FORMATD(S16_LE, "Signed 16 bit Little Endian"),
	FORMATD(S16_BE, "Signed 16 bit Big Endian"),
	FORMATD(U16_LE, "Unsigned 16 bit Little Endian"),
	FORMATD(U16_BE, "Unsigned 16 bit Big Endian"),
	FORMATD(S24_LE, "Signed 24 bit Little Endian"),
	FORMATD(S24_BE, "Signed 24 bit Big Endian"),
	FORMATD(U24_LE, "Unsigned 24 bit Little Endian"),
	FORMATD(U24_BE, "Unsigned 24 bit Big Endian"),
	FORMATD(S32_LE, "Signed 32 bit Little Endian"),
	FORMATD(S32_BE, "Signed 32 bit Big Endian"),
	FORMATD(U32_LE, "Unsigned 32 bit Little Endian"),
	FORMATD(U32_BE, "Unsigned 32 bit Big Endian"),
	FORMATD(FLOAT_LE, "Float 32 bit Little Endian"),
	FORMATD(FLOAT_BE, "Float 32 bit Big Endian"),
	FORMATD(FLOAT64_LE, "Float 64 bit Little Endian"),
	FORMATD(FLOAT64_BE, "Float 64 bit Big Endian"),
	FORMATD(IEC958_SUBFRAME_LE, "IEC-958 Little Endian"),
	FORMATD(IEC958_SUBFRAME_BE, "IEC-958 Big Endian"),
	FORMATD(MU_LAW, "Mu-Law"),
	FORMATD(A_LAW, "A-Law"),
	FORMATD(IMA_ADPCM, "Ima-ADPCM"),
	FORMATD(MPEG, "MPEG"),
	FORMATD(GSM, "GSM"),
	FORMATD(SPECIAL, "Special"),
};

static const char *snd_pcm_subformat_names[] = {
	SUBFORMAT(STD), 
};

static const char *snd_pcm_subformat_descriptions[] = {
	SUBFORMATD(STD, "Standard"), 
};

static const char *snd_pcm_start_mode_names[] = {
	START(EXPLICIT),
	START(DATA),
};

static const char *snd_pcm_xrun_mode_names[] = {
	XRUN(NONE),
	XRUN(STOP),
};

static const char *snd_pcm_tstamp_mode_names[] = {
	TSTAMP(NONE),
	TSTAMP(MMAP),
};
#endif

/**
 * \brief get name of PCM stream
 * \param stream PCM stream
 * \return ascii name of PCM stream
 */
const char *snd_pcm_stream_name(snd_pcm_stream_t stream)
{
	assert(stream <= SND_PCM_STREAM_LAST);
	return snd_pcm_stream_names[snd_enum_to_int(stream)];
}

/**
 * \brief get name of PCM access type
 * \param access PCM access type
 * \return ascii name of PCM access type
 */
const char *snd_pcm_access_name(snd_pcm_access_t access)
{
	assert(access <= SND_PCM_ACCESS_LAST);
	return snd_pcm_access_names[snd_enum_to_int(access)];
}

/**
 * \brief get name of PCM sample format
 * \param format PCM sample format
 * \return ascii name of PCM sample format
 */
const char *snd_pcm_format_name(snd_pcm_format_t format)
{
	assert(format <= SND_PCM_FORMAT_LAST);
	return snd_pcm_format_names[snd_enum_to_int(format)];
}

/**
 * \brief get description of PCM sample format
 * \param format PCM sample format
 * \return ascii description of PCM sample format
 */
const char *snd_pcm_format_description(snd_pcm_format_t format)
{
	assert(format <= SND_PCM_FORMAT_LAST);
	return snd_pcm_format_descriptions[snd_enum_to_int(format)];
}

/**
 * \brief get PCM sample format from name
 * \param name PCM sample format name (case insensitive)
 * \return PCM sample format
 */
snd_pcm_format_t snd_pcm_format_value(const char* name)
{
	snd_pcm_format_t format;
	for (format = 0; format <= SND_PCM_FORMAT_LAST; snd_enum_incr(format)) {
		if (snd_pcm_format_names[snd_enum_to_int(format)] &&
		    strcasecmp(name, snd_pcm_format_names[snd_enum_to_int(format)]) == 0) {
			return format;
		}
	}
	return SND_PCM_FORMAT_UNKNOWN;
}

/**
 * \brief get name of PCM sample subformat
 * \param format PCM sample subformat
 * \return ascii name of PCM sample subformat
 */
const char *snd_pcm_subformat_name(snd_pcm_subformat_t subformat)
{
	assert(subformat <= SND_PCM_SUBFORMAT_LAST);
	return snd_pcm_subformat_names[snd_enum_to_int(subformat)];
}

/**
 * \brief get description of PCM sample subformat
 * \param subformat PCM sample subformat
 * \return ascii description of PCM sample subformat
 */
const char *snd_pcm_subformat_description(snd_pcm_subformat_t subformat)
{
	assert(subformat <= SND_PCM_SUBFORMAT_LAST);
	return snd_pcm_subformat_descriptions[snd_enum_to_int(subformat)];
}

/**
 * \brief get name of PCM start mode setting
 * \param mode PCM start mode
 * \return ascii name of PCM start mode setting
 */
const char *snd_pcm_start_mode_name(snd_pcm_start_t mode)
{
	assert(mode <= SND_PCM_START_LAST);
	return snd_pcm_start_mode_names[snd_enum_to_int(mode)];
}

/**
 * \brief get name of PCM xrun mode setting
 * \param mode PCM xrun mode
 * \return ascii name of PCM xrun mode setting
 */
const char *snd_pcm_xrun_mode_name(snd_pcm_xrun_t mode)
{
	assert(mode <= SND_PCM_XRUN_LAST);
	return snd_pcm_xrun_mode_names[snd_enum_to_int(mode)];
}

/**
 * \brief get name of PCM tstamp mode setting
 * \param mode PCM tstamp mode
 * \return ascii name of PCM tstamp mode setting
 */
const char *snd_pcm_tstamp_mode_name(snd_pcm_tstamp_t mode)
{
	assert(mode <= SND_PCM_TSTAMP_LAST);
	return snd_pcm_tstamp_mode_names[snd_enum_to_int(mode)];
}

/**
 * \brief get name of PCM state
 * \param state PCM state
 * \return ascii name of PCM state
 */
const char *snd_pcm_state_name(snd_pcm_state_t state)
{
	assert(state <= SND_PCM_STATE_LAST);
	return snd_pcm_state_names[snd_enum_to_int(state)];
}

/**
 * \brief Dump current hardware setup for PCM
 * \param pcm PCM handle
 * \param out Output handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_dump_hw_setup(snd_pcm_t *pcm, snd_output_t *out)
{
	assert(pcm);
	assert(out);
	assert(pcm->setup);
        snd_output_printf(out, "stream       : %s\n", snd_pcm_stream_name(pcm->stream));
	snd_output_printf(out, "access       : %s\n", snd_pcm_access_name(pcm->access));
	snd_output_printf(out, "format       : %s\n", snd_pcm_format_name(pcm->format));
	snd_output_printf(out, "subformat    : %s\n", snd_pcm_subformat_name(pcm->subformat));
	snd_output_printf(out, "channels     : %u\n", pcm->channels);
	snd_output_printf(out, "rate         : %u\n", pcm->rate);
	snd_output_printf(out, "exact rate   : %g (%u/%u)\n", (double) pcm->rate_num / pcm->rate_den, pcm->rate_num, pcm->rate_den);
	snd_output_printf(out, "msbits       : %u\n", pcm->msbits);
	snd_output_printf(out, "buffer_size  : %lu\n", pcm->buffer_size);
	snd_output_printf(out, "period_size  : %lu\n", pcm->period_size);
	snd_output_printf(out, "period_time  : %u\n", pcm->period_time);
	snd_output_printf(out, "tick_time    : %u\n", pcm->tick_time);
	return 0;
}

/**
 * \brief Dump current software setup for PCM
 * \param pcm PCM handle
 * \param out Output handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_dump_sw_setup(snd_pcm_t *pcm, snd_output_t *out)
{
	assert(pcm);
	assert(out);
	assert(pcm->setup);
	snd_output_printf(out, "start_mode   : %s\n", snd_pcm_start_mode_name(pcm->start_mode));
	snd_output_printf(out, "xrun_mode    : %s\n", snd_pcm_xrun_mode_name(pcm->xrun_mode));
	snd_output_printf(out, "tstamp_mode  : %s\n", snd_pcm_tstamp_mode_name(pcm->tstamp_mode));
	snd_output_printf(out, "period_step  : %ld\n", (long)pcm->period_step);
	snd_output_printf(out, "sleep_min    : %ld\n", (long)pcm->sleep_min);
	snd_output_printf(out, "avail_min    : %ld\n", (long)pcm->avail_min);
	snd_output_printf(out, "xfer_align   : %ld\n", (long)pcm->xfer_align);
	snd_output_printf(out, "silence_threshold: %ld\n", (long)pcm->silence_threshold);
	snd_output_printf(out, "silence_size : %ld\n", (long)pcm->silence_size);
	snd_output_printf(out, "boundary     : %ld\n", (long)pcm->boundary);
	return 0;
}

/**
 * \brief Dump current setup (hardware and software) for PCM
 * \param pcm PCM handle
 * \param out Output handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_dump_setup(snd_pcm_t *pcm, snd_output_t *out)
{
	snd_pcm_dump_hw_setup(pcm, out);
	snd_pcm_dump_sw_setup(pcm, out);
	return 0;
}

/**
 * \brief Dump status
 * \param status Status container
 * \param out Output handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_status_dump(snd_pcm_status_t *status, snd_output_t *out)
{
	assert(status);
	snd_output_printf(out, "state       : %s\n", snd_pcm_state_name((snd_pcm_state_t) status->state));
	snd_output_printf(out, "trigger_time: %ld.%06ld\n",
		status->trigger_tstamp.tv_sec, status->trigger_tstamp.tv_usec);
	snd_output_printf(out, "tstamp      : %ld.%06ld\n",
		status->tstamp.tv_sec, status->tstamp.tv_usec);
	snd_output_printf(out, "delay       : %ld\n", (long)status->delay);
	snd_output_printf(out, "avail       : %ld\n", (long)status->avail);
	snd_output_printf(out, "avail_max   : %ld\n", (long)status->avail_max);
	return 0;
}

/**
 * \brief Dump PCM info
 * \param pcm PCM handle
 * \param out Output handle
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_dump(snd_pcm_t *pcm, snd_output_t *out)
{
	assert(pcm);
	assert(out);
	pcm->ops->dump(pcm->op_arg, out);
	return 0;
}

/**
 * \brief Convert bytes in frames for a PCM
 * \param pcm PCM handle
 * \param bytes quantity in bytes
 * \return quantity expressed in frames
 */
snd_pcm_sframes_t snd_pcm_bytes_to_frames(snd_pcm_t *pcm, ssize_t bytes)
{
	assert(pcm);
	assert(pcm->setup);
	return bytes * 8 / pcm->frame_bits;
}

/**
 * \brief Convert frames in bytes for a PCM
 * \param pcm PCM handle
 * \param frames quantity in frames
 * \return quantity expressed in bytes
 */
ssize_t snd_pcm_frames_to_bytes(snd_pcm_t *pcm, snd_pcm_sframes_t frames)
{
	assert(pcm);
	assert(pcm->setup);
	return frames * pcm->frame_bits / 8;
}

/**
 * \brief Convert bytes in samples for a PCM
 * \param pcm PCM handle
 * \param bytes quantity in bytes
 * \return quantity expressed in samples
 */
int snd_pcm_bytes_to_samples(snd_pcm_t *pcm, ssize_t bytes)
{
	assert(pcm);
	assert(pcm->setup);
	return bytes * 8 / pcm->sample_bits;
}

/**
 * \brief Convert samples in bytes for a PCM
 * \param pcm PCM handle
 * \param samples quantity in samples
 * \return quantity expressed in bytes
 */
ssize_t snd_pcm_samples_to_bytes(snd_pcm_t *pcm, int samples)
{
	assert(pcm);
	assert(pcm->setup);
	return samples * pcm->sample_bits / 8;
}

/**
 * \brief Opens a PCM
 * \param pcmp Returned PCM handle
 * \param name ASCII identifier of the PCM handle
 * \param stream Wanted stream
 * \param mode Open mode (see #SND_PCM_NONBLOCK, #SND_PCM_ASYNC)
 * \return a negative error code on failure or zero on success
 */
int snd_pcm_open(snd_pcm_t **pcmp, const char *name, 
		 snd_pcm_stream_t stream, int mode)
{
	const char *str;
	char buf[256];
	int err;
	snd_config_t *pcm_conf, *conf, *type_conf = NULL;
	snd_config_iterator_t i, next;
	const char *lib = NULL, *open = NULL;
	int (*open_func)(snd_pcm_t **pcmp, const char *name, snd_config_t *conf, 
			 snd_pcm_stream_t stream, int mode);
	void *h;
	const char *name1;
	assert(pcmp && name);
	err = snd_config_update();
	if (err < 0)
		return err;

	err = snd_config_search_alias(snd_config, "pcm", name, &pcm_conf);
	name1 = name;
	if (err < 0 || snd_config_get_string(pcm_conf, &name1) >= 0) {
		int card, dev, subdev;
		char socket[256], sname[256];
		char format[16], file[256];
		err = sscanf(name1, "hw:%d,%d,%d", &card, &dev, &subdev);
		if (err == 3)
			return snd_pcm_hw_open(pcmp, name, card, dev, subdev, stream, mode);
		err = sscanf(name1, "hw:%d,%d", &card, &dev);
		if (err == 2)
			return snd_pcm_hw_open(pcmp, name, card, dev, -1, stream, mode);
		err = sscanf(name1, "plug:%d,%d,%d", &card, &dev, &subdev);
		if (err == 3)
			return snd_pcm_plug_open_hw(pcmp, name, card, dev, subdev, stream, mode);
		err = sscanf(name1, "plug:%d,%d", &card, &dev);
		if (err == 2)
			return snd_pcm_plug_open_hw(pcmp, name, card, dev, -1, stream, mode);
		err = sscanf(name1, "plug:%256[^,]", sname);
		if (err == 1) {
			snd_pcm_t *slave;
			err = snd_pcm_open(&slave, sname, stream, mode);
			if (err < 0)
				return err;
			return snd_pcm_plug_open(pcmp, name, NULL, 0, 0, 0, slave, 1);
		}
		err = sscanf(name1, "shm:%256[^,],%256[^,]", socket, sname);
		if (err == 2)
			return snd_pcm_shm_open(pcmp, name, socket, sname, stream, mode);
		err = sscanf(name1, "file:%256[^,],%16[^,],%256[^,]", file, format, sname);
		if (err == 3) {
			snd_pcm_t *slave;
			err = snd_pcm_open(&slave, sname, stream, mode);
			if (err < 0)
				return err;
			return snd_pcm_file_open(pcmp, name1, file, -1, format, slave, 1);
		}
		err = sscanf(name1, "file:%256[^,],%16[^,]", file, format);
		if (err == 2) {
			snd_pcm_t *slave;
			err = snd_pcm_null_open(&slave, name, stream, mode);
			if (err < 0)
				return err;
			return snd_pcm_file_open(pcmp, name, file, -1, format, slave, 1);
		}
		err = sscanf(name1, "file:%256[^,]", file);
		if (err == 1) {
			snd_pcm_t *slave;
			err = snd_pcm_null_open(&slave, name, stream, mode);
			if (err < 0)
				return err;
			return snd_pcm_file_open(pcmp, name, file, -1, "raw", slave, 1);
		}
		if (strcmp(name1, "null") == 0)
			return snd_pcm_null_open(pcmp, name, stream, mode);
		SNDERR("Unknown PCM %s", name1);
		return -ENOENT;
	}
	if (snd_config_get_type(pcm_conf) != SND_CONFIG_TYPE_COMPOUND) {
		SNDERR("Invalid type for PCM %s definition", name1);
		return -EINVAL;
	}
	err = snd_config_search(pcm_conf, "type", &conf);
	if (err < 0) {
		SNDERR("type is not defined");
		return err;
	}
	err = snd_config_get_string(conf, &str);
	if (err < 0) {
		SNDERR("Invalid type for %s", snd_config_get_id(conf));
		return err;
	}
	err = snd_config_search_alias(snd_config, "pcm_type", str, &type_conf);
	if (err >= 0) {
		if (snd_config_get_type(type_conf) != SND_CONFIG_TYPE_COMPOUND) {
			SNDERR("Invalid type for PCM type %s definition", str);
			return -EINVAL;
		}
		snd_config_for_each(i, next, type_conf) {
			snd_config_t *n = snd_config_iterator_entry(i);
			const char *id = snd_config_get_id(n);
			if (strcmp(id, "comment") == 0)
				continue;
			if (strcmp(id, "lib") == 0) {
				err = snd_config_get_string(n, &lib);
				if (err < 0) {
					SNDERR("Invalid type for %s", id);
					return -EINVAL;
				}
				continue;
			}
			if (strcmp(id, "open") == 0) {
				err = snd_config_get_string(n, &open);
				if (err < 0) {
					SNDERR("Invalid type for %s", id);
					return -EINVAL;
				}
				continue;
			}
			SNDERR("Unknown field %s", id);
			return -EINVAL;
		}
	}
	if (!open) {
		open = buf;
		snprintf(buf, sizeof(buf), "_snd_pcm_%s_open", str);
	}
	if (!lib)
		lib = "libasound.so";
	h = dlopen(lib, RTLD_NOW);
	if (!h) {
		SNDERR("Cannot open shared library %s", lib);
		return -ENOENT;
	}
	open_func = dlsym(h, open);
	if (!open_func) {
		SNDERR("symbol %s is not defined inside %s", open, lib);
		dlclose(h);
		return -ENXIO;
	}
	return open_func(pcmp, name, pcm_conf, stream, mode);
}

/**
 * \brief Wait for a PCM to become ready
 * \param pcm a PCM handle
 * \param timeout maximum time in milliseconds to wait
 * \return a negative error code on failure or zero on success
 */
int snd_pcm_wait(snd_pcm_t *pcm, int timeout)
{
	struct pollfd pfd;
	int err;
	err = snd_pcm_poll_descriptors(pcm, &pfd, 1);
	assert(err == 1);
	err = poll(&pfd, 1, timeout);
	if (err < 0)
		return -errno;
	return 0;
}

/**
 * \brief Return number of frames ready to be read/written
 * \param pcm a PCM handle
 * \return a positive number of frames ready otherwise a negative
 * error code
 *
 * On capture does all the actions needed to transport to application
 * level all the ready frames across underlying layers.
 */
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm)
{
	return pcm->fast_ops->avail_update(pcm->fast_op_arg);
}

/**
 * \brief Advance PCM frame position in mmap buffer
 * \param pcm a PCM handle
 * \param size movement size
 * \return a positive number of actual movement size otherwise a negative
 * error code
 *
 * On playback does all the actions needed to transport the frames across
 * underlying layers. 
 */
snd_pcm_sframes_t snd_pcm_mmap_forward(snd_pcm_t *pcm, snd_pcm_uframes_t size)
{
	assert(size > 0);
	assert(size <= snd_pcm_mmap_avail(pcm));
	return pcm->fast_ops->mmap_forward(pcm->fast_op_arg, size);
}

/**
 * \brief Silence an area
 * \param dst_area area specification
 * \param dst_offset offset in frames inside area
 * \param samples samples to silence
 * \param format PCM sample format
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_area_silence(const snd_pcm_channel_area_t *dst_area, snd_pcm_uframes_t dst_offset,
			 unsigned int samples, snd_pcm_format_t format)
{
	/* FIXME: sub byte resolution and odd dst_offset */
	char *dst;
	unsigned int dst_step;
	int width;
	u_int64_t silence;
	if (!dst_area->addr)
		return 0;
	dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
	width = snd_pcm_format_physical_width(format);
	silence = snd_pcm_format_silence_64(format);
	if (dst_area->step == (unsigned int) width) {
		unsigned int dwords = samples * width / 64;
		samples -= dwords * 64 / width;
		while (dwords-- > 0)
			*((u_int64_t*)dst)++ = silence;
		if (samples == 0)
			return 0;
	}
	dst_step = dst_area->step / 8;
	switch (width) {
	case 4: {
		u_int8_t s0 = silence & 0xf0;
		u_int8_t s1 = silence & 0x0f;
		int dstbit = dst_area->first % 8;
		int dstbit_step = dst_area->step % 8;
		while (samples-- > 0) {
			if (dstbit) {
				*dst &= 0xf0;
				*dst |= s1;
			} else {
				*dst &= 0x0f;
				*dst |= s0;
			}
			dst += dst_step;
			dstbit += dstbit_step;
			if (dstbit == 8) {
				dst++;
				dstbit = 0;
			}
		}
		break;
	}
	case 8: {
		u_int8_t sil = silence;
		while (samples-- > 0) {
			*dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 16: {
		u_int16_t sil = silence;
		while (samples-- > 0) {
			*(u_int16_t*)dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 32: {
		u_int32_t sil = silence;
		while (samples-- > 0) {
			*(u_int32_t*)dst = sil;
			dst += dst_step;
		}
		break;
	}
	case 64: {
		while (samples-- > 0) {
			*(u_int64_t*)dst = silence;
			dst += dst_step;
		}
		break;
	}
	default:
		assert(0);
	}
	return 0;
}

/**
 * \brief Silence one or more areas
 * \param dst_areas areas specification (one for each channel)
 * \param dst_offset offset in frames inside area
 * \param channels channels count
 * \param frames frames to silence
 * \param format PCM sample format
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_areas_silence(const snd_pcm_channel_area_t *dst_areas, snd_pcm_uframes_t dst_offset,
			  unsigned int channels, snd_pcm_uframes_t frames, snd_pcm_format_t format)
{
	int width = snd_pcm_format_physical_width(format);
	while (channels > 0) {
		void *addr = dst_areas->addr;
		unsigned int step = dst_areas->step;
		const snd_pcm_channel_area_t *begin = dst_areas;
		int channels1 = channels;
		unsigned int chns = 0;
		int err;
		while (1) {
			channels1--;
			chns++;
			dst_areas++;
			if (channels1 == 0 ||
			    dst_areas->addr != addr ||
			    dst_areas->step != step ||
			    dst_areas->first != dst_areas[-1].first + width)
				break;
		}
		if (chns > 1 && chns * width == step) {
			/* Collapse the areas */
			snd_pcm_channel_area_t d;
			d.addr = begin->addr;
			d.first = begin->first;
			d.step = width;
			err = snd_pcm_area_silence(&d, dst_offset * chns, frames * chns, format);
			channels -= chns;
		} else {
			err = snd_pcm_area_silence(begin, dst_offset, frames, format);
			dst_areas = begin + 1;
			channels--;
		}
		if (err < 0)
			return err;
	}
	return 0;
}


/**
 * \brief Copy an area
 * \param dst_area destination area specification
 * \param dst_offset offset in frames inside destination area
 * \param src_area source area specification
 * \param src_offset offset in frames inside source area
 * \param samples samples to copy
 * \param format PCM sample format
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_area_copy(const snd_pcm_channel_area_t *dst_area, snd_pcm_uframes_t dst_offset,
		      const snd_pcm_channel_area_t *src_area, snd_pcm_uframes_t src_offset,
		      unsigned int samples, snd_pcm_format_t format)
{
	/* FIXME: sub byte resolution and odd dst_offset */
	const char *src;
	char *dst;
	int width;
	int src_step, dst_step;
	if (!src_area->addr)
		return snd_pcm_area_silence(dst_area, dst_offset, samples, format);
	src = snd_pcm_channel_area_addr(src_area, src_offset);
	if (!dst_area->addr)
		return 0;
	dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
	width = snd_pcm_format_physical_width(format);
	if (src_area->step == (unsigned int) width &&
	    dst_area->step == (unsigned int) width) {
		size_t bytes = samples * width / 8;
		samples -= bytes * 8 / width;
		memcpy(dst, src, bytes);
		if (samples == 0)
			return 0;
	}
	src_step = src_area->step / 8;
	dst_step = dst_area->step / 8;
	switch (width) {
	case 4: {
		int srcbit = src_area->first % 8;
		int srcbit_step = src_area->step % 8;
		int dstbit = dst_area->first % 8;
		int dstbit_step = dst_area->step % 8;
		while (samples-- > 0) {
			unsigned char srcval;
			if (srcbit)
				srcval = *src & 0x0f;
			else
				srcval = *src & 0xf0;
			if (dstbit)
				*dst &= 0xf0;
			else
				*dst &= 0x0f;
			*dst |= srcval;
			src += src_step;
			srcbit += srcbit_step;
			if (srcbit == 8) {
				src++;
				srcbit = 0;
			}
			dst += dst_step;
			dstbit += dstbit_step;
			if (dstbit == 8) {
				dst++;
				dstbit = 0;
			}
		}
		break;
	}
	case 8: {
		while (samples-- > 0) {
			*dst = *src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 16: {
		while (samples-- > 0) {
			*(u_int16_t*)dst = *(u_int16_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 32: {
		while (samples-- > 0) {
			*(u_int32_t*)dst = *(u_int32_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	case 64: {
		while (samples-- > 0) {
			*(u_int64_t*)dst = *(u_int64_t*)src;
			src += src_step;
			dst += dst_step;
		}
		break;
	}
	default:
		assert(0);
	}
	return 0;
}

/**
 * \brief Copy one or more areas
 * \param dst_areas destination areas specification (one for each channel)
 * \param dst_offset offset in frames inside destination area
 * \param src_areas source areas specification (one for each channel)
 * \param src_offset offset in frames inside source area
 * \param channels channels count
 * \param frames frames to copy
 * \param format PCM sample format
 * \return zero on success otherwise a negative error code
 */
int snd_pcm_areas_copy(const snd_pcm_channel_area_t *dst_areas, snd_pcm_uframes_t dst_offset,
		       const snd_pcm_channel_area_t *src_areas, snd_pcm_uframes_t src_offset,
		       unsigned int channels, snd_pcm_uframes_t frames, snd_pcm_format_t format)
{
	int width = snd_pcm_format_physical_width(format);
	while (channels > 0) {
		unsigned int step = src_areas->step;
		void *src_addr = src_areas->addr;
		const snd_pcm_channel_area_t *src_start = src_areas;
		void *dst_addr = dst_areas->addr;
		const snd_pcm_channel_area_t *dst_start = dst_areas;
		int channels1 = channels;
		unsigned int chns = 0;
		while (dst_areas->step == step) {
			channels1--;
			chns++;
			src_areas++;
			dst_areas++;
			if (channels1 == 0 ||
			    src_areas->step != step ||
			    src_areas->addr != src_addr ||
			    dst_areas->addr != dst_addr ||
			    src_areas->first != src_areas[-1].first + width ||
			    dst_areas->first != dst_areas[-1].first + width)
				break;
		}
		if (chns > 1 && chns * width == step) {
			/* Collapse the areas */
			snd_pcm_channel_area_t s, d;
			s.addr = src_start->addr;
			s.first = src_start->first;
			s.step = width;
			d.addr = dst_start->addr;
			d.first = dst_start->first;
			d.step = width;
			snd_pcm_area_copy(&d, dst_offset * chns,
					  &s, src_offset * chns, 
					  frames * chns, format);
			channels -= chns;
		} else {
			snd_pcm_area_copy(dst_start, dst_offset,
					  src_start, src_offset,
					  frames, format);
			src_areas = src_start + 1;
			dst_areas = dst_start + 1;
			channels--;
		}
	}
	return 0;
}

#ifndef DOC_HIDDEN

int _snd_pcm_poll_descriptor(snd_pcm_t *pcm)
{
	assert(pcm);
	return pcm->poll_fd;
}

void snd_pcm_areas_from_buf(snd_pcm_t *pcm, snd_pcm_channel_area_t *areas, 
			    void *buf)
{
	unsigned int channel;
	unsigned int channels = pcm->channels;
	for (channel = 0; channel < channels; ++channel, ++areas) {
		areas->addr = buf;
		areas->first = channel * pcm->sample_bits;
		areas->step = pcm->frame_bits;
	}
}

void snd_pcm_areas_from_bufs(snd_pcm_t *pcm, snd_pcm_channel_area_t *areas, 
			     void **bufs)
{
	unsigned int channel;
	unsigned int channels = pcm->channels;
	for (channel = 0; channel < channels; ++channel, ++areas, ++bufs) {
		areas->addr = *bufs;
		areas->first = 0;
		areas->step = pcm->sample_bits;
	}
}

snd_pcm_sframes_t snd_pcm_read_areas(snd_pcm_t *pcm, const snd_pcm_channel_area_t *areas,
				     snd_pcm_uframes_t offset, snd_pcm_uframes_t size,
				     snd_pcm_xfer_areas_func_t func)
{
	snd_pcm_uframes_t xfer = 0;
	int err = 0;
	snd_pcm_state_t state = snd_pcm_state(pcm);

	if (size == 0)
		return 0;
	if (size > pcm->xfer_align)
		size -= size % pcm->xfer_align;

	switch (snd_enum_to_int(state)) {
	case SND_PCM_STATE_PREPARED:
		if (pcm->start_mode == SND_PCM_START_DATA) {
			err = snd_pcm_start(pcm);
			if (err < 0)
				goto _end;
		}
		break;
	case SND_PCM_STATE_DRAINING:
	case SND_PCM_STATE_RUNNING:
		break;
	case SND_PCM_STATE_XRUN:
		return -EPIPE;
	default:
		return -EBADFD;
	}

	while (size > 0) {
		snd_pcm_uframes_t frames;
		snd_pcm_sframes_t avail;
	_again:
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0) {
			err = -EPIPE;
			goto _end;
		}
		if (state == SND_PCM_STATE_DRAINING) {
			if (avail == 0) {
				err = -EPIPE;
				goto _end;
			}
		} else if (avail == 0 ||
			   (size >= pcm->xfer_align && 
			    (snd_pcm_uframes_t) avail < pcm->xfer_align)) {
			if (pcm->mode & SND_PCM_NONBLOCK) {
				err = -EAGAIN;
				goto _end;
			}

			err = snd_pcm_wait(pcm, -1);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
			goto _again;
			
		}
		if ((snd_pcm_uframes_t) avail > pcm->xfer_align)
			avail -= avail % pcm->xfer_align;
		frames = size;
		if (frames > (snd_pcm_uframes_t) avail)
			frames = avail;
		assert(frames != 0);
		err = func(pcm, areas, offset, frames);
		if (err < 0)
			break;
		assert((snd_pcm_uframes_t)err == frames);
		offset += frames;
		size -= frames;
		xfer += frames;
#if 0
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_XRUN) {
			err = -EPIPE;
			goto _end;
		}
#endif
	}
 _end:
	return xfer > 0 ? xfer : err;
}

snd_pcm_sframes_t snd_pcm_write_areas(snd_pcm_t *pcm, const snd_pcm_channel_area_t *areas,
				      snd_pcm_uframes_t offset, snd_pcm_uframes_t size,
				      snd_pcm_xfer_areas_func_t func)
{
	snd_pcm_uframes_t xfer = 0;
	int err = 0;
	snd_pcm_state_t state = snd_pcm_state(pcm);

	if (size == 0)
		return 0;
	if (size > pcm->xfer_align)
		size -= size % pcm->xfer_align;

	switch (snd_enum_to_int(state)) {
	case SND_PCM_STATE_PREPARED:
	case SND_PCM_STATE_RUNNING:
		break;
	case SND_PCM_STATE_XRUN:
		return -EPIPE;
	default:
		return -EBADFD;
	}

	while (size > 0) {
		snd_pcm_uframes_t frames;
		snd_pcm_sframes_t avail;
	_again:
		avail = snd_pcm_avail_update(pcm);
		if (avail < 0) {
			err = -EPIPE;
			goto _end;
		}
		if (state == SND_PCM_STATE_PREPARED) {
			if (avail == 0) {
				err = -EPIPE;
				goto _end;
			}
		} else if (avail == 0 ||
			   (size >= pcm->xfer_align && 
			    (snd_pcm_uframes_t) avail < pcm->xfer_align)) {
			if (pcm->mode & SND_PCM_NONBLOCK) {
				err = -EAGAIN;
				goto _end;
			}

			err = snd_pcm_wait(pcm, -1);
			if (err < 0)
				break;
			state = snd_pcm_state(pcm);
			goto _again;
			
		}
		if ((snd_pcm_uframes_t) avail > pcm->xfer_align)
			avail -= avail % pcm->xfer_align;
		frames = size;
		if (frames > (snd_pcm_uframes_t) avail)
			frames = avail;
		assert(frames != 0);
		err = func(pcm, areas, offset, frames);
		if (err < 0)
			break;
		assert((snd_pcm_uframes_t)err == frames);
		offset += frames;
		size -= frames;
		xfer += frames;
#if 0
		state = snd_pcm_state(pcm);
		if (state == SND_PCM_STATE_XRUN) {
			err = -EPIPE;
			goto _end;
		}
#endif
		if (state == SND_PCM_STATE_PREPARED &&
		    pcm->start_mode == SND_PCM_START_DATA) {
			err = snd_pcm_start(pcm);
			if (err < 0)
				goto _end;
		}
	}
 _end:
	return xfer > 0 ? xfer : err;
}

snd_pcm_uframes_t _snd_pcm_mmap_hw_ptr(snd_pcm_t *pcm)
{
	return *pcm->hw_ptr;
}

snd_pcm_uframes_t _snd_pcm_boundary(snd_pcm_t *pcm)
{
	return pcm->boundary;
}

static const char *names[SND_PCM_HW_PARAM_LAST + 1] = {
	[SND_PCM_HW_PARAM_FORMAT] = "format",
	[SND_PCM_HW_PARAM_CHANNELS] = "channels",
	[SND_PCM_HW_PARAM_RATE] = "rate",
	[SND_PCM_HW_PARAM_PERIOD_TIME] = "period_time",
	[SND_PCM_HW_PARAM_BUFFER_TIME] = "buffer_time"
};

int snd_pcm_slave_conf(snd_config_t *conf, const char **namep, 
		       unsigned int count, ...)
{
	snd_config_iterator_t i, next;
	const char *str;
	struct {
		unsigned int index;
		int mandatory;
		void *ptr;
		int valid;
	} fields[count];
	unsigned int k;
	int pcm_valid = 0;
	int err;
	va_list args;
	if (snd_config_get_string(conf, &str) >= 0) {
		err = snd_config_search_alias(snd_config, "pcm_slave", str, &conf);
		if (err < 0) {
			SNDERR("unkown pcm_slave %s", str);
			return err;
		}
	}
	va_start(args, count);
	for (k = 0; k < count; ++k) {
		fields[k].index = va_arg(args, int);
		fields[k].mandatory = va_arg(args, int);
		fields[k].ptr = va_arg(args, void *);
		fields[k].valid = 0;
	}
	va_end(args);
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id = snd_config_get_id(n);
		if (strcmp(id, "comment") == 0)
			continue;
		if (strcmp(id, "pcm") == 0) {
			if (pcm_valid) {
			_duplicated:
				SNDERR("duplicated %s", id);
				return -EINVAL;
			}
			err = snd_config_get_string(n, namep);
			if (err < 0) {
			_invalid:
				SNDERR("invalid type for %s", id);
				return err;
			}
			pcm_valid = 1;
			continue;
		}
		for (k = 0; k < count; ++k) {
			unsigned int idx = fields[k].index;
			long v;
			assert(idx < SND_PCM_HW_PARAM_LAST);
			assert(names[idx]);
			if (strcmp(id, names[idx]) != 0)
				continue;
			if (fields[k].valid)
				goto _duplicated;
			switch (idx) {
			case SND_PCM_HW_PARAM_FORMAT:
			{
				snd_pcm_format_t f;
				err = snd_config_get_string(n, &str);
				if (err < 0)
					goto _invalid;
				f = snd_pcm_format_value(str);
				if (f == SND_PCM_FORMAT_UNKNOWN) {
					SNDERR("unknown format");
					return -EINVAL;
				}
				*(snd_pcm_format_t*)fields[k].ptr = f;
					break;
			}
			default:
				err = snd_config_get_integer(n, &v);
				if (err < 0)
					goto _invalid;
				*(int*)fields[k].ptr = v;
				break;
			}
			fields[k].valid = 1;
			break;
		}
		if (k < count)
			continue;
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}
	for (k = 0; k < count; ++k) {
		if (fields[k].mandatory && !fields[k].valid) {
			SNDERR("missing field %s", names[fields[k].index]);
			return -EINVAL;
		}
	}
	return 0;
}
		
#endif
