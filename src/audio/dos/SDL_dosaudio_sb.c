/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_DOS_SOUNDBLASTER

#include "SDL_dosaudio_sb.h"

// Set to 1 to force 8-bit mono (pre-SB16) code path even on SB16 hardware.
// Useful for testing in DOSBox which always emulates an SB16 (DSP 4.x).
#define FORCE_SB_8BIT 0

static int soundblaster_base_port = -1;
static int soundblaster_irq = -1;
static int soundblaster_dma_channel = -1;
static int soundblaster_highdma_channel = -1;
static int soundblaster_version = -1;
static int soundblaster_version_minor = -1;
static bool soundblaster_is_sb16 = false;  // false when FORCE_SB_8BIT or DSP < 4
static Uint8 soundblaster_silence_value = 0;

static void ResetSoundBlasterDSP(void)
{
    // reset the DSP.
    const int reset_port = soundblaster_base_port + 0x6;
    outportb(reset_port, 1);
    SDL_DelayPrecise(3000);  // wait at least 3 microseconds for hardware to see it.
    outportb(reset_port, 0);
}

static bool ReadSoundBlasterReady(void)
{
    const int ready_port = soundblaster_base_port + 0xE;
    return ((inportb(ready_port) & (1<<7)) != 0);
}

static void WriteSoundBlasterDSP(const Uint8 val)
{
    const int port = soundblaster_base_port + 0xC;
    while (inportb(port) & (1<<7)) { /* spin until the DSP says it can accept a command. */ }
    outportb(port, val);
}

static Uint8 ReadSoundBlasterDSP(void)
{
    const int query_port = soundblaster_base_port + 0xA;
    while (!ReadSoundBlasterReady()) { /* spin until the DSP says it has a reply available. */ }
    return (Uint8) inportb(query_port);
}

volatile int audio_streams_locked = 0;
static SDL_AudioDevice *opened_soundblaster_device = NULL;
static volatile bool soundblaster_irq_pending = false;
static volatile int soundblaster_irq_count = 0;
static volatile int soundblaster_mix_count = 0;
// These are copies of the values from hidden, cached here so the IRQ handler
// can access them without chasing pointers into potentially-unlocked memory.
static volatile Uint8 *isr_dma_buffer = NULL;
static volatile int isr_dma_halfdma = 0;
static volatile int isr_dma_channel = 0;
static volatile int isr_irq_ack_port = 0;
static volatile Uint8 isr_silence_value = 0;
static volatile bool isr_is_16bit = false;
static void SoundBlasterIRQHandler(void)  // this is wrapped in a thing that handles IRET, etc.
{
    // Set flag and acknowledge hardware. The actual mixing happens in
    // SDL_DOS_PumpAudio() called from the main loop. We cannot call
    // SDL_PlaybackAudioThreadIterate() here (reentrancy issues with malloc, etc.).
    soundblaster_irq_pending = true;
    soundblaster_irq_count++;

    // If the main loop hasn't mixed since the last IRQ, the half-buffer that just
    // finished is stale. Silence it so the hardware plays silence instead of
    // repeating old audio (prevents stuttering/noise during load screens).
    // We use an inline loop instead of SDL_memset to avoid calling library functions
    // that may not be in locked memory.
    if (isr_dma_buffer && (soundblaster_irq_count - soundblaster_mix_count) > 1) {
        const int halfdma = isr_dma_halfdma;
        const int ch = isr_dma_channel;
        const Uint8 silence = isr_silence_value;
        int count;
        // Read DMA position to find which half is currently playing, then silence the OTHER half.
        if (isr_is_16bit) {
            // High DMA (16-bit, channels 5-7): ports in 0xC0+ range, counts in words
            count = (int) inportb(0xC0 + (ch - 4) * 4 + 2);
            count += (int) inportb(0xC0 + (ch - 4) * 4 + 2) << 8;
            // count is in 16-bit words; halfdma is in bytes, so compare against halfdma/2
            {
                volatile Uint8 *stale_half = isr_dma_buffer + (count < (halfdma / 2) ? halfdma : 0);
                int i;
                for (i = 0; i < halfdma; i++) {
                    stale_half[i] = silence;
                }
            }
        } else {
            // Low DMA (8-bit, channels 0-3): ports in 0x00+ range, counts in bytes
            count = (int) inportb(ch * 2 + 1);
            count += (int) inportb(ch * 2 + 1) << 8;
            // count is in bytes; halfdma is in bytes, so compare directly
            {
                volatile Uint8 *stale_half = isr_dma_buffer + (count < halfdma ? halfdma : 0);
                int i;
                for (i = 0; i < halfdma; i++) {
                    stale_half[i] = silence;
                }
            }
        }
    }

    inportb(isr_irq_ack_port);  // acknowledge the interrupt by reading this port. Makes the SB stop pulling the line.
    DOS_EndOfInterrupt();
}

