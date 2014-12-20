/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* necessary because libavutil/common.h uses UINT64_C */
#define __STDC_CONSTANT_MACROS

#include "lib/ffmpeg/Time.hxx"
#include "config.h"
#include "FfmpegDecoderPlugin.hxx"
#include "lib/ffmpeg/Domain.hxx"
#include "lib/ffmpeg/Error.hxx"
#include "lib/ffmpeg/LogError.hxx"
#include "lib/ffmpeg/LogCallback.hxx"
#include "lib/ffmpeg/Buffer.hxx"
#include "../DecoderAPI.hxx"
#include "FfmpegMetaData.hxx"
#include "FfmpegIo.hxx"
#include "tag/TagHandler.hxx"
#include "input/InputStream.hxx"
#include "CheckAudioFormat.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "LogV.hxx"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>

#if LIBAVUTIL_VERSION_MAJOR >= 53
#include <libavutil/frame.h>
#endif
}

#include <assert.h>
#include <string.h>

/**
 * API compatibility wrapper for av_open_input_stream() and
 * avformat_open_input().
 */
static int
mpd_ffmpeg_open_input(AVFormatContext **ic_ptr,
		      AVIOContext *pb,
		      const char *filename,
		      AVInputFormat *fmt)
{
	AVFormatContext *context = avformat_alloc_context();
	if (context == nullptr)
		return AVERROR(ENOMEM);

	context->pb = pb;
	*ic_ptr = context;
	return avformat_open_input(ic_ptr, filename, fmt, nullptr);
}

static bool
ffmpeg_init(gcc_unused const config_param &param)
{
	av_log_set_callback(FfmpegLogCallback);

	av_register_all();
	return true;
}

gcc_pure
static int
ffmpeg_find_audio_stream(const AVFormatContext &format_context)
{
	for (unsigned i = 0; i < format_context.nb_streams; ++i)
		if (format_context.streams[i]->codec->codec_type ==
		    AVMEDIA_TYPE_AUDIO)
			return i;

	return -1;
}

/**
 * Accessor for AVStream::start_time that replaces AV_NOPTS_VALUE with
 * zero.  We can't use AV_NOPTS_VALUE in calculations, and we simply
 * assume that the stream's start time is zero, which appears to be
 * the best way out of that situation.
 */
static constexpr int64_t
start_time_fallback(const AVStream &stream)
{
	return FfmpegTimestampFallback(stream.start_time, 0);
}

static void
copy_interleave_frame2(uint8_t *dest, uint8_t **src,
		       unsigned nframes, unsigned nchannels,
		       unsigned sample_size)
{
	for (unsigned frame = 0; frame < nframes; ++frame) {
		for (unsigned channel = 0; channel < nchannels; ++channel) {
			memcpy(dest, src[channel] + frame * sample_size,
			       sample_size);
			dest += sample_size;
		}
	}
}

/**
 * Copy PCM data from a non-empty AVFrame to an interleaved buffer.
 */
static ConstBuffer<void>
copy_interleave_frame(const AVCodecContext &codec_context,
		      const AVFrame &frame,
		      FfmpegBuffer &global_buffer,
		      Error &error)
{
	assert(frame.nb_samples > 0);

	int plane_size;
	const int data_size =
		av_samples_get_buffer_size(&plane_size,
					   codec_context.channels,
					   frame.nb_samples,
					   codec_context.sample_fmt, 1);
	assert(data_size != 0);
	if (data_size < 0) {
		SetFfmpegError(error, data_size);
		return 0;
	}

	void *output_buffer;
	if (av_sample_fmt_is_planar(codec_context.sample_fmt) &&
	    codec_context.channels > 1) {
		output_buffer = global_buffer.GetT<uint8_t>(data_size);
		if (output_buffer == nullptr) {
			/* Not enough memory - shouldn't happen */
			error.SetErrno(ENOMEM);
			return 0;
		}

		copy_interleave_frame2((uint8_t *)output_buffer,
				       frame.extended_data,
				       frame.nb_samples,
				       codec_context.channels,
				       av_get_bytes_per_sample(codec_context.sample_fmt));
	} else {
		output_buffer = frame.extended_data[0];
	}

	return { output_buffer, (size_t)data_size };
}

/**
 * Convert AVPacket::pts to a stream-relative time stamp (still in
 * AVStream::time_base units).  Returns a negative value on error.
 */
