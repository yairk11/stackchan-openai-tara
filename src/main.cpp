#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include <ArduinoJson.h>

#include "EmotionController.h"
#include "ServoGestureController.h"
#include "secrets.h"

static const char* STREAM_HOST = "10.0.0.210";
static constexpr uint16_t STREAM_PORT = 8002;
static const char* STREAM_PATH = "/stream";

static constexpr uint32_t MIC_SAMPLE_RATE = 16000;
static constexpr size_t MIC_CHUNK_SAMPLES = 2000;
static constexpr size_t MIC_CHUNK_BYTES =
    MIC_CHUNK_SAMPLES * sizeof(int16_t);
static constexpr size_t MIC_TX_BLOCK_BYTES = 1000;

static int16_t micChunk[MIC_CHUNK_SAMPLES];

static constexpr uint32_t SPEAKER_SAMPLE_RATE = 24000;
static constexpr uint8_t SPEAKER_VOLUME = 255;
static constexpr int SPEAKER_CHANNEL = 1;

// =====================================================
// SOFTWARE GAIN
// =====================================================

static constexpr int32_t PCM_GAIN_NUM = 4;
static constexpr int32_t PCM_GAIN_DEN = 1;

// =====================================================
// AUDIO RX QUEUE
// =====================================================

static constexpr size_t RX_QUEUE_SLOTS = 12;
static constexpr size_t RX_SLOT_BYTES = 8192;

static uint8_t rxAudio[RX_QUEUE_SLOTS][RX_SLOT_BYTES];
static size_t rxLength[RX_QUEUE_SLOTS];

static size_t rxWrite = 0;
static size_t rxRead = 0;
static size_t rxCount = 0;

static uint32_t rxPackets = 0;
static uint32_t rxBytes = 0;
static uint32_t rxDroppedBlocks = 0;

// =====================================================
// PLAYBACK BUFFERS
// =====================================================

static constexpr size_t PLAYBACK_BUFFERS = 3;

static uint8_t playbackBuffer[PLAYBACK_BUFFERS][RX_SLOT_BYTES];
static size_t playbackWrite = 0;

// =====================================================
// TARA
// =====================================================

EmotionController emotion;
ServoGestureController servoGestures;

// =====================================================
// WEBSOCKET
// =====================================================

WebSocketsClient webSocket;

static bool websocketStarted = false;
static bool websocketConnected = false;
static bool serverReady = false;

// =====================================================
// STATE
// =====================================================

static bool conversationActive = false;
static bool micRunning = false;
static bool responseActive = false;

static bool speakerStarted = false;
static bool speakerStartRequested = false;

static bool audioDoneReceived = false;
static bool responseDoneReceived = false;

static bool lastScreenPressed = false;

static uint32_t micChunkCounter = 0;

static uint32_t lastRssiLogMs = 0;
static constexpr uint32_t RSSI_LOG_INTERVAL_MS = 10000;

// =====================================================
// AUDIO QUEUE
// =====================================================

static void clearAudioQueue() {
    rxWrite = 0;
    rxRead = 0;
    rxCount = 0;

    playbackWrite = 0;
}

static bool pushAudioBlock(
    const uint8_t* data,
    size_t length
) {
    if (data == nullptr || length < 2) {
        return false;
    }

    length &= ~static_cast<size_t>(1);

    if (length == 0) {
        return false;
    }

    if (rxCount >= RX_QUEUE_SLOTS) {
        rxDroppedBlocks++;

        Serial.print("RX QUEUE FULL dropped=");
        Serial.println(rxDroppedBlocks);

        return false;
    }

    if (length > RX_SLOT_BYTES) {
        length = RX_SLOT_BYTES;
    }

    memcpy(
        rxAudio[rxWrite],
        data,
        length
    );

    rxLength[rxWrite] = length;

    rxWrite++;

    if (rxWrite >= RX_QUEUE_SLOTS) {
        rxWrite = 0;
    }

    rxCount++;

    return true;
}