// Called from the main loop (via DOSVESA_PumpEvents → SDL_DOS_PumpAudio) to
// run the audio mixing pipeline in normal (non-IRQ) context.
void SDL_DOS_PumpAudio(void)
{
    if (!opened_soundblaster_device) {
        return;
    }

    // Check-and-clear atomically w.r.t. the IRQ by bracketing with cli/sti.
    DOS_DisableInterrupts();
    const bool pending = soundblaster_irq_pending;
    soundblaster_irq_pending = false;
    DOS_EnableInterrupts();

    if (pending && audio_streams_locked == 0) {
        SDL_PlaybackAudioThreadIterate(opened_soundblaster_device);
        soundblaster_mix_count = soundblaster_irq_count;
    }
}

// this is sort of hacky, but we need to make sure the audio mixing doesn't
//  run while an audio stream is locked, since we don't have real mutexes.
// !!! FIXME: we should probably do this only for streams bound to an audio
// !!! FIXME: device, but good enough for now.
void SDL_DOS_LockAudioStream(SDL_AudioStream *stream)
{
    DOS_DisableInterrupts();
    audio_streams_locked++;
    DOS_EnableInterrupts();
}

void SDL_DOS_UnlockAudioStream(SDL_AudioStream *stream)
{
    DOS_DisableInterrupts();

    if (audio_streams_locked > 0) {
        --audio_streams_locked;
    }

    DOS_EnableInterrupts();
}