gcc_pure
static int64_t
StreamRelativePts(const AVPacket &packet, const AVStream &stream)
{
	auto pts = packet.pts;
	if (pts < 0 || pts == int64_t(AV_NOPTS_VALUE))
		return -1;

	auto start = start_time_fallback(stream);
	return pts - start;
}

/**
 * Convert a non-negative stream-relative time stamp in
 * AVStream::time_base units to a PCM frame number.
 */
gcc_pure
static uint64_t
PtsToPcmFrame(uint64_t pts, const AVStream &stream,
	      const AVCodecContext &codec_context)
{
	return av_rescale_q(pts, stream.time_base, codec_context.time_base);
}

/**
 * Decode an #AVPacket and send the resulting PCM data to the decoder
 * API.
 *
 * @param min_frame skip all data before this PCM frame number; this
 * is used after seeking to skip data in an AVPacket until the exact
 * desired time stamp has been reached
 */
static DecoderCommand
ffmpeg_send_packet(Decoder &decoder, InputStream &is,
		   AVPacket &&packet,
		   AVCodecContext &codec_context,
		   const AVStream &stream,
		   AVFrame &frame,
		   uint64_t min_frame, size_t pcm_frame_size,
		   FfmpegBuffer &buffer)
{
	size_t skip_bytes = 0;

	const auto pts = StreamRelativePts(packet, stream);
	if (pts >= 0) {
		if (min_frame > 0) {
			auto cur_frame = PtsToPcmFrame(pts, stream,
						       codec_context);
			if (cur_frame < min_frame)
				skip_bytes = pcm_frame_size * (min_frame - cur_frame);
		} else
			decoder_timestamp(decoder,
					  FfmpegTimeToDouble(pts,
							     stream.time_base));
	}

	Error error;

	DecoderCommand cmd = DecoderCommand::NONE;
	while (packet.size > 0 && cmd == DecoderCommand::NONE) {
		int got_frame = 0;
		int len = avcodec_decode_audio4(&codec_context,
						&frame, &got_frame,
						&packet);
		if (len < 0) {
			/* if error, we skip the frame */
			LogFfmpegError(len, "decoding failed, frame skipped");
			break;
		}

		packet.data += len;
		packet.size -= len;

		if (!got_frame || frame.nb_samples <= 0)
			continue;

		auto output_buffer =
			copy_interleave_frame(codec_context, frame,
					      buffer, error);
		if (output_buffer.IsNull()) {
			/* this must be a serious error,
			   e.g. OOM */
			LogError(error);
			return DecoderCommand::STOP;
		}

		if (skip_bytes > 0) {
			if (skip_bytes >= output_buffer.size) {
				skip_bytes -= output_buffer.size;
				continue;
			}

			output_buffer.data =
				(const uint8_t *)output_buffer.data + skip_bytes;
			output_buffer.size -= skip_bytes;
			skip_bytes = 0;
		}

		cmd = decoder_data(decoder, is,
				   output_buffer.data, output_buffer.size,
				   codec_context.bit_rate / 1000);
	}
	return cmd;
}

static DecoderCommand
ffmpeg_send_packet(Decoder &decoder, InputStream &is,
		   const AVPacket &packet,
		   AVCodecContext &codec_context,
		   const AVStream &stream,
		   AVFrame &frame,
		   uint64_t min_frame, size_t pcm_frame_size,
		   FfmpegBuffer &buffer)
{
	return ffmpeg_send_packet(decoder, is,
				  /* copy the AVPacket, because FFmpeg
				     < 3.0 requires this */
				  AVPacket(packet),
				  codec_context, stream,
				  frame, min_frame, pcm_frame_size,
				  buffer);
}

gcc_const
static SampleFormat
ffmpeg_sample_format(enum AVSampleFormat sample_fmt)
{
	switch (sample_fmt) {
	case AV_SAMPLE_FMT_S16:
	case AV_SAMPLE_FMT_S16P:
		return SampleFormat::S16;

	case AV_SAMPLE_FMT_S32:
	case AV_SAMPLE_FMT_S32P:
		return SampleFormat::S32;

	case AV_SAMPLE_FMT_FLT:
	case AV_SAMPLE_FMT_FLTP:
		return SampleFormat::FLOAT;

	default:
		break;
	}

	char buffer[64];
	const char *name = av_get_sample_fmt_string(buffer, sizeof(buffer),
						    sample_fmt);
	if (name != nullptr)
		FormatError(ffmpeg_domain,
			    "Unsupported libavcodec SampleFormat value: %s (%d)",
			    name, sample_fmt);
	else
		FormatError(ffmpeg_domain,
			    "Unsupported libavcodec SampleFormat value: %d",
			    sample_fmt);
	return SampleFormat::UNDEFINED;
}