static void queueIncomingAudio(
    const uint8_t* payload,
    size_t length
) {
    if (payload == nullptr || length < 2) {
        return;
    }

    rxPackets++;
    rxBytes += length;

    size_t offset = 0;

    while (offset < length) {
        size_t remaining =
            length - offset;

        size_t blockLength =
            remaining > RX_SLOT_BYTES
                ? RX_SLOT_BYTES
                : remaining;

        blockLength &= ~static_cast<size_t>(1);

        if (blockLength == 0) {
            break;
        }

        if (!pushAudioBlock(
                payload + offset,
                blockLength
            )) {
            break;
        }

        offset += blockLength;
    }

    if (rxPackets % 10 == 0) {
        Serial.print("AUDIO NET RX packets=");
        Serial.print(rxPackets);

        Serial.print(" bytes=");
        Serial.print(rxBytes);

        Serial.print(" queue=");
        Serial.print(rxCount);

        Serial.print(" dropped=");
        Serial.println(rxDroppedBlocks);
    }
}

// =====================================================
// SOFTWARE GAIN + LIMITER
// =====================================================

static void applySpeakerGain(
    uint8_t* data,
    size_t length
) {
    if (data == nullptr || length < 2) {
        return;
    }

    size_t sampleCount =
        length / sizeof(int16_t);

    int16_t* samples =
        reinterpret_cast<int16_t*>(data);

    for (size_t i = 0; i < sampleCount; ++i) {
        int32_t value =
            static_cast<int32_t>(samples[i]);

        value =
            (value * PCM_GAIN_NUM) /
            PCM_GAIN_DEN;

        if (value > 32767) {
            value = 32767;
        }

        if (value < -32768) {
            value = -32768;
        }

        samples[i] =
            static_cast<int16_t>(value);
    }
}

// =====================================================
// AUDIO LEVELS
// =====================================================

static void calculateAudioLevels(
    const int16_t* samples,
    size_t count,
    uint16_t& peak,
    uint32_t& rms
) {
    uint32_t maxValue = 0;
    uint64_t sumSquares = 0;

    for (size_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];

        uint32_t absoluteValue =
            sample < 0
                ? static_cast<uint32_t>(-sample)
                : static_cast<uint32_t>(sample);

        if (absoluteValue > maxValue) {
            maxValue = absoluteValue;
        }

        int64_t signedSample = sample;

        sumSquares +=
            static_cast<uint64_t>(
                signedSample *
                signedSample
            );
    }

    peak =
        static_cast<uint16_t>(
            maxValue > 32767
                ? 32767
                : maxValue
        );

    if (count == 0) {
        rms = 0;
        return;
    }

    uint64_t meanSquare =
        sumSquares / count;

    uint64_t x = meanSquare;
    uint64_t result = 0;

    uint64_t bit =
        static_cast<uint64_t>(1) << 62;

    while (bit > x) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (x >= result + bit) {
            x -= result + bit;

            result =
                (result >> 1) + bit;
        } else {
            result >>= 1;
        }

        bit >>= 2;
    }

    rms =
        static_cast<uint32_t>(result);
}

// =====================================================
// MICROPHONE
// =====================================================

static void stopMicrophone() {
    if (!micRunning) {
        return;
    }

    M5.Mic.end();
    micRunning = false;

    Serial.println("MIC STOPPED");
}

static bool startMicrophone() {
    if (micRunning) {
        return true;
    }

    M5.Speaker.end();

    delay(30);

    auto micConfig = M5.Mic.config();
    micConfig.magnification = 4;
    M5.Mic.config(micConfig);

    Serial.print("MIC MAGNIFICATION: ");
    Serial.println(micConfig.magnification);

    if (!M5.Mic.begin()) {
        Serial.println("Mic begin failed");
        return false;
    }

    micRunning = true;

    Serial.println("MIC STARTED");

    return true;
}

// =====================================================
// SPEAKER START
// =====================================================

