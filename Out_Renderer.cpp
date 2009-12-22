#include "Out_Renderer.h"
#include "Out_Effects.h"
#include "ConfigFile.h"

#define DEBUG_BUFFER_SIZE 255

#ifdef _DEBUG
	#include <crtdbg.h>
#endif

#define SYNC_START EnterCriticalSection(&critical_section)
#define SYNC_END LeaveCriticalSection(&critical_section)

namespace WinampOpenALOut
{

	Output_Renderer::Output_Renderer(
		unsigned int buffer_len,
		unsigned char a_channel,
		Output_Effects* the_effects)
	{
		InitializeCriticalSection(&critical_section);

		SYNC_START;

		// make sure all the pointers are set to zero
		conf_buffer_length = buffer_len;
		effects = the_effects;
		this->channel = a_channel;

		is_playing = false;
		stream_open = false;
		buffer_free = 0;
		buffers_free = 0;
		sample_rate = 0;
		number_of_channels = 0;
		bits_per_sample = 0;
		no_buffers = 0;
		bytes_per_sample_channel = 0;
		volume = 0;
		played = 0;
		source = 0;
		last_pause = 0;
		format = 0;
		buffer_size = 0;

		memset(buffers, 0, sizeof(buffer_type) * MAX_NO_BUFFERS);

		SYNC_END;
	}

	Output_Renderer::~Output_Renderer()
	{
		//ensure everything in memory is deleted
		DeleteCriticalSection(&critical_section);
	}

	void Output_Renderer::onError()
	{
		this->Close();
	}

	void Output_Renderer::CheckProcessedBuffers() 
	{

		ALint	buffers_processed = 0;
		ALuint	next_buffer = 0;
		ALenum	err = AL_NO_ERROR;

		unsigned int selected_buffer = 0u;

		buffers_processed = NO_BUFFERS_PROCESSED;

		// ask open al how many buffers have been processed
		alGetError();
		alGetSourcei(source, AL_BUFFERS_PROCESSED, &buffers_processed);
		if( (err = alGetError()) != AL_NO_ERROR )
		{
			if(err == AL_INVALID_VALUE) {
				MessageBoxA(NULL, "Error with Source - Invalid Value", "Error With Source", MB_OK);
			} else if (err == AL_INVALID_ENUM) {
				MessageBoxA(NULL, "Error with Source - Invalid Enumeration", "Error With Source", MB_OK);
			} else if (err == AL_INVALID_NAME) {
				MessageBoxA(NULL, "Error with Source - Invalid Name", "Error With Source", MB_OK);
			} else if (err == AL_INVALID_OPERATION) {
				MessageBoxA(NULL, "Error with Source - Invalid Operation", "Error With Source", MB_OK);
			} else {
				MessageBoxA(NULL, "Error with Source - Unknown Error", "Error With Source", MB_OK);
			}
			onError();
		}

		while (buffers_processed--)
		{
			next_buffer = DEFAULT_BUFFER;
			
			// clear any open al errors
			alGetError();

			/* unqueue *any* processed buffer. the buffer that has been
				processed will be put into uiNextBuffer */
			alSourceUnqueueBuffers(
				source,
				GET_ONE_BUFFER,
				&next_buffer); 

			// based on the bufferID determine the buffer index
			selected_buffer = UNKNOWN_BUFFER;
			for(unsigned int buffer_index=0 ; buffer_index < no_buffers ; buffer_index++)
			{
				if( (!buffers[buffer_index].available) && 
					buffers[buffer_index].buffer_id == next_buffer )
				{
					selected_buffer = buffer_index;
					buffers[buffer_index].available = true;

					if ( buffers[buffer_index].data != NULL )
					{
						delete buffers[buffer_index].data;
						buffers[buffer_index].data = NULL;
					}

					buffers_free++;
					break;
				}
			}

			/* if no errors have occured we can mark that
				buffer as available again */
			if(alGetError() == AL_NO_ERROR)
			{
				/* increase our played time position
				 (the delta for where we are in the current buffer is done
				 in get_output_time */
				buffer_free += buffers[selected_buffer].size;
				played += buffers[selected_buffer].size;
				buffers[selected_buffer].size = 0;

			} else { 
				MessageBoxA(NULL, "Error in Monitor Thread - Out of Range", "Error Monitoring", MB_OK);
				this->onError();
			}
		}
	}