static AVInputFormat *
ffmpeg_probe(Decoder *decoder, InputStream &is)
{
	constexpr size_t BUFFER_SIZE = 16384;
	constexpr size_t PADDING = 16;

	unsigned char buffer[BUFFER_SIZE];
	size_t nbytes = decoder_read(decoder, is, buffer, BUFFER_SIZE);
	if (nbytes <= PADDING || !is.LockRewind(IgnoreError()))
		return nullptr;

	/* some ffmpeg parsers (e.g. ac3_parser.c) read a few bytes
	   beyond the declared buffer limit, which makes valgrind
	   angry; this workaround removes some padding from the buffer
	   size */
	nbytes -= PADDING;

	AVProbeData avpd;

	/* new versions of ffmpeg may add new attributes, and leaving
	   them uninitialized may crash; hopefully, zero-initializing
	   everything we don't know is ok */
	memset(&avpd, 0, sizeof(avpd));

	avpd.buf = buffer;
	avpd.buf_size = nbytes;
	avpd.filename = is.GetURI();

#ifdef AVPROBE_SCORE_MIME
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(56, 5, 1)
	/* this attribute was added in libav/ffmpeg version 11, but
	   unfortunately it's "uint8_t" instead of "char", and it's
	   not "const" - wtf? */
	avpd.mime_type = (uint8_t *)const_cast<char *>(is.GetMimeType());
#else
	/* API problem fixed in FFmpeg 2.5 */
	avpd.mime_type = is.GetMimeType();
#endif
#endif

	return av_probe_input_format(&avpd, true);
}

static void
ffmpeg_decode(Decoder &decoder, InputStream &input)
{
	AVInputFormat *input_format = ffmpeg_probe(&decoder, input);
	if (input_format == nullptr)
		return;

	FormatDebug(ffmpeg_domain, "detected input format '%s' (%s)",
		    input_format->name, input_format->long_name);

	AvioStream stream(&decoder, input);
	if (!stream.Open()) {
		LogError(ffmpeg_domain, "Failed to open stream");
		return;
	}

	AVFormatContext *format_context = nullptr;
	if (mpd_ffmpeg_open_input(&format_context, stream.io,
				  input.GetURI(),
				  input_format) != 0) {
		LogError(ffmpeg_domain, "Open failed");
		return;
	}

	const int find_result =
		avformat_find_stream_info(format_context, nullptr);
	if (find_result < 0) {
		LogError(ffmpeg_domain, "Couldn't find stream info");
		avformat_close_input(&format_context);
		return;
	}

	int audio_stream = ffmpeg_find_audio_stream(*format_context);
	if (audio_stream == -1) {
		LogError(ffmpeg_domain, "No audio stream inside");
		avformat_close_input(&format_context);
		return;
	}

	AVStream &av_stream = *format_context->streams[audio_stream];

	AVCodecContext *codec_context = av_stream.codec;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54, 25, 0)
	const AVCodecDescriptor *codec_descriptor =
		avcodec_descriptor_get(codec_context->codec_id);
	if (codec_descriptor != nullptr)
		FormatDebug(ffmpeg_domain, "codec '%s'",
			    codec_descriptor->name);
#else
	if (codec_context->codec_name[0] != 0)
		FormatDebug(ffmpeg_domain, "codec '%s'",
			    codec_context->codec_name);
#endif

	AVCodec *codec = avcodec_find_decoder(codec_context->codec_id);

	if (!codec) {
		LogError(ffmpeg_domain, "Unsupported audio codec");
		avformat_close_input(&format_context);
		return;
	}

	const SampleFormat sample_format =
		ffmpeg_sample_format(codec_context->sample_fmt);
	if (sample_format == SampleFormat::UNDEFINED) {
		// (error message already done by ffmpeg_sample_format())
		avformat_close_input(&format_context);
		return;
	}

	Error error;
	AudioFormat audio_format;
	if (!audio_format_init_checked(audio_format,
				       codec_context->sample_rate,
				       sample_format,
				       codec_context->channels, error)) {
		LogError(error);
		avformat_close_input(&format_context);
		return;
	}

	/* the audio format must be read from AVCodecContext by now,
	   because avcodec_open() has been demonstrated to fill bogus
	   values into AVCodecContext.channels - a change that will be
	   reverted later by avcodec_decode_audio3() */

	const int open_result = avcodec_open2(codec_context, codec, nullptr);
	if (open_result < 0) {
		LogError(ffmpeg_domain, "Could not open codec");
		avformat_close_input(&format_context);
		return;
	}

	const SignedSongTime total_time =
		FromFfmpegTimeChecked(av_stream.duration, av_stream.time_base);

	decoder_initialized(decoder, audio_format,
			    input.IsSeekable(), total_time);

