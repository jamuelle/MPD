/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
 * This project's homepage is: http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "mp4_decode.h"

#ifdef HAVE_FAAD

#include "command.h"
#include "utils.h"
#include "audio.h"
#include "log.h"
#include "pcm_utils.h"
#include "inputStream.h"
#include "outputBuffer.h"

#include "mp4ff/mp4ff.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <faad.h>

/* all code here is either based on or copied from FAAD2's frontend code */

int mp4_getAACTrack(mp4ff_t *infile) {
	/* find AAC track */
	int i, rc;
	int numTracks = mp4ff_total_tracks(infile);

	for (i = 0; i < numTracks; i++) {
		unsigned char *buff = NULL;
		int buff_size = 0;
#ifdef HAVE_MP4AUDIOSPECIFICCONFIG
		mp4AudioSpecificConfig mp4ASC;
#else
		unsigned long dummy1_32;
            	unsigned char dummy2_8, dummy3_8, dummy4_8, dummy5_8, dummy6_8,
                		dummy7_8, dummy8_8;
#endif
	
		mp4ff_get_decoder_config(infile, i, &buff, &buff_size);

		if (buff) {
#ifdef HAVE_MP4AUDIOSPECIFICCONFIG
			rc = AudioSpecificConfig(buff, buff_size, &mp4ASC);
#else
			rc = AudioSpecificConfig(buff, &dummy1_32, &dummy2_8,
					&dummy3_8, &dummy4_8, &dummy5_8, 
					&dummy6_8, &dummy7_8, &dummy8_8);
#endif
			free(buff);
			if (rc < 0) continue;
            		return i;
		}
	}

	/* can't decode this */
	return -1;
}

uint32_t mp4_inputStreamReadCallback(void *inStream, void *buffer, 
		uint32_t length) 
{
	return readFromInputStream((InputStream*) inStream, buffer, 1, length);
}
            
uint32_t mp4_inputStreamSeekCallback(void *inStream, uint64_t position) {
	return seekInputStream((InputStream *) inStream, position, SEEK_SET);
}       
		    