static void startSpeakerModeNow() {
    if (speakerStarted) {
        return;
    }

    speakerStartRequested = false;

    stopMicrophone();

    delay(30);

    bool speakerBeginOk = M5.Speaker.begin();
    Serial.print("SPEAKER BEGIN: ");
    Serial.println(speakerBeginOk ? "OK" : "FAIL");

    M5.Speaker.setVolume(
        SPEAKER_VOLUME
    );

    speakerStarted = true;


    emotion.setEmotion("speaking");

    Serial.println();
    Serial.println("==============================");
    Serial.println("SPEAKER MODE");
    Serial.println("MASTER VOLUME: 255");
    Serial.println("PCM SOFTWARE GAIN: x4");
    Serial.println("LIMITER: ON");

    Serial.print("Rate: ");
    Serial.print(SPEAKER_SAMPLE_RATE);
    Serial.println(" Hz");

    Serial.println("==============================");
}

// =====================================================
// FINISH TURN
// =====================================================

static void finishSpeakerMode() {
    if (!speakerStarted) {
        return;
    }

    M5.Speaker.stop();
    M5.Speaker.end();

    speakerStarted = false;
    speakerStartRequested = false;

    responseActive = false;

    audioDoneReceived = false;
    responseDoneReceived = false;

    conversationActive = false;

    clearAudioQueue();

    Serial.println();
    Serial.println("SPEAKER FINISHED");
    Serial.println("TURN FINISHED");
    Serial.println("TARA STOPPED LISTENING");

    emotion.setEmotion("neutral");

    ServoGestureController::Step restingHead[] = {
        {0, 420, 500, 450, false}
    };

    servoGestures.queueSteps(
        "speaking_head_rest",
        restingHead,
        1
    );
}

// =====================================================
// SPEAKER SERVICE
// =====================================================

static void serviceSpeakerAudio() {
    if (
        speakerStartRequested &&
        !speakerStarted
    ) {
        startSpeakerModeNow();
    }

    if (!speakerStarted) {
        return;
    }

    if (
        rxCount > 0 &&
        M5.Speaker.isPlaying(
            SPEAKER_CHANNEL
        ) < 2
    ) {
        size_t length =
            rxLength[rxRead];

        if (
            length >= 2 &&
            length <= RX_SLOT_BYTES
        ) {
            memcpy(
                playbackBuffer[
                    playbackWrite
                ],
                rxAudio[rxRead],
                length
            );

            applySpeakerGain(
                playbackBuffer[
                    playbackWrite
                ],
                length
            );

            size_t samples =
                length /
                sizeof(int16_t);

            bool ok =
                M5.Speaker.playRaw(
                    reinterpret_cast<
                        const int16_t*
                    >(
                        playbackBuffer[
                            playbackWrite
                        ]
                    ),
                    samples,
                    SPEAKER_SAMPLE_RATE,
                    false,
                    1,
                    SPEAKER_CHANNEL
                );

            if (ok) {
                playbackWrite++;

                if (
                    playbackWrite >=
                    PLAYBACK_BUFFERS
                ) {
                    playbackWrite = 0;
                }

                rxRead++;

                if (
                    rxRead >=
                    RX_QUEUE_SLOTS
                ) {
                    rxRead = 0;
                }

                rxCount--;
            } else {
                static uint32_t lastPlayRawFailLogMs = 0;
                uint32_t now = millis();

                if (now - lastPlayRawFailLogMs >= 500) {
                    lastPlayRawFailLogMs = now;

                    Serial.print("SPEAKER playRaw FAIL rxCount=");
                    Serial.print(rxCount);
                    Serial.print(" playing=");
                    Serial.println(
                        M5.Speaker.isPlaying(
                            SPEAKER_CHANNEL
                        )
                    );
                }
            }
        }
    }

    if (
        responseDoneReceived
        &&
        rxCount == 0
        &&
        M5.Speaker.isPlaying(
            SPEAKER_CHANNEL
        ) == 0
    ) {
        finishSpeakerMode();
    }
}

// =====================================================
// WIFI
// =====================================================

static bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    Serial.println();
    Serial.println("Connecting WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    uint32_t started = millis();

    while (
        WiFi.status() != WL_CONNECTED
    ) {
        M5StackChan.update();

        emotion.loop();
        servoGestures.loop();

        delay(50);

        if (
            millis() - started >
            20000
        ) {
            Serial.println("WiFi timeout");

            return false;
        }
    }

    Serial.println("WiFi connected");

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.print("BSSID: ");
    Serial.println(WiFi.BSSIDstr());
    Serial.print("Channel: ");
    Serial.println(WiFi.channel());

    return true;
}