	typedef struct
	{
		Output_Renderer* renderer;
		HANDLE playEvent;
	} play_Handle;

	static DWORD PlayThread(LPVOID playEvent) {
		play_Handle* p = (play_Handle*)playEvent;
		p->renderer->Play(p->playEvent);
		delete p;
		return 0;
	}

	void Output_Renderer::Play(HANDLE playEvent) {

		WaitForSingleObject(playEvent, INFINITE);

		alSourcePlay(this->source);
	}

	HANDLE Output_Renderer::CheckPlayState(HANDLE playEvent) {

		ALint	state = AL_SOURCE_STATE;
		ALint	queued_buffers = 0;
		HANDLE thread_handle = NULL;

		/*
			can the current state of the source
		*/
		/* AL_SOURCE_STATE, AL_INITIAL, AL_PLAYING, AL_PAUSED, AL_STOPPED */
		alGetError();
		alGetSourcei(source, AL_SOURCE_STATE, &state);
		
		if(alGetError() != AL_NO_ERROR)
		{
			this->onError();
		}

		if (state == AL_PLAYING)
		{
			// if the source is playing then we record that
			is_playing = true;
		}else{

			if(stream_open && 
				(state == AL_STOPPED))
			{
				is_playing = false;
				return thread_handle;
			}

			// if we're not playing check to see if any buffers are queued
			alGetError();
			alGetSourcei(source, AL_BUFFERS_QUEUED, &queued_buffers);

			if(alGetError() != AL_NO_ERROR)
			{
				MessageBoxA(NULL, "Error playing", "Error Playing", MB_OK);
				this->onError();
			}

			if (queued_buffers > 0)
			{
				// if any buffers are queued and we're not playing - play!
				if(!last_pause && !is_playing)
				{
					DWORD id = 0;

					play_Handle * h = new play_Handle;
					h->renderer = this;
					h->playEvent = playEvent;

					thread_handle = CreateThread(NULL, 0x1000,(LPTHREAD_START_ROUTINE)&PlayThread, h, NULL, &id);

#ifdef _DEBUGGING
					char dbg[DEBUG_BUFFER_SIZE] = {'\0'};
					sprintf_s(
						dbg,
						DEBUG_BUFFER_SIZE,
						"-> Renderer started to play!");
					this->log_debug_msg(dbg, __FILE__, __LINE__);
#endif

					is_playing = true;
				}
			} else {
				// otherwise we're finished
				is_playing = false;
			}
		}

		return thread_handle;
	}

	void Output_Renderer::log_debug_msg(char* msg, char* file, int line)
	{
		/* basic logging to file - only if we're in debug mode */
#ifdef _DEBUG
		FILE *debug_file = NULL;
		fopen_s(&debug_file, "out_openal.log", "a+");
		fprintf_s(debug_file,"%s, %d - %s\n", file, line, msg);
		fclose(debug_file);
#endif
	}