static bool DOSSOUNDBLASTER_OpenDevice(SDL_AudioDevice *device)
{
    const bool is_sb16 = soundblaster_is_sb16;

    if (is_sb16) {
        // SB16 (DSP >= 4): 16-bit stereo signed
        device->spec.format = SDL_AUDIO_S16LE;
        device->spec.channels = 2;
    } else {
        // Pre-SB16 (DSP < 4): 8-bit mono unsigned.
        // SB Pro (DSP 3.x) can do stereo but the effective sample rate halves;
        // SB 2.0 (DSP 2.x) and SB 1.x are mono-only. Keep mono for simplicity.
        device->spec.format = SDL_AUDIO_U8;
        device->spec.channels = 1;
    }

    // Accept whatever frequency SDL3's audio layer passes in. For SB16 (DSP >= 4)
    // the hardware supports 5000–44100 Hz via DSP command 0x41. For pre-SB16,
    // clamp to hardware limits:
    //   SB 1.x: max ~23 kHz mono
    //   SB 2.0 (DSP 2.x): max 44100 Hz mono (high-speed), ~23 kHz normal
    //   SB Pro (DSP 3.x): max 44100 Hz mono, 22050 Hz stereo
    // Since we use mono for all pre-SB16, 22050 Hz is safe for all models.
    if (!is_sb16 && device->spec.freq > 22050) {
        device->spec.freq = 22050;  // clamp to safe max for pre-SB16 mono
    }
    device->sample_frames = SDL_GetDefaultSampleFramesFromFreq(device->spec.freq);

    // Calculate the final parameters for this audio specification
    SDL_UpdatedAudioDeviceFormat(device);

    SDL_Log("SOUNDBLASTER: Opening at %d Hz, %d channels, format 0x%X, %d sample frames (DSP %d.%d, %s)",
            device->spec.freq, device->spec.channels, device->spec.format, device->sample_frames,
            soundblaster_version, soundblaster_version_minor, is_sb16 ? "SB16" : "pre-SB16");

    if (device->buffer_size > (32 * 1024)) {
        return SDL_SetError("Buffer size is too large (choose smaller audio format and/or less sample frames");  // DMA buffer has to fit in 64K segment, so buffer_size has to be half that, as we double it.
    }

    // Initialize all variables that we clean on shutdown
    struct SDL_PrivateAudioData *hidden = (struct SDL_PrivateAudioData *) SDL_calloc(1, sizeof(*device->hidden));
    if (!hidden) {
        return false;
    }

    device->hidden = hidden;
    hidden->is_16bit = is_sb16;

    ResetSoundBlasterDSP();

    // allocate conventional memory for the DMA buffer.
    hidden->dma_channel = is_sb16 ? soundblaster_highdma_channel : soundblaster_dma_channel;
    hidden->dma_buflen = device->buffer_size * 2;
    hidden->dma_buffer = (Uint8 *) DOS_AllocateDMAMemory(hidden->dma_buflen, &hidden->dma_seginfo);
    if (!hidden->dma_buffer) {
        return SDL_SetError("Couldn't allocate Sound Blaster DMA buffer!");
    }

    SDL_Log("SOUNDBLASTER: Allocated %d bytes of conventional memory at segment %d (ptr=%p)", (int) hidden->dma_buflen, (int) hidden->dma_seginfo.rm_segment, hidden->dma_buffer);

    // silence the DMA buffer to start
    SDL_memset(hidden->dma_buffer, soundblaster_silence_value, hidden->dma_buflen);

    // set up DMA controller.
    const Uint32 physical = DOS_LinearToPhysical(hidden->dma_buffer);
    const Uint8 physical_page = (physical >> 16) & 0xFF;

    if (is_sb16) {
        // High DMA (16-bit, channels 5-7): ports in 0xC0-0xDF range, counts in words.
        const int dma_words = (hidden->dma_buflen / 2) - 1;
        outportb(0xD4, 0x04 | hidden->dma_channel);  // mask the DMA channel
        outportb(0xD6, 0x58 | (hidden->dma_channel - 4));  // mode: single, read, auto-init
        outportb(0x8B, physical_page);  // page to transfer
        outportb(0xD8, 0x00);  // clear the flip-flop
        outportb(0xC0 + (hidden->dma_channel - 4) * 4, (Uint8) ((physical >> 1) & 0xFF));   // offset low (word address)
        outportb(0xC0 + (hidden->dma_channel - 4) * 4, (Uint8) ((physical >> 9) & 0xFF));   // offset high
        outportb(0xD8, 0x00);  // clear the flip-flop
        outportb(0xC0 + (hidden->dma_channel - 4) * 4 + 2, (Uint8) (dma_words & 0xFF));     // count low
        outportb(0xC0 + (hidden->dma_channel - 4) * 4 + 2, (Uint8) ((dma_words >> 8) & 0xFF));  // count high
        outportb(0xD4, hidden->dma_channel & ~4);  // unmask the DMA channel
    } else {
        // Low DMA (8-bit, channels 0-3): ports in 0x00-0x0F range, counts in bytes.
        static const int page_ports[] = { 0x87, 0x83, 0x81, 0x82 };  // DMA page register ports for channels 0-3 (yes, they're out of order — that's how the IBM PC DMA controller works)
        const int dma_bytes = hidden->dma_buflen - 1;
        outportb(0x0A, 0x04 | hidden->dma_channel);  // mask the DMA channel
        outportb(0x0B, 0x58 | hidden->dma_channel);  // mode: single, read, auto-init
        outportb(page_ports[hidden->dma_channel], physical_page);  // page to transfer
        outportb(0x0C, 0x00);  // clear the flip-flop
        outportb(hidden->dma_channel * 2, (Uint8) (physical & 0xFF));         // offset low (byte address)
        outportb(hidden->dma_channel * 2, (Uint8) ((physical >> 8) & 0xFF));  // offset high
        outportb(0x0C, 0x00);  // clear the flip-flop
        outportb(hidden->dma_channel * 2 + 1, (Uint8) (dma_bytes & 0xFF));          // count low
        outportb(hidden->dma_channel * 2 + 1, (Uint8) ((dma_bytes >> 8) & 0xFF));   // count high
        outportb(0x0A, hidden->dma_channel);  // unmask the DMA channel (just the channel number, no bit 2)
    }

    soundblaster_irq_pending = false;
    soundblaster_irq_count = 0;
    soundblaster_mix_count = 0;

    // Cache DMA parameters in globals so the IRQ handler can access them
    // without chasing pointers through potentially-unlocked heap memory.
    isr_dma_buffer = hidden->dma_buffer;
    isr_dma_halfdma = hidden->dma_buflen / 2;
    isr_dma_channel = hidden->dma_channel;
    isr_irq_ack_port = soundblaster_base_port + (is_sb16 ? 0x0F : 0x0E);
    isr_silence_value = soundblaster_silence_value;
    isr_is_16bit = is_sb16;

    // Lock ISR code and data to prevent page faults during interrupts.
    // The IRQ handler only touches these globals and the DMA buffer (which is
    // in conventional memory and always physically mapped).
    _go32_dpmi_lock_code((void *)SoundBlasterIRQHandler,
        (unsigned long)SDL_DOS_PumpAudio - (unsigned long)SoundBlasterIRQHandler);
    _go32_dpmi_lock_data((void *)&soundblaster_irq_pending, sizeof(soundblaster_irq_pending));
    _go32_dpmi_lock_data((void *)&soundblaster_irq_count, sizeof(soundblaster_irq_count));
    _go32_dpmi_lock_data((void *)&soundblaster_mix_count, sizeof(soundblaster_mix_count));
    _go32_dpmi_lock_data((void *)&isr_dma_buffer, sizeof(isr_dma_buffer));
    _go32_dpmi_lock_data((void *)&isr_dma_halfdma, sizeof(isr_dma_halfdma));
    _go32_dpmi_lock_data((void *)&isr_dma_channel, sizeof(isr_dma_channel));
    _go32_dpmi_lock_data((void *)&isr_irq_ack_port, sizeof(isr_irq_ack_port));
    _go32_dpmi_lock_data((void *)&isr_silence_value, sizeof(isr_silence_value));
    _go32_dpmi_lock_data((void *)&isr_is_16bit, sizeof(isr_is_16bit));
    _go32_dpmi_lock_data((void *)&soundblaster_base_port, sizeof(soundblaster_base_port));

    DOS_HookInterrupt(soundblaster_irq, SoundBlasterIRQHandler, &hidden->interrupt_hook);

    WriteSoundBlasterDSP(0xD1);  // turn on the speaker
    // !!! FIXME: can we query (soundblaster_base_port + 0xC) to see if this is done faster?
    SDL_Delay(112);  // takes a maximum of 112 milliseconds to complete this command! Gosh!

    if (is_sb16) {
        // SB16 (DSP >= 4): set output sample rate directly
        WriteSoundBlasterDSP(0x41);  // set output sampling rate
        WriteSoundBlasterDSP((Uint8) (device->spec.freq >> 8));
        WriteSoundBlasterDSP((Uint8) (device->spec.freq & 0xFF));

        // start 16-bit auto-initialize DMA mode
        // half the total buffer per transfer, then convert to samples (divide by 2 because they are 16-bits each).
        const int block_size = ((hidden->dma_buflen / 2) / sizeof (Sint16)) - 1;  // one less than samples to be transferred.
        WriteSoundBlasterDSP(0xB6);  // 16-bit output, auto-init, FIFO on
        WriteSoundBlasterDSP(0x30);  // 16-bit stereo signed PCM
        WriteSoundBlasterDSP((Uint8) (block_size & 0xFF));
        WriteSoundBlasterDSP((Uint8) (block_size >> 8));
    } else {
        // Pre-SB16 (DSP < 4): set sample rate via Time Constant
        // Time Constant = 256 - (1000000 / freq)
        // For 22050 Hz mono: TC = 256 - 45 = 211 (0xD3)
        const Uint8 time_constant = (Uint8)(256 - (1000000 / device->spec.freq));
        WriteSoundBlasterDSP(0x40);  // set time constant
        WriteSoundBlasterDSP(time_constant);

        // start 8-bit auto-initialize DMA mode
        // block_size is in bytes for 8-bit, and it's the half-buffer size minus 1
        const int block_size = (hidden->dma_buflen / 2) - 1;
        WriteSoundBlasterDSP(0x48);  // set DSP block transfer size
        WriteSoundBlasterDSP((Uint8) (block_size & 0xFF));
        WriteSoundBlasterDSP((Uint8) (block_size >> 8));
        // NOTE: DSP 1.x does not support auto-init (0x1C). Those cards are extremely
        // rare and would need single-cycle transfers re-triggered from the ISR.
        // For now we use 0x1C anyway and hope for the best on DSP 1.x hardware.
        WriteSoundBlasterDSP(0x1C);  // 8-bit auto-init DMA playback
    }

    opened_soundblaster_device = device;

    SDL_Log("SoundBlaster opened!");
    return true;
}