// =====================================================
// TARA MOTION COMMANDS
// =====================================================

static int clampMotionInt(
    int value,
    int minimum,
    int maximum
) {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

static bool handleTaraMotionCommand(
    const String& message
) {
    JsonDocument doc;

    DeserializationError error =
        deserializeJson(
            doc,
            message
        );

    if (error) {
        return false;
    }

    const char* type =
        doc["type"] | "";
    if (
        strcmp(
            type,
            "set_emotion"
        ) == 0
    ) {
        const char* emotionName =
            doc["emotion"] | "neutral";

        emotion.setEmotion(
            String(emotionName)
        );

        Serial.print(
            "TARA EMOTION: "
        );
        Serial.print(
            emotion.currentEmotion()
        );
        Serial.println(
            " result=OK"
        );

        return true;
    }


    if (
        strcmp(
            type,
            "servo_gesture"
        ) == 0
    ) {
        const char* gesture =
            doc["gesture"] | "";

        Serial.print(
            "TARA SERVO GESTURE SKIPPED: "
        );
        Serial.println(gesture);


        return true;
    }

    if (
        strcmp(
            type,
            "head_motion"
        ) == 0
    ) {
        ServoGestureController::Step steps[6]{};
        uint8_t count = 0;

        JsonArrayConst arr =
            doc["steps"]
                .as<JsonArrayConst>();

        for (
            JsonObjectConst step :
            arr
        ) {
            if (count >= 6) {
                break;
            }

            float xDeg =
                step["x_deg"] | 0.0f;

            float yDeg =
                step["y_deg"] | 0.0f;

            int speed =
                step["speed"] | 500;

            int holdMs =
                step["hold_ms"] | 500;

            xDeg =
                constrain(
                    xDeg,
                    -65.0f,
                    65.0f
                );

            yDeg =
                constrain(
                    yDeg,
                    -6.0f,
                    40.0f
                );

            steps[count].x =
                clampMotionInt(
                    static_cast<int>(
                        xDeg * 10.0f
                    ),
                    -650,
                    650
                );

            steps[count].y =
                clampMotionInt(
                    static_cast<int>(
                        yDeg * 10.0f
                    ),
                    -60,
                    400
                );

            steps[count].speed =
                clampMotionInt(
                    speed,
                    100,
                    700
                );

            steps[count].holdMs =
                static_cast<uint16_t>(
                    clampMotionInt(
                        holdMs,
                        100,
                        1500
                    )
                );

            steps[count].relative = true;

            count++;
        }

        if (count == 0) {
            Serial.println(
                "TARA HEAD MOTION: missing_steps"
            );

            return true;
        }

        const char* motionNameRaw =
            doc["motion_name"]
                | "custom_motion";

        String motionName(
            motionNameRaw
        );

        bool queued =
            servoGestures.queueSteps(
                motionName,
                steps,
                count
            );

        Serial.print(
            "TARA HEAD MOTION: "
        );
        Serial.print(motionName);
        Serial.print(" steps=");
        Serial.print(count);
        Serial.print(" result=");
        Serial.println(
            queued
                ? "OK"
                : "FAIL"
        );

        return true;
    }

    return false;
}

// =====================================================
// SERVER TEXT
// =====================================================

static void handleServerText(
    const String& message
) {
    Serial.print("WS: ");
    Serial.println(message);

    if (
        handleTaraMotionCommand(
            message
        )
    ) {
        return;
    }

    if (
        message.indexOf(
            "\"type\": \"ready\""
        ) >= 0
        ||
        message.indexOf(
            "\"type\":\"ready\""
        ) >= 0
    ) {
        serverReady = true;

        Serial.println(
            "Realtime server ready"
        );

        return;
    }

    if (
        message.indexOf(
            "\"type\": \"speech_started\""
        ) >= 0
        ||
        message.indexOf(
            "\"type\":\"speech_started\""
        ) >= 0
    ) {
        Serial.println(
            "OPENAI HEARD SPEECH"
        );

        return;
    }

    if (
        message.indexOf(
            "\"type\": \"speech_stopped\""
        ) >= 0
        ||
        message.indexOf(
            "\"type\":\"speech_stopped\""
        ) >= 0
    ) {
        Serial.println(
            "OPENAI SPEECH STOPPED"
        );

        emotion.setEmotion(
            "thinking"
        );

        return;
    }

    if (
        message.indexOf(
            "\"type\": \"response_started\""
        ) >= 0
        ||
        message.indexOf(
            "\"type\":\"response_started\""
        ) >= 0
    ) {
        Serial.println(
            "OPENAI RESPONSE STARTED"
        );

        responseActive = true;

        audioDoneReceived = false;
        responseDoneReceived = false;

        return;
    }

    if (
        message.indexOf(
            "\"type\": \"audio_done\""
        ) >= 0
        ||
        message.indexOf(
            "\"type\":\"audio_done\""
        ) >= 0
    ) {
        Serial.println(
            "OPENAI AUDIO DONE"
        );

        audioDoneReceived = true;

        return;
    }

    if (
        message.indexOf(
            "\"type\": \"response_done\""
        ) >= 0
        ||
        message.indexOf(
            "\"type\":\"response_done\""
        ) >= 0
    ) {
        Serial.println(
            "OPENAI RESPONSE DONE"
        );

        responseDoneReceived = true;

        return;
    }
}

// =====================================================
// SERVER AUDIO
// =====================================================

static void handleServerAudio(
    uint8_t* payload,
    size_t length
) {
    if (
        payload == nullptr ||
        length < 2
    ) {
        return;
    }

    responseActive = true;

    if (!speakerStarted) {
        speakerStartRequested = true;
    }

    queueIncomingAudio(
        payload,
        length
    );
}

// =====================================================
// WEBSOCKET CALLBACK
// =====================================================

static void webSocketEvent(
    WStype_t type,
    uint8_t* payload,
    size_t length
) {
    switch (type) {

        case WStype_CONNECTED:
            websocketConnected = true;

            Serial.println();
            Serial.println(
                "WebSocket connected"
            );

            break;

        case WStype_DISCONNECTED:
            websocketConnected = false;
            serverReady = false;

            Serial.println();
            Serial.println(
                "WebSocket disconnected"
            );

            stopMicrophone();

            if (speakerStarted) {
                M5.Speaker.stop();
                M5.Speaker.end();

                speakerStarted = false;
            }

            speakerStartRequested = false;
            responseActive = false;

            audioDoneReceived = false;
            responseDoneReceived = false;

            conversationActive = false;

            clearAudioQueue();

            emotion.setEmotion(
                "neutral"
            );

            servoGestures.queueGesture(
                "center_head"
            );

            break;

        case WStype_TEXT: {
            String message(
                reinterpret_cast<char*>(
                    payload
                ),
                length
            );

            handleServerText(
                message
            );

            break;
        }

        case WStype_BIN:
            handleServerAudio(
                payload,
                length
            );

            break;

        case WStype_ERROR:
            Serial.println(
                "WebSocket error"
            );

            break;

        default:
            break;
    }
}

// =====================================================
// WEBSOCKET START
// =====================================================

static void startWebSocket() {
    if (websocketStarted) {
        return;
    }

    Serial.println(
        "Opening WebSocket..."
    );

    webSocket.begin(
        STREAM_HOST,
        STREAM_PORT,
        STREAM_PATH
    );

    webSocket.onEvent(
        webSocketEvent
    );

    webSocket.setReconnectInterval(
        2000
    );

    websocketStarted = true;
}

// =====================================================
// START TURN
// =====================================================

static void startConversation() {
    if (conversationActive) {
        return;
    }

    emotion.setEmotion(
        "thinking"
    );

    if (!connectWiFi()) {
        emotion.setEmotion(
            "error"
        );

        return;
    }

    startWebSocket();

    uint32_t started =
        millis();

    while (
        !websocketConnected &&
        millis() - started < 10000
    ) {
        M5StackChan.update();

        webSocket.loop();

        emotion.loop();
        servoGestures.loop();

        delay(2);
    }

    if (!websocketConnected) {
        Serial.println(
            "WebSocket connect timeout"
        );

        emotion.setEmotion(
            "error"
        );

        return;
    }

    started = millis();

    while (
        !serverReady &&
        millis() - started < 30000
    ) {
        M5StackChan.update();

        webSocket.loop();

        emotion.loop();
        servoGestures.loop();

        delay(2);
    }

    if (!serverReady) {
        Serial.println(
            "Server ready timeout"
        );

        emotion.setEmotion(
            "error"
        );

        return;
    }

    conversationActive = true;

    responseActive = false;

    speakerStarted = false;
    speakerStartRequested = false;

    audioDoneReceived = false;
    responseDoneReceived = false;

    micChunkCounter = 0;

    rxPackets = 0;
    rxBytes = 0;
    rxDroppedBlocks = 0;

    clearAudioQueue();

    if (!startMicrophone()) {
        conversationActive = false;

        emotion.setEmotion(
            "error"
        );

        return;
    }

    emotion.setEmotion(
        "listening"
    );

    Serial.println();

    Serial.println("==============================");
    Serial.println("OPENAI REALTIME STARTED");
    Serial.println("ONE TOUCH = ONE TURN");
    Serial.println("MIC: PCM16 / 16000 Hz");
    Serial.println("MIC CAPTURE: 4000 bytes");
    Serial.println("TX: 4 x 1000 bytes");
    Serial.println("SPEAKER: PCM16 / 24000 Hz");
    Serial.println("MASTER VOLUME: 255");
    Serial.println("PCM SOFTWARE GAIN: x4");
    Serial.println("LIMITER: ON");
    Serial.println("==============================");
}

// =====================================================
// STOP TURN
// =====================================================

static void stopConversation() {
    if (!conversationActive) {
        return;
    }

    conversationActive = false;
    responseActive = false;

    speakerStartRequested = false;

    audioDoneReceived = false;
    responseDoneReceived = false;

    stopMicrophone();

    if (speakerStarted) {
        M5.Speaker.stop();
        M5.Speaker.end();

        speakerStarted = false;
    }

    clearAudioQueue();

    emotion.setEmotion(
        "neutral"
    );

    servoGestures.queueGesture(
        "center_head"
    );

    Serial.println();
    Serial.println(
        "CONVERSATION STOPPED"
    );
}

// =====================================================
// MIC CAPTURE + 4 x 1000 BYTE SEND
// =====================================================

static void captureAndSendChunk() {
    if (
        !conversationActive ||
        !websocketConnected ||
        !micRunning ||
        responseActive
    ) {
        return;
    }

    uint32_t captureStarted =
        millis();

    bool recorded =
        M5.Mic.record(
            micChunk,
            MIC_CHUNK_SAMPLES,
            MIC_SAMPLE_RATE
        );

    if (!recorded) {
        webSocket.loop();
        return;
    }

    while (
        M5.Mic.isRecording()
    ) {
        M5StackChan.update();

        webSocket.loop();

        emotion.loop();
        servoGestures.loop();

        delay(1);
    }

    uint32_t capturedAt =
        millis();

    uint16_t peak = 0;
    uint32_t rms = 0;

    calculateAudioLevels(
        micChunk,
        MIC_CHUNK_SAMPLES,
        peak,
        rms
    );

    uint8_t* raw =
        reinterpret_cast<uint8_t*>(
            micChunk
        );

    bool allSent = true;

    uint32_t sendStarted =
        millis();

    uint32_t worstBlockMs = 0;

    for (
        size_t offset = 0;
        offset < MIC_CHUNK_BYTES;
        offset += MIC_TX_BLOCK_BYTES
    ) {
        if (
            !websocketConnected ||
            responseActive
        ) {
            allSent = false;
            break;
        }

        size_t blockLength =
            MIC_CHUNK_BYTES -
            offset;

        if (
            blockLength >
            MIC_TX_BLOCK_BYTES
        ) {
            blockLength =
                MIC_TX_BLOCK_BYTES;
        }

        uint32_t blockStarted =
            millis();

        bool sent =
            webSocket.sendBIN(
                raw + offset,
                blockLength
            );

        uint32_t blockElapsed =
            millis() -
            blockStarted;

        if (
            blockElapsed >
            worstBlockMs
        ) {
            worstBlockMs =
                blockElapsed;
        }

        if (!sent) {
            allSent = false;
            break;
        }

        webSocket.loop();

        M5StackChan.update();

        emotion.loop();
        servoGestures.loop();

        delay(1);
    }

    uint32_t sendElapsed =
        millis() -
        sendStarted;

    micChunkCounter++;

    Serial.print("MIC ");
    Serial.print(micChunkCounter);

    Serial.print(" capture=");
    Serial.print(
        capturedAt -
        captureStarted
    );

    Serial.print("ms send=");
    Serial.print(sendElapsed);

    Serial.print("ms worst=");
    Serial.print(worstBlockMs);

    Serial.print("ms peak=");
    Serial.print(peak);

    Serial.print(" rms=");
    Serial.print(rms);

    Serial.print(" result=");

    Serial.println(
        allSent
            ? "OK"
            : "FAIL"
    );
}

// =====================================================
// SETUP
// =====================================================

void setup() {
    Serial.begin(115200);

    delay(300);

    M5StackChan.begin();

    emotion.begin();
    servoGestures.begin();

    M5StackChan.Motion.goHome();

    emotion.setEmotion(
        "neutral"
    );

    ServoGestureController::Step startupHead[] = {
        {0, 420, 500, 700, false}
    };

    servoGestures.queueSteps(
        "startup_head_30",
        startupHead,
        1
    );

    clearAudioQueue();

    Serial.println();
    Serial.println("================================");
    Serial.println("STACKCHAN OPENAI REALTIME");
    Serial.println("TARA UI / LED / SERVO PRESERVED");
    Serial.println("ONE TOUCH = ONE TURN");
    Serial.println("MASTER VOLUME = 255");
    Serial.println("PCM SOFTWARE GAIN = x4");
    Serial.println("LIMITER = ON");
    Serial.println("MIC TX = 4 x 1000 BYTES");
    Serial.println("NON-BLOCKING AUDIO RX");
    Serial.println("================================");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
    M5StackChan.update();

    static String serialCommand;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            serialCommand.trim();

            if (serialCommand.equalsIgnoreCase("shutdown")) {
                Serial.println("TARA SHUTDOWN");
                Serial.flush();
                delay(200);
                M5.Power.powerOff();
            }

            serialCommand = "";
        } else {
            serialCommand += c;
        }
    }
    emotion.loop();
    servoGestures.loop();

    if (websocketStarted) {
        webSocket.loop();
    }

    uint32_t nowMs = millis();

    if (
        WiFi.status() == WL_CONNECTED &&
        nowMs - lastRssiLogMs >= RSSI_LOG_INTERVAL_MS
    ) {
        lastRssiLogMs = nowMs;

        Serial.print("RSSI: ");
        Serial.print(WiFi.RSSI());
        Serial.println(" dBm");
    }

    serviceSpeakerAudio();

#if defined(ARDUINO_M5STACK_CORES3)
    bool touched = false;

#if defined(ARDUINO_M5STACK_CORES3)

    auto touchDetail =
        M5.Touch.getDetail();

    bool screenPressed =
        (
            M5.Touch.getCount() > 0
        )
        ||
        touchDetail.isPressed();

    if (
        screenPressed &&
        !lastScreenPressed
    ) {
        touched = true;
    }

    lastScreenPressed =
        screenPressed;

#endif
#else
    bool touched =
        M5StackChan.TouchSensor.wasPressed();
#endif

    if (touched) {
        if (conversationActive) {
            stopConversation();
        } else {
            startConversation();
        }
    }

    if (
        conversationActive &&
        websocketConnected &&
        micRunning &&
        !responseActive
    ) {
        captureAndSendChunk();
    }

    serviceSpeakerAudio();

    delay(1);
}