	/*
		open

		this procedure is invoked by winamp when it opens a file
		we store the sample rate, numchannels, bitsPerSample

		the open al buffers are created and an open al source
	*/
	int Output_Renderer::Open(
		int samplerate, 
		int numchannels, 
		int bitspersamp, 
		int bufferlenms, 
		int prebufferms) {
		
		SYNC_START;

		if(bitspersamp > SIXTEEN_BIT_PER_SAMPLE)
		{
			MessageBoxA(NULL, "This Plug-In only supports 8 and 16bit audio, please disable 24bit audio in Winamp", "Whoops", MB_OK);
			SYNC_END;
			return -1;
		}
#ifdef _DEBUGGING
		char dbg[DEBUG_BUFFER_SIZE] = {'\0'};
		sprintf_s(
			dbg,
			DEBUG_BUFFER_SIZE,
			"Open: Sample rate {%d}, Channels {%d}, Bits {%d}",
			samplerate,
			numchannels,
			bitspersamp);
		log_debug_msg(dbg, __FILE__, __LINE__);
#endif

		//record the format of the data we're getting
		sample_rate = samplerate;
		number_of_channels = numchannels;
		bits_per_sample = bitspersamp;

		// determine the format to output in
		switch(number_of_channels)
		{
			case 1:
				format = bits_per_sample == 8 ? alGetEnumValue("AL_FORMAT_MONO8") : alGetEnumValue("AL_FORMAT_MONO16");
				break;
			case 2:
				format = bits_per_sample == 8 ? alGetEnumValue("AL_FORMAT_STEREO8") : alGetEnumValue("AL_FORMAT_STEREO16");
				break;
			case 4:
				format = alGetEnumValue("AL_FORMAT_QUAD16");
				break;
			case 6:
				format = alGetEnumValue("AL_FORMAT_51CHN16");
				break;
			case 7:
				format = alGetEnumValue("AL_FORMAT_61CHN16");
				break;
			case 8:
				format = alGetEnumValue("AL_FORMAT_71CHN16");
				break;
		};

		// reset the play position back to zero
		last_pause = 0;
		played = 0;

		// determine the size of the buffer
		bytes_per_sample_channel = ((bits_per_sample >> SHIFT_BITS_TO_BYTES)*number_of_channels);

		buffer_size = (bytes_per_sample_channel * (sample_rate / ONE_SECOND_IN_MS)) * conf_buffer_length;

		//noBuffers = MAX_NO_BUFFERS;
		no_buffers = (buffer_size / MAXIMUM_BUFFER_SIZE);// + MAXIMUM_BUFFER_OFFSET;

		// if we have no buffers just assume we can use at least (MINIMUM_BUFFERS)
		// we need more than one so we can listen to one and write t'other
		// if that doesnt happen it will error anyway
		if(no_buffers < MINIMUM_BUFFERS)
		{
			no_buffers = MINIMUM_BUFFERS;
		}
		if(no_buffers > MAX_NO_BUFFERS)
		{
			no_buffers = MAX_NO_BUFFERS;
		}

		next_buffer_index = 0;
		this->buffer_free = no_buffers * MAXIMUM_BUFFER_SIZE;
		this->buffers_free = no_buffers;

#ifdef _DEBUGGING
		sprintf_s(
			dbg,
			DEBUG_BUFFER_SIZE,
			"-> Using {%d} buffers for total size of {%d}", 
			noBuffers,
			bufferSize);
		this->log_debug_msg(dbg, __FILE__, __LINE__);
#endif
		unsigned int in_xram_ok = 0;

		// allocate some buffers
		alGetError();
		for(unsigned int buffer_index=0 ; buffer_index<MAX_NO_BUFFERS ; buffer_index++)
		{
			if ( buffers[buffer_index].buffer_id != 0)
			{
				buffers[buffer_index].buffer_id = 0;
			}
			buffers[buffer_index].buffer_id = 0;
			buffers[buffer_index].size = 0;
			buffers[buffer_index].available = false;
			if ( buffers[buffer_index].data )
			{
				delete buffers[buffer_index].data;
				buffers[buffer_index].data = NULL;
			}
		}

		for(unsigned int buffer_index=0 ; buffer_index<no_buffers ; buffer_index++)
		{
			buffers[buffer_index].available = true;
			/* deallocate each buffer one-at-a-time */
			alGenBuffers( 1, &buffers[buffer_index].buffer_id );

			/* if xram is enabled write the buffer to XRAM */
			if ( xram_enabled == true &&
				alGetEnumValue("AL_EAX_RAM_FREE") > 0 &&
				eaxSetBufferMode > NULL &&
				eXRAMHardware > NULL)
			{
				ALboolean inhw = eaxSetBufferMode(
					1,
					&buffers[buffer_index].buffer_id,
					eXRAMHardware);

				if ( inhw == AL_FALSE )
				{
					log_debug_msg("Failed to set buffer as XRAM", __FILE__, __LINE__);
				}
				else
				{
					in_xram_ok++;
				}	
			}
		}

#ifdef _DEBUGGING
		if ( xram_enabled == true )
		{
			sprintf_s(
				dbg, 
				DEBUG_BUFFER_SIZE, 
				"--> {%d} Buffers from {%d} were stored in XRAM OK", 
				in_xram_ok, 
				noBuffers);
			log_debug_msg(dbg, __FILE__, __LINE__);
		}
#endif

		ALenum err = alGetError();
		if( err == AL_INVALID_VALUE || err == AL_OUT_OF_MEMORY)
		{
			if( err != AL_NO_ERROR) 
			{
				if(err == AL_OUT_OF_MEMORY) {
					MessageBoxA(NULL, "Error generating Buffer - Out of Memory", "Error", MB_OK);
				} else if(err == AL_INVALID_VALUE) {
					MessageBoxA(NULL, "Error generating  Buffer - Invalid Value", "Error", MB_OK);
				} else {
					MessageBoxA(NULL, "Error generating  Buffer - Unknown Error", "Error", MB_OK);
				}
				this->onError();
			}
			
			/* if we've created too many buffers try and
		     * use less */
			while(err != AL_NO_ERROR && no_buffers > 1)
			{
				alGetError();
				no_buffers--;
				for(unsigned int i=0;i<no_buffers;i++)
				{
					alGenBuffers( no_buffers, &buffers[i].buffer_id );
				}
				err = alGetError();
			}
		}

		// allocate a source
		alGetError();
		alGenSources( 1, &source );

		if( (err = alGetError()) != AL_NO_ERROR)
		{
			if(err == AL_OUT_OF_MEMORY) {
				MessageBoxA(NULL, "Error generating Source - Out of Memory", "Error", MB_OK);
			} else if(err == AL_INVALID_VALUE) {
				MessageBoxA(NULL, "Error generating Source - Invalid Value", "Error", MB_OK);
			} else if(err == AL_INVALID_OPERATION) {
				MessageBoxA(NULL, "Error generating Source - Invalid Operation", "Error", MB_OK);
			}  else {
				MessageBoxA(NULL, "Error generating Source - Unknown Error", "Error", MB_OK);
			}
			this->onError();
		}
		
		alGetError();
		alSourcei(source, AL_LOOPING, AL_FALSE);
		
		if(alGetError() != AL_NO_ERROR)
		{
			this->onError();
		}
		
		/* Effects */
		if ( effects != NULL )
		{
			effects->add_source(source);
		}

		// set the volume for the source
		this->SetVolumeInternal(volume);

		// we're not playing yet because we're prebuffering
		is_playing = false;
		// the stream is open and ready for the main thread
		stream_open = true;

		SYNC_END;

		return conf_buffer_length;
	}

