#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#include <BluetoothA2DPSource.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

namespace {
constexpr uint8_t BUTTON_1_PIN = 25;
constexpr uint8_t BUTTON_2_PIN = 26;
constexpr uint8_t BT_STATUS_LED_PIN = 2;
constexpr uint8_t SD_CS_PIN = 5;

constexpr uint32_t DEBOUNCE_MS = 40;
constexpr uint32_t LOOP_DELAY_MS = 5;
constexpr size_t PCM_BUFFER_BYTES = 16 * 1024;

const char *kTrackButton1 = "/button1.mp3";
const char *kTrackButton2 = "/button2.mp3";

BluetoothA2DPSource a2dpSource;
AudioGeneratorMP3 mp3Decoder;
AudioFileSourceSD *currentFile = nullptr;

SemaphoreHandle_t pcmMutex;
uint8_t pcmBuffer[PCM_BUFFER_BYTES];
size_t pcmReadIndex = 0;
size_t pcmWriteIndex = 0;
size_t pcmUsed = 0;

bool queuedPlayTrack1 = false;
bool queuedPlayTrack2 = false;

struct ButtonState {
  bool lastRaw = HIGH;
  bool stable = HIGH;
  uint32_t changedAtMs = 0;
};

ButtonState button1;
ButtonState button2;

class BufferedAudioOutput final : public AudioOutput {
 public:
  bool begin() override { return true; }

  bool ConsumeSample(int16_t sample[2]) override {
    if (pcmMutex == nullptr) {
      return false;
    }

    const uint8_t frame[4] = {
        static_cast<uint8_t>(sample[0] & 0xFF),
        static_cast<uint8_t>((sample[0] >> 8) & 0xFF),
        static_cast<uint8_t>(sample[1] & 0xFF),
        static_cast<uint8_t>((sample[1] >> 8) & 0xFF),
    };

    xSemaphoreTake(pcmMutex, portMAX_DELAY);
    if (PCM_BUFFER_BYTES - pcmUsed < sizeof(frame)) {
      xSemaphoreGive(pcmMutex);
      return false;
    }

    for (size_t i = 0; i < sizeof(frame); ++i) {
      pcmBuffer[pcmWriteIndex] = frame[i];
      pcmWriteIndex = (pcmWriteIndex + 1) % PCM_BUFFER_BYTES;
    }
    pcmUsed += sizeof(frame);
    xSemaphoreGive(pcmMutex);
    return true;
  }

  bool stop() override { return true; }
};

BufferedAudioOutput bufferedOutput;

void resetPcmBuffer() {
  if (pcmMutex == nullptr) {
    return;
  }

  xSemaphoreTake(pcmMutex, portMAX_DELAY);
  pcmReadIndex = 0;
  pcmWriteIndex = 0;
  pcmUsed = 0;
  xSemaphoreGive(pcmMutex);
}

int32_t btDataCallback(uint8_t *data, int32_t length) {
  if (pcmMutex == nullptr) {
    memset(data, 0, static_cast<size_t>(length));
    return length;
  }

  xSemaphoreTake(pcmMutex, portMAX_DELAY);

  const size_t requested = static_cast<size_t>(length);
  const size_t available = pcmUsed;
  const size_t toCopy = (available < requested) ? available : requested;

  for (size_t i = 0; i < toCopy; ++i) {
    data[i] = pcmBuffer[pcmReadIndex];
    pcmReadIndex = (pcmReadIndex + 1) % PCM_BUFFER_BYTES;
  }

  pcmUsed -= toCopy;
  xSemaphoreGive(pcmMutex);

  if (toCopy < requested) {
    memset(data + toCopy, 0, requested - toCopy);
  }

  return length;
}

void stopTrack() {
  if (mp3Decoder.isRunning()) {
    mp3Decoder.stop();
  }
  if (currentFile != nullptr) {
    currentFile->close();
    delete currentFile;
    currentFile = nullptr;
  }
  resetPcmBuffer();
}

bool startTrack(const char *path) {
  stopTrack();

  if (!SD.exists(path)) {
    Serial.printf("Missing track on SD card: %s\n", path);
    return false;
  }

  currentFile = new AudioFileSourceSD(path);
  if (currentFile == nullptr || !currentFile->isOpen()) {
    Serial.printf("Unable to open track: %s\n", path);
    stopTrack();
    return false;
  }

  if (!mp3Decoder.begin(currentFile, &bufferedOutput)) {
    Serial.printf("Failed to begin MP3 decode: %s\n", path);
    stopTrack();
    return false;
  }

  Serial.printf("Playing: %s\n", path);
  return true;
}

void queueTrackForButton(uint8_t buttonPin) {
  if (buttonPin == BUTTON_1_PIN) {
    queuedPlayTrack1 = true;
  } else if (buttonPin == BUTTON_2_PIN) {
    queuedPlayTrack2 = true;
  }
}

void updateButton(ButtonState &state, uint8_t pin) {
  const bool raw = digitalRead(pin);
  const uint32_t now = millis();

  if (raw != state.lastRaw) {
    state.lastRaw = raw;
    state.changedAtMs = now;
  }

  if ((now - state.changedAtMs) >= DEBOUNCE_MS && state.stable != raw) {
    state.stable = raw;
    if (state.stable == LOW) {
      queueTrackForButton(pin);
    }
  }
}

void setupSdCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card mount failed - playback will not work");
  }
}

void setupBluetooth() {
  a2dpSource.set_auto_reconnect(true);
  a2dpSource.start("GameDay DJ", btDataCallback);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BUTTON_1_PIN, INPUT_PULLUP);
  pinMode(BUTTON_2_PIN, INPUT_PULLUP);
  pinMode(BT_STATUS_LED_PIN, OUTPUT);
  digitalWrite(BT_STATUS_LED_PIN, LOW);

  pcmMutex = xSemaphoreCreateMutex();
  if (pcmMutex == nullptr) {
    Serial.println("Failed to create PCM mutex - playback disabled");
    while (true) {
      digitalWrite(BT_STATUS_LED_PIN, HIGH);
      delay(250);
      digitalWrite(BT_STATUS_LED_PIN, LOW);
      delay(250);
    }
  }
  resetPcmBuffer();

  setupSdCard();
  setupBluetooth();
}

void loop() {
  updateButton(button1, BUTTON_1_PIN);
  updateButton(button2, BUTTON_2_PIN);

  if (queuedPlayTrack1) {
    queuedPlayTrack1 = false;
    startTrack(kTrackButton1);
  }

  if (queuedPlayTrack2) {
    queuedPlayTrack2 = false;
    startTrack(kTrackButton2);
  }

  if (mp3Decoder.isRunning() && !mp3Decoder.loop()) {
    stopTrack();
  }

  digitalWrite(BT_STATUS_LED_PIN, a2dpSource.is_connected() ? HIGH : LOW);

  delay(LOOP_DELAY_MS);
}
