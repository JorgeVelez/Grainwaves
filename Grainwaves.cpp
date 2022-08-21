#include "daisy_patch_sm.h"
#include "daisysp.h"
#include "core_cm7.h"
#include "Daisy_SSD1327/Daisy_SSD1327.h"

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;

#define RECORDING_XFADE_OVERLAP 100 // Samples
#define RECORDING_BUFFER_SIZE (48000 * 5) // X seconds at 48kHz
#define MIN_GRAIN_SIZE 480 // 10 ms
#define MAX_GRAIN_SIZE (48000 * 2) // 2 second
#define MAX_GRAIN_COUNT 64

struct Grain {
    size_t length = 0;
    int start_offset = 0;
    size_t step = 0;
    float pan = 0; // 0 is left, 1 is right
    float playback_speed = 0;
};

DaisyPatchSM patch;
CpuLoadMeter cpu_load_meter;
Switch       record_button;
Switch       shift_button;

uint8_t DMA_BUFFER_MEM_SECTION oled_buffer[SSD1327_REQUIRED_DMA_BUFFER_SIZE];
Daisy_SSD1327 oled;
SpiHandle spi;
dsy_gpio dc_pin;
I2CHandle i2c;

float DSY_SDRAM_BSS recording[RECORDING_BUFFER_SIZE];
size_t recording_length = 0;
size_t write_head = 0;

const size_t RENDERABLE_RECORDING_BUFFER_SIZE = oled.width;
const float RECORDING_TO_RENDERABLE_RECORDING_BUFFER_RATIO = RENDERABLE_RECORDING_BUFFER_SIZE / (float)RECORDING_BUFFER_SIZE;
const float RENDERABLE_RECORDING_TO_RECORDING_BUFFER_RATIO = RECORDING_BUFFER_SIZE / (float)RENDERABLE_RECORDING_BUFFER_SIZE;
float DSY_SDRAM_BSS renderable_recording[RENDERABLE_RECORDING_BUFFER_SIZE]; // Much lower resolution, for easy rendering
size_t last_written_renderable_recording_index = 0; 
size_t max_written_renderable_recording_index = 0; 

bool is_recording = false;

size_t grain_length = 48000 / 5; 
float grain_start_offset = 0.f;
unsigned int spawn_rate = 48000 / 3; // samples
float spawn_rate_spread;
float next_spawn_offset;
float pan_spread;
uint32_t last_spawn_time = 0;
uint32_t samples_seen = 0;
Grain grains[MAX_GRAIN_COUNT];
Stack<uint8_t, MAX_GRAIN_COUNT> available_grains;
uint32_t cycles_used = 0;

float fwrap(float x, float min, float max) {
    if (max == min) return min;
    if (min > max) return fwrap(x, max, min);

    return (x >= 0 ? min : max) + fmodf(x, max - min);
}

int wrap(int x, int min, int max) {
    if (max == min) return min;
    if (min > max) return wrap(x, max, min);

    return (x >= 0 ? min : max) + x % (max - min);
}

float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

float randF(float min, float max) {
    return min + rand() * kRandFrac * (max - min);
}

float map_to_range(float fraction, float min, float max) {
    return min + fraction * (max - min);
}

float modf(float x) {
    static float junk;
    return modf(x, &junk);
}

// Responsible for wrapping the index
// and xfading the start and end of the track 
// to get rid of the pop when transitioning from 
// the last to the first sample of the recording
float getSample(int index) {
    index = wrap(index, 0, recording_length);

    // TODO: The pop-zone has moved. It now follows the record_head. This should too
    if (index < RECORDING_XFADE_OVERLAP) {
        float xfade_magnitude = 1 - (index + 1) / ((float)RECORDING_XFADE_OVERLAP + 1.f);

        return lerp(
            recording[index], 
            recording[recording_length - 1], 
            xfade_magnitude
        );
    } else {
        return recording[index];
    }
}