static Uint8 *DOSSOUNDBLASTER_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    struct SDL_PrivateAudioData *hidden = device->hidden;
    SDL_assert(*buffer_size == (hidden->dma_buflen / 2));
    const int halfdma = *buffer_size;

    int count;
    if (hidden->is_16bit) {
        // High DMA (16-bit, channels 5-7): ports in 0xC0+ range, counts in 16-bit words
        count = (int) inportb(0xC0 + (hidden->dma_channel - 4) * 4 + 2);
        count += (int) inportb(0xC0 + (hidden->dma_channel - 4) * 4 + 2) << 8;
        // count is in words; halfdma is in bytes, so compare against halfdma/2
        return hidden->dma_buffer + (count < (halfdma / 2) ? 0 : halfdma);
    } else {
        // Low DMA (8-bit, channels 0-3): ports in 0x00+ range, counts in bytes
        count = (int) inportb(hidden->dma_channel * 2 + 1);
        count += (int) inportb(hidden->dma_channel * 2 + 1) << 8;
        // count is in bytes; halfdma is in bytes, so compare directly
        return hidden->dma_buffer + (count < halfdma ? 0 : halfdma);
    }
}

static void DOSSOUNDBLASTER_CloseDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *hidden = device->hidden;
    if (hidden) {
        // Disable PCM.
        if (hidden->is_16bit) {
            WriteSoundBlasterDSP(0xDA);  // exit 16-bit auto-init DMA
            WriteSoundBlasterDSP(0xD3);  // turn off the speaker
        } else {
            WriteSoundBlasterDSP(0xD0);  // halt 8-bit DMA
            WriteSoundBlasterDSP(0xDA);  // exit auto-init DMA
            WriteSoundBlasterDSP(0xD3);  // turn off the speaker
        }

        DOS_UnhookInterrupt(&hidden->interrupt_hook, true);

        // disable DMA — mask the appropriate DMA channel.
        if (hidden->dma_buffer) {
            if (hidden->is_16bit) {
                outportb(0xD4, 0x04 | hidden->dma_channel);  // mask high DMA channel (channels 5-7)
            } else {
                outportb(0x0A, 0x04 | hidden->dma_channel);  // mask low DMA channel (channels 0-3)
            }
            DOS_FreeConventionalMemory(&hidden->dma_seginfo);
        }

        soundblaster_irq_pending = false;
        soundblaster_irq_count = 0;
        soundblaster_mix_count = 0;
        isr_dma_buffer = NULL;
        isr_dma_halfdma = 0;
        isr_dma_channel = 0;
        isr_irq_ack_port = 0;
        isr_silence_value = 0;
        isr_is_16bit = false;
        opened_soundblaster_device = NULL;

        SDL_free(hidden);
    }
}