#if LIBAVUTIL_VERSION_MAJOR >= 53
	AVFrame *frame = av_frame_alloc();
#else
	AVFrame *frame = avcodec_alloc_frame();
#endif
	if (!frame) {
		LogError(ffmpeg_domain, "Could not allocate frame");
		avformat_close_input(&format_context);
		return;
	}

	FfmpegBuffer interleaved_buffer;

	uint64_t min_frame = 0;

	DecoderCommand cmd;
	do {
		AVPacket packet;
		if (av_read_frame(format_context, &packet) < 0)
			/* end of file */
			break;

		if (packet.stream_index == audio_stream) {
			cmd = ffmpeg_send_packet(decoder, input,
						 packet,
						 *codec_context,
						 av_stream,
						 *frame,
						 min_frame, audio_format.GetFrameSize(),
						 interleaved_buffer);
			min_frame = 0;
		} else
			cmd = decoder_get_command(decoder);

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(56, 25, 100)
		av_packet_unref(&packet);
#else
		av_free_packet(&packet);
#endif

		if (cmd == DecoderCommand::SEEK) {
			int64_t where =
				ToFfmpegTime(decoder_seek_time(decoder),
					     av_stream.time_base) +
				start_time_fallback(av_stream);

			/* AVSEEK_FLAG_BACKWARD asks FFmpeg to seek to
			   the packet boundary before the seek time
			   stamp, not after */

			if (av_seek_frame(format_context, audio_stream, where,
					  AVSEEK_FLAG_ANY|AVSEEK_FLAG_BACKWARD) < 0)
				decoder_seek_error(decoder);
			else {
				avcodec_flush_buffers(codec_context);
				min_frame = decoder_seek_where_frame(decoder);
				decoder_command_finished(decoder);
			}
		}
	} while (cmd != DecoderCommand::STOP);

#if LIBAVUTIL_VERSION_MAJOR >= 53
	av_frame_free(&frame);
#elif LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54, 28, 0)
	avcodec_free_frame(&frame);
#else
	av_free(frame);
#endif

	avcodec_close(codec_context);
	avformat_close_input(&format_context);
}

static bool
ffmpeg_scan_stream(InputStream &is,
		   const struct tag_handler *handler, void *handler_ctx)
{
	AVInputFormat *input_format = ffmpeg_probe(nullptr, is);
	if (input_format == nullptr)
		return false;

	AvioStream stream(nullptr, is);
	if (!stream.Open())
		return false;

	AVFormatContext *f = nullptr;
	if (mpd_ffmpeg_open_input(&f, stream.io, is.GetURI(),
				  input_format) != 0)
		return false;

	const int find_result =
		avformat_find_stream_info(f, nullptr);
	if (find_result < 0) {
		avformat_close_input(&f);
		return false;
	}

	if (f->duration != (int64_t)AV_NOPTS_VALUE) {
		const auto duration =
			SongTime::FromScale<uint64_t>(f->duration,
						      AV_TIME_BASE);
		tag_handler_invoke_duration(handler, handler_ctx, duration);
	}

	FfmpegScanDictionary(f->metadata, handler, handler_ctx);
	int idx = ffmpeg_find_audio_stream(*f);
	if (idx >= 0)
		FfmpegScanDictionary(f->streams[idx]->metadata,
				     handler, handler_ctx);

	avformat_close_input(&f);
	return true;
}

/**
 * A list of extensions found for the formats supported by ffmpeg.
 * This list is current as of 02-23-09; To find out if there are more
 * supported formats, check the ffmpeg changelog since this date for
 * more formats.
 */