	/*
		close

		this procedure is invoked by winamp when the file is closed
	*/
	void Output_Renderer::Close() 
	{
		SYNC_START;

		stream_open = false;

		// stop the source
		alSourceStop(source);

		played = 0;

		ALint state = 0;
		unsigned int timeout = 0;
		// query the state of the source and wait for it to stop
		alGetSourcei(source, AL_SOURCE_STATE, &state);
		while(state != AL_STOPPED && state != AL_INITIAL)
		{
			Sleep(1);
			if(timeout++ > CLOSE_TIMEOUT_COUNT) {
				break;
			}
		}

		int count_timeout = 0;
		// wait for all the buffers to be used up
		while(buffers_free != no_buffers)
		{
			CheckProcessedBuffers();
			Sleep(1);
			if ( ++count_timeout > CLOSE_TIMEOUT_COUNT )
			{
				break;
			}
		}
		// remove all buffers
		alSourcei(source, AL_BUFFER, 0);

		// delete the source
		alDeleteSources( 1, &source );

		// delete all the buffers
		for(unsigned int buffer_index=0 ; buffer_index<no_buffers ; buffer_index++)
		{
			alDeleteBuffers( 1, &buffers[buffer_index].buffer_id );
			buffers[buffer_index].buffer_id = 0;
		}

		// make sure all memory from the buffers is freed up,
		// if CheckProcessed is working correct this shouldn't run
		for(unsigned int buffer_index=0 ; buffer_index<no_buffers ; buffer_index++)
		{
			if ( buffers[buffer_index].data != NULL )
			{
				delete buffers[buffer_index].data;
				buffers[buffer_index].data = NULL;
			}
		}

		// just incase the thread has exitted, assume playing has stopped
		is_playing = false;

		no_buffers = NO_BUFFERS;
		buffer_free = 0;
		buffers_free = 0;
		sample_rate = NO_SAMPLE_RATE;
		bits_per_sample = NO_BITS_PER_SAMPLE;
		number_of_channels = NO_NUMBER_OF_CHANNELS;

		SYNC_END;

	}