void AudioCallback(
    AudioHandle::InputBuffer  in,
    AudioHandle::OutputBuffer out,
    size_t size
) {
    cpu_load_meter.OnBlockStart();

    patch.ProcessAllControls();
    record_button.Debounce();
    shift_button.Debounce();
    
    spawn_rate = map_to_range(1 - log10f(1 + patch.GetAdcValue(CV_1) * 9), MIN_GRAIN_SIZE, MAX_GRAIN_SIZE / 4);
    grain_length = map_to_range(patch.GetAdcValue(CV_2), MIN_GRAIN_SIZE, MAX_GRAIN_SIZE);
    float scan_speed = map_to_range(patch.GetAdcValue(CV_3), -2, 2);
    float grain_spread = patch.GetAdcValue(CV_4);
    pan_spread = patch.GetAdcValue(CV_5);
    float spawn_rate_spread = patch.GetAdcValue(CV_6);
    float modified_spawn_rate = spawn_rate * (1 + next_spawn_offset * spawn_rate_spread);
    // float pitch_shift_in_semitones = map_to_range(patch.GetAdcValue(CV_7), -12 * 5, 12 * 5); // volt per octave
    float pitch_shift_in_semitones = (int)map_to_range(patch.GetAdcValue(CV_7), -12, 12); // Without CV this is more playable
    float pitch_shift_spread = patch.GetAdcValue(CV_8);

    // Toggle the record state on button press
    if(record_button.RisingEdge())
    {
        if (!is_recording) {
            is_recording = true;
            recording_length = 0;
            write_head = 0;
            last_written_renderable_recording_index = 0;
            max_written_renderable_recording_index = 0;
            memset(renderable_recording, 0, sizeof(renderable_recording));
        } else {
            is_recording = false;
        }
    }

    patch.SetLed(is_recording || shift_button.Pressed()); 

    // Process audio
    for(size_t i = 0; i < size; i++)
    {
        samples_seen++;

        if (is_recording) {
            recording[write_head] = IN_L[i];

            // TODO: Record positive and negative values seperately
            size_t renderable_recording_index = write_head * RECORDING_TO_RENDERABLE_RECORDING_BUFFER_RATIO;

            // Clear out the element when we first start writing fresh values to it
            if (write_head == 0 || renderable_recording_index > last_written_renderable_recording_index) {
                renderable_recording[renderable_recording_index] = 0;
            }

            // Downsample the samples into renderable_recording_index by averaging them
            renderable_recording[renderable_recording_index] += abs(IN_L[i]) / RENDERABLE_RECORDING_TO_RECORDING_BUFFER_RATIO;
            last_written_renderable_recording_index = renderable_recording_index;
            max_written_renderable_recording_index = std::max(max_written_renderable_recording_index, last_written_renderable_recording_index);
            
            if (recording_length < RECORDING_BUFFER_SIZE) {
                recording_length++;
            }
            
            write_head++;
            if (write_head >= RECORDING_BUFFER_SIZE) {
                write_head = 0;
            }
        } 

        if (recording_length > 4800 /* A kinda abitrary number */) {
            // Spawn grains
            // TODO: This will break when samples_seen wraps
            if (samples_seen - last_spawn_time >= modified_spawn_rate && !available_grains.IsEmpty()) {
                size_t next_grain_to_spawn = available_grains.PopBack();

                last_spawn_time = samples_seen;

                grains[next_grain_to_spawn].length = grain_length;
                grains[next_grain_to_spawn].start_offset = grain_start_offset + grain_spread * randF(-0.5f, 0.5f) * recording_length;
                grains[next_grain_to_spawn].step = 0;
                grains[next_grain_to_spawn].pan = 0.5f + randF(-0.5f, 0.5f) * pan_spread;

                float pitch_shift_offset_in_semitones = randF(-2.f, 2.f) * pitch_shift_spread;
                float pitch_shift_in_octaves = (pitch_shift_in_semitones + pitch_shift_offset_in_semitones) / 12.f;
                float playback_speed = pow(2, pitch_shift_in_octaves);
                grains[next_grain_to_spawn].playback_speed = playback_speed;

                next_spawn_offset = randF(-1.f, 1.f);
            }

            // Calculate output
            float wet_l = 0.f;
            float wet_r = 0.f;

            for (int j = 0; j < MAX_GRAIN_COUNT; j++) {
                if (grains[j].step <= grains[j].length) {
                    size_t buffer_index = grains[j].start_offset + grains[j].step * grains[j].playback_speed;

                    // playback_speed is a float so we need to interpolate between samples
                    float sample = getSample(buffer_index);
                    float next_sample = getSample(buffer_index + 1);

                    float decimal_portion = modf(grains[j].step * grains[j].playback_speed);
                    float interpolated_sample = sample * (1 - decimal_portion) + next_sample * decimal_portion;

                    // hacky bad envelope
                    float envelope_mult = std::min((grains[j].length - grains[j].step), grains[j].step);
                    // TODO: Get rid of this 0.75f and balance the output properly
                    float signal = interpolated_sample * envelope_mult / grains[j].length * 0.75f;
                    wet_l += (1.f - grains[j].pan) * signal;
                    wet_r += grains[j].pan * signal;

                    grains[j].step++;

                    if (grains[j].step > grains[j].length) {
                        available_grains.PushBack(j);
                    }
                }
            }

            OUT_L[i] = wet_l + IN_L[i];
            OUT_R[i] = wet_r + IN_L[i];
        } else {
            OUT_L[i] = IN_L[i];
            OUT_R[i] = IN_L[i];
        }
    }

    grain_start_offset = fwrap(grain_start_offset + scan_speed * size, 0.f, recording_length);

    cpu_load_meter.OnBlockEnd();
}