static const char *const ffmpeg_suffixes[] = {
	"16sv", "3g2", "3gp", "4xm", "8svx", "aa3", "aac", "ac3", "afc", "aif",
	"aifc", "aiff", "al", "alaw", "amr", "anim", "apc", "ape", "asf",
	"atrac", "au", "aud", "avi", "avm2", "avs", "bap", "bfi", "c93", "cak",
	"cin", "cmv", "cpk", "daud", "dct", "divx", "dts", "dv", "dvd", "dxa",
	"eac3", "film", "flac", "flc", "fli", "fll", "flx", "flv", "g726",
	"gsm", "gxf", "iss", "m1v", "m2v", "m2t", "m2ts",
	"m4a", "m4b", "m4v",
	"mad",
	"mj2", "mjpeg", "mjpg", "mka", "mkv", "mlp", "mm", "mmf", "mov", "mp+",
	"mp1", "mp2", "mp3", "mp4", "mpc", "mpeg", "mpg", "mpga", "mpp", "mpu",
	"mve", "mvi", "mxf", "nc", "nsv", "nut", "nuv", "oga", "ogm", "ogv",
	"ogx", "oma", "ogg", "omg", "opus", "psp", "pva", "qcp", "qt", "r3d", "ra",
	"ram", "rl2", "rm", "rmvb", "roq", "rpl", "rvc", "shn", "smk", "snd",
	"sol", "son", "spx", "str", "swf", "tak", "tgi", "tgq", "tgv", "thp", "ts",
	"tsp", "tta", "xa", "xvid", "uv", "uv2", "vb", "vid", "vob", "voc",
	"vp6", "vmd", "wav", "webm", "wma", "wmv", "wsaud", "wsvga", "wv",
	"wve",
	nullptr
};

static const char *const ffmpeg_mime_types[] = {
	"application/flv",
	"application/m4a",
	"application/mp4",
	"application/octet-stream",
	"application/ogg",
	"application/x-ms-wmz",
	"application/x-ms-wmd",
	"application/x-ogg",
	"application/x-shockwave-flash",
	"application/x-shorten",
	"audio/8svx",
	"audio/16sv",
	"audio/aac",
	"audio/aacp",
	"audio/ac3",
	"audio/aiff"
	"audio/amr",
	"audio/basic",
	"audio/flac",
	"audio/m4a",
	"audio/mp4",
	"audio/mpeg",
	"audio/musepack",
	"audio/ogg",
	"audio/opus",
	"audio/qcelp",
	"audio/vorbis",
	"audio/vorbis+ogg",
	"audio/x-8svx",
	"audio/x-16sv",
	"audio/x-aac",
	"audio/x-ac3",
	"audio/x-aiff"
	"audio/x-alaw",
	"audio/x-au",
	"audio/x-dca",
	"audio/x-eac3",
	"audio/x-flac",
	"audio/x-gsm",
	"audio/x-mace",
	"audio/x-matroska",
	"audio/x-monkeys-audio",
	"audio/x-mpeg",
	"audio/x-ms-wma",
	"audio/x-ms-wax",
	"audio/x-musepack",
	"audio/x-ogg",
	"audio/x-vorbis",
	"audio/x-vorbis+ogg",
	"audio/x-pn-realaudio",
	"audio/x-pn-multirate-realaudio",
	"audio/x-speex",
	"audio/x-tta"
	"audio/x-voc",
	"audio/x-wav",
	"audio/x-wma",
	"audio/x-wv",
	"video/anim",
	"video/quicktime",
	"video/msvideo",
	"video/ogg",
	"video/theora",
	"video/webm",
	"video/x-dv",
	"video/x-flv",
	"video/x-matroska",
	"video/x-mjpeg",
	"video/x-mpeg",
	"video/x-ms-asf",
	"video/x-msvideo",
	"video/x-ms-wmv",
	"video/x-ms-wvx",
	"video/x-ms-wm",
	"video/x-ms-wmx",
	"video/x-nut",
	"video/x-pva",
	"video/x-theora",
	"video/x-vid",
	"video/x-wmv",
	"video/x-xvid",

	/* special value for the "ffmpeg" input plugin: all streams by
	   the "ffmpeg" input plugin shall be decoded by this
	   plugin */
	"audio/x-mpd-ffmpeg",

	nullptr
};

const struct DecoderPlugin ffmpeg_decoder_plugin = {
	"ffmpeg",
	ffmpeg_init,
	nullptr,
	ffmpeg_decode,
	nullptr,
	nullptr,
	ffmpeg_scan_stream,
	nullptr,
	ffmpeg_suffixes,
	ffmpeg_mime_types
};