	/*
		write

		this procedure is invoked by winamp when it attempts to write
		data to the plugin
	*/
	int Output_Renderer::Write(char *buf, int len)
	{
		SYNC_START;

		ALenum err = AL_NO_ERROR;

		// if we cannot write exit now (non-blocking op)
		if(buffers_free == 0 || !stream_open)
		{
			SYNC_END;
			return -1;
		}

		// if the buffer is valid (non-NULL)
		if (buf) {

#ifdef _DEBUGGING
			char dbg[DEBUG_BUFFER_SIZE] = {'\0'};

			sprintf_s(
				dbg,
				DEBUG_BUFFER_SIZE,
				"Writing data {0x%08X} with length {%d}",buf, len);
			log_debug_msg(dbg, __FILE__, __LINE__);
#endif
			
			// set it to ERROR'd so we can detect errors
			ALuint next_buffer = UNKNOWN_BUFFER;
			unsigned int selected_buffer = UNKNOWN_BUFFER;

			/*	go through the buffers and find an available one

				if we're here (critical section, canWrite=true) then
				we assume that one is! 
			*/

			/* this block shouldn't be needed but it just makes
			 * sure that the next buffer is available
			 */
			while ( !buffers[next_buffer_index].available )
			{
				next_buffer_index++;
			}

			/* allocate the next buffer */
			next_buffer = buffers[next_buffer_index].buffer_id;
			buffers[next_buffer_index].available = false;
			selected_buffer = next_buffer_index;
			buffers_free--;

			next_buffer_index = (next_buffer_index + 1) % no_buffers;

#ifdef _DEBUGGING
			sprintf_s(
				dbg,
				DEBUG_BUFFER_SIZE,
				"Writing using buffer {%d}", selectedBuffer);
			log_debug_msg(dbg, __FILE__, __LINE__);
#endif
			if(selected_buffer == UNKNOWN_BUFFER)
			{
				SYNC_END;
				return -1;
			}

			buffers[selected_buffer].data = buf;

			buffer_free -= len;
			buffers[selected_buffer].size = len;

#ifdef _DEBUGGING
			sprintf_s(
				dbg,
				DEBUG_BUFFER_SIZE,
				"Writing to OpenAL buffer,{%d} bytes", len);
			log_debug_msg(dbg, __FILE__, __LINE__);
#endif
			// buffer the data with the correct format
			alGetError();
			alBufferData(next_buffer, format, buf, len, sample_rate);
			if( (err = alGetError()) != AL_NO_ERROR)
			{
				if(err == AL_OUT_OF_MEMORY) {
					MessageBoxA(NULL, "Error Buffering - Out of Memory", "Error", MB_OK);
				} else if(err == AL_INVALID_VALUE) {
					MessageBoxA(NULL, "Error Buffering - Invalid Value", "Error", MB_OK);
				} else if(err == AL_INVALID_ENUM) {
					MessageBoxA(NULL, "Error Buffering - Invalid Enumeration", "Error", MB_OK);
				} else if( err == AL_INVALID_NAME) {
					MessageBoxA(NULL, "Error Buffering - Invalid Name", "Error", MB_OK);
				} else {
					char txt[200] = {'\0'};
					sprintf_s(txt, "Error Buffering - Unknown Error [%d]", err);
					MessageBoxA(NULL, txt, "Error", MB_OK);
				}
				this->onError();
			}

			// add the buffer to the source queue
			alGetError();
			alSourceQueueBuffers(source, 1, &next_buffer);
			if( (err = alGetError()) != AL_NO_ERROR)
			{
				if(err == AL_INVALID_NAME) {
					MessageBoxA(NULL, "Error Queuing - Invalid Name", "Error", MB_OK);
				} else if(err == AL_INVALID_OPERATION) {
					MessageBoxA(NULL, "Error Queuing - Invalid Operation", "Error", MB_OK);
				} else {
					MessageBoxA(NULL, "Error Queuing - Unknown Error", "Error", MB_OK);
				}
				this->onError();
			}
		}

		SYNC_END;

		return 0;
	}