int mp4_decode(OutputBuffer * cb, DecoderControl * dc) {
	mp4ff_t * mp4fh;
	mp4ff_callback_t * mp4cb; 
	int32_t track;
	float time;
	int32_t scale;
	faacDecHandle decoder;
	faacDecFrameInfo frameInfo;
	faacDecConfigurationPtr config;
	unsigned char * mp4Buffer;
	int mp4BufferSize;
	unsigned long sampleRate;
	unsigned char channels;
	long sampleId;
	long numSamples;
	int eof = 0;
	long dur;
	unsigned int sampleCount;
	char * sampleBuffer;
	size_t sampleBufferLen;
	unsigned int initial = 1;
	float * seekTable;
	long seekTableEnd = -1;
	int seekPositionFound = 0;
	long offset;
	mpd_uint16 bitRate = 0;
	InputStream inStream;

	if(openInputStream(&inStream,dc->file) < 0) {
		ERROR("failed to open %s\n",dc->file);
		return -1;
	}

	mp4cb = malloc(sizeof(mp4ff_callback_t));
	mp4cb->read = mp4_inputStreamReadCallback;
	mp4cb->seek = mp4_inputStreamSeekCallback;
	mp4cb->user_data = &inStream;

	mp4fh = mp4ff_open_read(mp4cb);
	if(!mp4fh) {
		ERROR("Input does not appear to be a mp4 stream.\n");
		free(mp4cb);
		closeInputStream(&inStream);
		return -1;
	}

	track = mp4_getAACTrack(mp4fh);
	if(track < 0) {
		ERROR("No AAC track found in mp4 stream.\n");
		mp4ff_close(mp4fh);
		closeInputStream(&inStream);
		free(mp4cb);
		return -1;
	}

	decoder = faacDecOpen();

	config = faacDecGetCurrentConfiguration(decoder);
	config->outputFormat = FAAD_FMT_16BIT;
#ifdef HAVE_FAACDECCONFIGURATION_DOWNMATRIX
	config->downMatrix = 1;
#endif
#ifdef HAVE_FAACDECCONFIGURATION_DONTUPSAMPLEIMPLICITSBR
	config->dontUpSampleImplicitSBR = 0;
#endif
	faacDecSetConfiguration(decoder,config);

	dc->audioFormat.bits = 16;

	mp4Buffer = NULL;
	mp4BufferSize = 0;
	mp4ff_get_decoder_config(mp4fh,track,&mp4Buffer,&mp4BufferSize);

	if(faacDecInit2(decoder,mp4Buffer,mp4BufferSize,&sampleRate,&channels)
			< 0)
	{
		ERROR("Error not a AAC stream.\n");
		faacDecClose(decoder);
		mp4ff_close(mp4fh);
		free(mp4cb);
		closeInputStream(&inStream);
		return -1;
	}

	dc->audioFormat.sampleRate = sampleRate;
	dc->audioFormat.channels = channels;
	time = mp4ff_get_track_duration_use_offsets(mp4fh,track);
	scale = mp4ff_time_scale(mp4fh,track);

	if(mp4Buffer) free(mp4Buffer);

	if(scale < 0) {
		ERROR("Error getting audio format of mp4 AAC track.\n");
		faacDecClose(decoder);
		mp4ff_close(mp4fh);
		closeInputStream(&inStream);
		free(mp4cb);
		return -1;
	}
	dc->totalTime = ((float)time)/scale;

	numSamples = mp4ff_num_samples(mp4fh,track);

	time = 0.0;

	seekTable = malloc(sizeof(float)*numSamples);

	for(sampleId=0; sampleId<numSamples && !eof; sampleId++) {
		if(dc->seek && seekTableEnd>1 && 
				seekTable[seekTableEnd]>=dc->seekWhere)
		{
			int i = 2;
			while(seekTable[i]<dc->seekWhere) i++;
			sampleId = i-1;
			time = seekTable[sampleId];
		}

		dur = mp4ff_get_sample_duration(mp4fh,track,sampleId);
		offset = mp4ff_get_sample_offset(mp4fh,track,sampleId);

		if(sampleId>seekTableEnd) {
			seekTable[sampleId] = time;
			seekTableEnd = sampleId;
		}

		if(sampleId==0) dur = 0;
		if(offset>dur) dur = 0;
		else dur-=offset;
		time+=((float)dur)/scale;

		if(dc->seek && time>dc->seekWhere) seekPositionFound = 1;

		if(dc->seek && seekPositionFound) {
			seekPositionFound = 0;
                        clearOutputBuffer(cb);
                        dc->seekChunk = cb->end;
			dc->seek = 0;
		}

		if(dc->seek) continue;
		
		if(mp4ff_read_sample(mp4fh,track,sampleId,&mp4Buffer,
				&mp4BufferSize) == 0)
		{
			eof = 1;
			continue;
		}

#ifdef HAVE_FAAD_BUFLEN_FUNCS
		sampleBuffer = faacDecDecode(decoder,&frameInfo,mp4Buffer,
						mp4BufferSize);
#else
		sampleBuffer = faacDecDecode(decoder,&frameInfo,mp4Buffer);
#endif

		if(mp4Buffer) free(mp4Buffer);
		if(frameInfo.error > 0) {
			ERROR("error decoding MP4 file: %s\n",dc->file);
			ERROR("faad2 error: %s\n",
				faacDecGetErrorMessage(frameInfo.error));
			eof = 1;
			break;
		}

		if(dc->state != DECODE_STATE_DECODE) {
			channels = frameInfo.channels;
#ifdef HAVE_FAACDECFRAMEINFO_SAMPLERATE
			scale = frameInfo.samplerate;
#endif
			dc->audioFormat.sampleRate = scale;
			dc->audioFormat.channels = frameInfo.channels;
                        getOutputAudioFormat(&(dc->audioFormat),
                                        &(cb->audioFormat));
			dc->state = DECODE_STATE_DECODE;
		}

		if(channels*(dur+offset) > frameInfo.samples) {
			dur = frameInfo.samples/channels;
			offset = 0;
		}

		sampleCount = (unsigned long)(dur*channels);

		if(sampleCount>0) {
			initial =0;
			bitRate = frameInfo.bytesconsumed*8.0*
				frameInfo.channels*scale/
				frameInfo.samples/1000+0.5;
		}
			

		sampleBufferLen = sampleCount*2;

		sampleBuffer+=offset*channels*2;

		sendDataToOutputBuffer(cb, NULL, dc, 1, sampleBuffer,
				sampleBufferLen, time, bitRate);
		if(dc->stop) {
			eof = 1;
			break;
		}
	}

	flushOutputBuffer(cb);

	free(seekTable);
	faacDecClose(decoder);
	mp4ff_close(mp4fh);
	closeInputStream(&inStream);
	free(mp4cb);

	if(dc->state != DECODE_STATE_DECODE) return -1;

	if(dc->seek) dc->seek = 0;

	if(dc->stop) {
		dc->state = DECODE_STATE_STOP;
		dc->stop = 0;
	}
	else dc->state = DECODE_STATE_STOP;

	return 0;
}

#endif /* HAVE_FAAD */
/* vim:set shiftwidth=4 tabstop=8 expandtab: */
