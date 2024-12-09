#include <MozziConfigValues.h>
#define MOZZI_AUDIO_INPUT MOZZI_AUDIO_INPUT_STANDARD
#define MOZZI_AUDIO_INPUT_PIN 0
#define MOZZI_AUDIO_MODE MOZZI_OUTPUT_2PIN_PWM

#include <Mozzi.h>
#include <RollingAverage.h>

#include <LowPassFilter.h>

#define LENGTH_PIN 1
#define FB_PIN 2
#define LOFI_NOFI_PIN 3
#define LED_PIN 4

int freq_input;
RollingAverage<int, 8> kAverageFreq;
int averaged_freq;
int feedback;
RollingAverage<int, 8> kAverageFeedBack;
int averaged_feedback;

int adpcm = 0;
int adpcm_out = 0;

int asig;

char buffer[512];
int buffer_length = 512;

int buff_index = 0;
int buff_value_select = 0;

float buff_index_fl = 0.0f;
float buff_add = 1.0f;

int real_out;

bool has_looped_once = false;

int sample_rate_reducer = 0;
int sample_rate_divider = 0;
int previous_out;

bool nofi_mode = true;

bool paused = false;

int lofi_length = 1;
int lofi_index = 0;
int lofi_sample_rate_reducer = 0;
int lofi_delay_output = 0;
int lofi_led_counter = 0;

void setup() {
  pinMode(LENGTH_PIN, INPUT);
  pinMode(FB_PIN, INPUT);
  pinMode(LOFI_NOFI_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);

  startMozzi();  // uses the default control rate of 64

  for (int i = 0; i < buffer_length; i++) {
    buffer[i] = 0;
  }
}

void updateControl() {
  int length_input = mozziAnalogRead<10>(LENGTH_PIN);  // request reading at 10-bit resolution, i.e. 0-1023
  freq_input = 1023 - length_input;
  lofi_length = (length_input / 3 ) + 169;
  if (lofi_length > 511) {
    lofi_length = 511;
  }
  averaged_freq = kAverageFreq.next(freq_input);
  int prev_div = sample_rate_divider;
  sample_rate_divider = averaged_freq / 10;
  sample_rate_divider += 10;
  if (sample_rate_divider > 100) {
    sample_rate_divider = 100;
  }

  feedback = mozziAnalogRead<8>(FB_PIN);  // request reading at 10-bit resolution, i.e. 0-1023
  averaged_feedback = kAverageFeedBack.next(feedback);

  bool previous_mode = nofi_mode;
  nofi_mode = digitalRead(LOFI_NOFI_PIN);

  if (previous_mode != nofi_mode) {
    paused = true;
    for (int i = 0; i < buffer_length; i++) {
      buffer[i] = 0;
    }
    buff_index = 0;
    sample_rate_reducer = 0;
    has_looped_once = false;
    previous_out = 0;
    real_out = 0;
    adpcm_out = 0;
    paused = false;
  }

}

AudioOutput updateAudio() {
  if (paused) {
    return MonoOutput::from8Bit(0);
  } else {
    int audio_raw = getAudioInput();
    if (nofi_mode) {
      if (sample_rate_reducer >= 100) {
        asig = audio_raw;  // range 0-1023
        asig = asig - 512;       // now range is -512 to 511
        if (asig < 2 && asig > -2) {
          asig = 0;
        }
        asig = (asig / 2 + previous_out);

        if (asig > 240) {
          asig = 240;
        } else if (asig < -240) {
          asig = -240;
        }

        if (asig > adpcm + 7) {
          buffer[buff_index] = (buffer[buff_index] & ~(0b00000011 << (buff_value_select * 2))) | (0b00000011 << (buff_value_select * 2));
          adpcm += 8;
        } else if (asig >= adpcm) {
          buffer[buff_index] = (buffer[buff_index] & ~(0b00000011 << (buff_value_select * 2))) | (0b00000010 << (buff_value_select * 2));
          adpcm += 2;
        } else if (asig < adpcm - 7) {
          buffer[buff_index] = (buffer[buff_index] & ~(0b00000011 << (buff_value_select * 2))); // | (0b00000000 << (buff_value_select * 2));
          adpcm -= 8;
        } else {
          buffer[buff_index] = (buffer[buff_index] & ~(0b00000011 << (buff_value_select * 2))) | (0b00000001 << (buff_value_select * 2));
          // adpcm -= 0;
        }

        int read_index = buff_index + 1;
        if (read_index >= buffer_length) {
          read_index = 0;
        }

        int read_data = buffer[read_index] >> (buff_value_select * 2) & 0b00000011;

        if (read_data == 0b00000011) {
          adpcm_out += 8;
        } else if (read_data == 0b00000010) {
          adpcm_out += 2;
        } else if (read_data == 0b00000000) {
          adpcm_out -= 8;
        } else {
          adpcm_out -= 0;
        }

        if (adpcm > 255) {
          adpcm = 255;
        } else if (adpcm < -255) {
          adpcm = -255;
        }

        if (adpcm_out > 250) {
          adpcm_out = 250;
        } else if (adpcm_out < -250) {
          adpcm_out = -250;
        }

        if (!has_looped_once) {
          adpcm_out = 0;
        }
        
        if (buff_value_select == 0) {
          digitalWrite(LED_PIN, HIGH);
        } else {
          digitalWrite(LED_PIN, LOW);
        }

        buff_index += 1;
        if (buff_index >= buffer_length) {
          buff_index = 0;
          buff_value_select += 1;
          if (buff_value_select > 3) {
            buff_value_select = 0;
            has_looped_once = true;
          }
        }

        real_out = adpcm_out / 2;

        previous_out = (adpcm_out * averaged_feedback) / 256;

        sample_rate_reducer -= 100;
      }
      
      sample_rate_reducer += sample_rate_divider;

      return MonoOutput::from8Bit(real_out);
    } else {
      if (lofi_sample_rate_reducer == 0) {
        int read_index = lofi_index - lofi_length;
        if (read_index < 0) {
          read_index = buffer_length - 1 + read_index;
        }

        lofi_delay_output = buffer[read_index];

        asig = audio_raw;  // range 0-1023
        asig = asig - 512; // now range is -512 to 511
        asig = asig / 6;
        asig = asig + previous_out;

        if (asig > 100) {
          asig = 100 + ((asig - 100) / 2);
        } else if (asig < -100) {
          asig = -100 + ((asig + 100) / 2);
        }

        if (asig > 125) {
          asig = 125;
        } else if (asig < -125) {
          asig = -125;
        }

        buffer[lofi_index] = asig;

        lofi_index++;
        if (lofi_index >= buffer_length) {
          lofi_index = 0;
        }

        lofi_led_counter++;
        if (lofi_led_counter < lofi_length / 4) {
          digitalWrite(LED_PIN, HIGH);
        } else if (lofi_led_counter >= lofi_length) {
          lofi_led_counter = 0;
          digitalWrite(LED_PIN, HIGH);
        } else {
          digitalWrite(LED_PIN, LOW);
        }

        lofi_sample_rate_reducer++;

        previous_out = (lofi_delay_output * averaged_feedback) / 256;
      } else {
        lofi_sample_rate_reducer++;
        if (lofi_sample_rate_reducer >= 5) {
          lofi_sample_rate_reducer = 0;
        }
      }

      return MonoOutput::from8Bit(lofi_delay_output);
    }
  }
  
}

void loop() {
  audioHook();  // required here
}