	/*
		canwrite

		this procedure is invoked by winamp when it determining
		if more data can be written to the plugin
	*/
	int Output_Renderer::CanWrite() {
		
		SYNC_START;
		
		int r = EMPTY_THE_BUFFER;
		
		if(stream_open) {

			this->CheckProcessedBuffers();

			r = ((buffers_free > 0) ? buffer_free : 0);
		}

		SYNC_END;
		
		return r;
	}

	/*
		isplaying

		invoked by winamp to determine if there is still data in the buffers
		return 0 if empty
		return <0> if data present
	*/
	bool Output_Renderer::IsPlaying() {
		SYNC_START;

		this->CheckProcessedBuffers();

		bool r = is_playing && stream_open ? true : false;
		SYNC_END;
		return r;
	}

	/*
		pause

		returns the previous pause state
	*/
	int Output_Renderer::Pause(int pause) {
		SYNC_START;
		last_pause = pause;
		
		// clear the error state
		alGetError();

		// if this is un-paused during pre-buffering
		// we may start to play data before it's ready
		// and cause and under-run
		if(stream_open)
		{
			if(pause)
			{
				alSourcePause(source);
			}else{
				alSourcePlay(source);
			}
		}

		SYNC_END;
		return last_pause;
	}

	/*
		set volume (internal) - used to check the volume range
		and store the volume for later use
	*/
	void Output_Renderer::SetVolumeInternal(ALfloat new_volume)
	{
		if(new_volume <= VOLUME_MAX && new_volume >= VOLUME_MIN)
		{
			volume = new_volume;
			alSourcef(source,AL_GAIN, new_volume);
		}
	}

	/*
		flush

		this procedure is invoked by winamp to flush the buffers
		and start playing from time (t)ms.
	*/
	void Output_Renderer::Flush()
	{
		SYNC_START;

		// make sure we've stopped playing
		alSourceStop(source);

		SYNC_END;
	}

	void Output_Renderer::SetXRAMEnabled( bool enabled )
	{
		xram_enabled = enabled;
	}

	void Output_Renderer::SetMatrix ( speaker_T speaker )
	{
		const ALfloat x = ((float)speaker.x) / 255.0f;
		const ALfloat y = ((float)speaker.y) / 255.0f;
		const ALfloat z = ((float)speaker.z) / 255.0f;

		alSource3f(
			this->source,
			AL_POSITION,
			x,
			y,
			z);
	}

}