uint32_t last_render_millis = 0;
uint32_t last_debug_print_millis = 0;

int main(void)
{
    patch.Init();
    record_button.Init(patch.B7);
    shift_button.Init(patch.B8);
    patch.StartLog();

    spi.Init(
        oled.getSpiConfig(
            patch.D10, /* sclk */
            patch.D9, /* mosi */
            patch.D8, /* miso */
            patch.D1 /* nss */
        )
    );

    oled.init(spi, patch.A9, oled_buffer, patch);
    oled.clear(0x5);
    oled.display();

    // Populate available grains stack
    for (u_int8_t i = 0; i < MAX_GRAIN_COUNT; i++) {
        available_grains.PushBack(i);
    }

    cpu_load_meter.Init(patch.AudioSampleRate(), patch.AudioBlockSize());
    patch.StartAudio(AudioCallback);

    while(1) {
        if (System::GetNow() - last_render_millis > 8 && !oled.isRendering()) {
            last_render_millis = System::GetNow();

            if (recording_length == 0) {
                oled.clear(0x2);
            } else {
                oled.clear(SSD1327_BLACK);
            }

            // Recording Waveform
            uint8_t last_amplitude = 0;
            // Traversing backwards stops the leading wave of recording
            // affecting values infront of it due to how the smoothing filter works
            for (int x = oled.width - 1; x >= 0; x--) {
                size_t renderable_recording_index = (x / (float)oled.width) * max_written_renderable_recording_index;

                uint8_t amplitude = std::min(128.f, renderable_recording[renderable_recording_index] / 0.1f * oled.height);

                // Smooth out the waveform
                // TODO: Smooth differently, this produces weird classic LPF shapes
                if (x > 0) {
                    amplitude = amplitude * 0.4 + last_amplitude * 0.6;
                }
                last_amplitude = amplitude;

                uint8_t margin = (oled.height - amplitude) / 2;

                for (uint8_t y = margin; y < oled.width - margin; y++) {
                    oled.setPixel(x, y, 0x1);
                }
            }

            // Grain start offset
            float grain_start_offset_x = grain_start_offset / (float)recording_length * oled.width;
            float grain_start_offset_x_decimal_part = modf(grain_start_offset_x);
            uint8_t y_margin = (oled.height - pan_spread * oled.height) / 2;

            for (uint8_t y = y_margin; y < oled.height - y_margin; y++) {
                oled.setPixel(grain_start_offset_x, y, map_to_range(1 - grain_start_offset_x_decimal_part, 0x2, 0x6));
                oled.setPixel(wrap(grain_start_offset_x + 1, 0, oled.width), y, map_to_range(grain_start_offset_x_decimal_part, 0x2, 0x6));
            }

            // Grains
            for (int j = 0; j < MAX_GRAIN_COUNT; j++) {
                Grain grain = grains[j];

                if (grain.step <= grain.length) {
                    uint8_t y = grain.pan * oled.height;
                    uint32_t current_offset = wrap(grain.start_offset + grain.step * grains[j].playback_speed, 0, recording_length);
                    float x = (current_offset / (float)recording_length) * oled.width;

                    float amplitude = std::min((grains[j].length - grains[j].step), grains[j].step) / (float)grains[j].length;  

                    oled.setPixel(x, y, 0xF * amplitude);
                }
            }

            oled.display();
        }

        if (System::GetNow() - last_debug_print_millis > 250) {
            last_debug_print_millis = System::GetNow();

            // Note, this ignores any work done in this loop, eg running the OLED
            // patch.PrintLine("cpu Max:x " FLT_FMT3 "\tAvg:" FLT_FMT3, FLT_VAR3(cpu_load_meter.GetMaxCpuLoad()), FLT_VAR3(cpu_load_meter.GetAvgCpuLoad()));
            // patch.PrintLine(FLT_FMT3, FLT_VAR3(round(map_to_range(patch.GetAdcValue(CV_7), -12, 12)) / 12));
            patch.PrintLine(FLT_FMT3, FLT_VAR3(
                1 - log10f(1 + patch.GetAdcValue(CV_1) * 9)
            ));
            // patch.PrintLine("%d", last_written_renderable_recording_index);
        }
    }
} 
 