static bool CheckForSoundBlaster(void)
{
    ResetSoundBlasterDSP();

    // wait for the DSP to say it's ready.
    bool ready = false;
    for (int i = 0; i < 300; i++) { // may take up to 100msecs to initialize. We'll give it 300.
        SDL_DelayPrecise(1000);
        if (ReadSoundBlasterReady()) {
            ready = true;
            break;
        }
    }

    if (!ready) {
        return SDL_SetError("No SoundBlaster detected on port 0x%X", soundblaster_base_port);  // either no SoundBlaster or it's on a different base port.
    } else if (ReadSoundBlasterDSP() != 0xAA) {
        return SDL_SetError("Not a SoundBlaster at port 0x%X\n", soundblaster_base_port);  // either it's not a SoundBlaster or there's a problem.
    }
    return true;
}

static bool IsSoundBlasterPresent(void)
{
    const char *env = SDL_getenv("BLASTER");
    if (!env) {
        return SDL_SetError("No BLASTER environment variable to find Sound Blaster");   // definitely doesn't have a Sound Blaster (or they screwed up).
    }

    char *copy = SDL_strdup(env);
    if (!copy) {
        return false;  // oh well.
    }

    char *str = copy;
    char *saveptr = NULL;

    char *token;
    while ((token = SDL_strtok_r(str, " ", &saveptr)) != NULL) {
        str = NULL;  // must be NULL for future calls to tokenize the same string.
        char *endp = NULL;
        const int num = (int) SDL_strtol(token+1, &endp, 16);
        if ((token[1] == 0) || (*endp != 0)) {  // bogus num
            continue;
        } else if (num < 0) {
            continue;
        }

        switch (SDL_toupper(*token)) {
            case 'A':  // Base i/o port (in hex)
                soundblaster_base_port = num;
                break;

            case 'I':  // IRQ
                soundblaster_irq = num;
                break;

            case 'D':  // DMA channel
                soundblaster_dma_channel = num;
                break;

            case 'H':  // High DMA channel
                soundblaster_highdma_channel = num;
                break;

            // don't care about these.
            //case 'M':  // mixer chip base port
            //case 'P':  // MPU-401 base port
            //case 'T':  // type of device
            //case 'E':  // EMU8000 base port: an AWE32 thing
            default: break;
        }
    }
    SDL_free(copy);

    if (!soundblaster_base_port || !soundblaster_irq || (!soundblaster_dma_channel && !soundblaster_highdma_channel)) {
        return SDL_SetError("BLASTER environment variable is incomplete or incorrect");
    } else if (!CheckForSoundBlaster()) {
        return false;
    }

    WriteSoundBlasterDSP(0xE1);  // query DSP version
    soundblaster_version = (int) ReadSoundBlasterDSP();
    soundblaster_version_minor = (int) ReadSoundBlasterDSP();

    SDL_Log("SB: BLASTER env='%s'", env);
    SDL_Log("SB: port=0x%X", soundblaster_base_port);
    SDL_Log("SB: irq=%d", soundblaster_irq);
    SDL_Log("SB: dma8=%d", soundblaster_dma_channel);
    SDL_Log("SB: dma16=%d", soundblaster_highdma_channel);
    SDL_Log("SB: version=%d.%d", soundblaster_version, soundblaster_version_minor);

    soundblaster_is_sb16 = !FORCE_SB_8BIT && (soundblaster_version >= 4);
    soundblaster_silence_value = soundblaster_is_sb16 ? 0x00 : 0x80;  // S16LE silence is 0x00, U8 silence is 0x80

    return true;
}

static bool DOSSOUNDBLASTER_Init(SDL_AudioDriverImpl *impl)
{
    if (!IsSoundBlasterPresent()) {
        return false;
    }

    impl->OpenDevice = DOSSOUNDBLASTER_OpenDevice;
    impl->GetDeviceBuf = DOSSOUNDBLASTER_GetDeviceBuf;
    impl->CloseDevice = DOSSOUNDBLASTER_CloseDevice;

    // !!! FIXME: maybe later
    //impl->WaitRecordingDevice = DOSSOUNDBLASTER_WaitDevice;
    //impl->RecordDevice = DOSSOUNDBLASTER_RecordDevice;
    //impl->FlushRecording = DOSSOUNDBLASTER_FlushRecording;
    //impl->HasRecordingSupport = true;
    //impl->OnlyHasDefaultRecordingDevice = true;

    impl->ProvidesOwnCallbackThread = true;  // hardware interrupts!
    impl->OnlyHasDefaultPlaybackDevice = true;

    return true;
}

AudioBootStrap DOSSOUNDBLASTER_bootstrap = {
    "soundblaster", "Sound Blaster", DOSSOUNDBLASTER_Init, false, false
};

#endif // SDL_AUDIO_DRIVER_OSS
