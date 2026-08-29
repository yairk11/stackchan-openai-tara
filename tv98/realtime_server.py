import asyncio
import base64
import json
import os
from array import array

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve


# ============================================================
# CONFIG
# ============================================================

HOST = "0.0.0.0"
PORT = 8002

ENV_FILE = "stackchan.env"

MODEL = "gpt-realtime-2.1"

CORE_INPUT_RATE = 16000
OPENAI_INPUT_RATE = 24000
OPENAI_OUTPUT_RATE = 24000

INPUT_QUEUE_MAX = 256
OUTPUT_QUEUE_MAX = 512

# PCM16 mono @ 24 kHz:
# 24000 samples/sec * 2 bytes = 48000 bytes/sec.
AUDIO_BYTES_PER_SECOND = 48000

# Send 4096-byte pieces to CoreS3.
# 4096 bytes = about 85.3 ms audio.
AUDIO_SEND_CHUNK_BYTES = 4096

REALTIME_URL = (
    "wss://api.openai.com/v1/realtime"
    f"?model={MODEL}"
)

MEMORY_FILE = "tara_memory.json"

# Only one CoreS3 WebSocket connection may be active at a time.
active_stackchan_ws = None


def load_memory():
    try:
        if not os.path.exists(MEMORY_FILE):
            return []

        with open(
            MEMORY_FILE,
            "r",
            encoding="utf-8"
        ) as f:
            data = json.load(f)

        if not isinstance(data, list):
            return []

        return [
            str(item)
            for item in data
            if str(item).strip()
        ][:50]

    except Exception as e:
        print("MEMORY LOAD ERROR:", repr(e))
        return []


def save_memory(memory):
    try:
        clean = [
            str(item).strip()
            for item in memory
            if str(item).strip()
        ][:50]

        with open(
            MEMORY_FILE,
            "w",
            encoding="utf-8"
        ) as f:
            json.dump(
                clean,
                f,
                ensure_ascii=False,
                indent=2
            )

        return True

    except Exception as e:
        print("MEMORY SAVE ERROR:", repr(e))
        return False


# ============================================================
# ENV
# ============================================================

def load_env_file(path):
    if not os.path.exists(path):
        return

    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()

            if not line:
                continue

            if line.startswith("#"):
                continue

            if "=" not in line:
                continue

            key, value = line.split("=", 1)

            key = key.strip()
            value = value.strip()

            if (
                len(value) >= 2
                and value[0] == value[-1]
                and value[0] in ("'", '"')
            ):
                value = value[1:-1]

            if key and key not in os.environ:
                os.environ[key] = value


# ============================================================
# RESAMPLER 16 kHz -> 24 kHz
# ============================================================

def resample_pcm16_16k_to_24k(data):
    if not data:
        return b""

    usable = len(data) & ~1

    if usable < 4:
        return data[:usable]

    source = array("h")
    source.frombytes(data[:usable])

    source_count = len(source)

    if source_count < 2:
        return data[:usable]

    # 16 kHz -> 24 kHz = x1.5
    output_count = (source_count * 3) // 2

    output = array(
        "h",
        [0] * output_count
    )

    for out_index in range(output_count):
        # source position = out_index * 2/3
        base = (out_index * 2) // 3
        phase = (out_index * 2) % 3

        if base >= source_count - 1:
            output[out_index] = source[-1]
            continue

        a = source[base]
        b = source[base + 1]

        if phase == 0:
            value = a

        elif phase == 1:
            value = (
                (a * 2) + b
            ) // 3

        else:
            value = (
                a + (b * 2)
            ) // 3

        if value > 32767:
            value = 32767

        elif value < -32768:
            value = -32768

        output[out_index] = value

    return output.tobytes()


# ============================================================
# OPENAI SESSION
# ============================================================

def make_session_update():
    memory = load_memory()

    memory_text = ""

    if memory:
        memory_text = (
            " Persistent memory from previous sessions: "
            + " | ".join(memory)
            + ". Use these facts only when relevant and do not invent details beyond them."
        )

    return {
        "type": "session.update",
        "session": {
            "type": "realtime",
            "model": MODEL,

            "output_modalities": [
                "audio"
            ],

            "tools": [
                {
                    "type": "function",
                    "name": "servo_gesture",
                    "description": (
                        "Queue a safe non-blocking StackChan head gesture. "
                        "Use center_head only when the user explicitly asks "
                        "to center/rest/go home or ends the session. "
                        "For natural pre-speech motion prefer small gestures."
                    ),
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "gesture": {
                                "type": "string",
                                "enum": [
                                    "center_head",
                                    "look_at_user",
                                    "look_left_small",
                                    "look_right_small",
                                    "look_left",
                                    "look_right",
                                    "look_up",
                                    "look_down",
                                    "look_top_left",
                                    "look_top_right",
                                    "nod_yes",
                                    "shake_no",
                                    "curious_tilt",
                                    "speaking_micro_motion",
                                    "stop_motion"
                                ]
                            }
                        },
                        "required": [
                            "gesture"
                        ]
                    }
                },
                {
                    "type": "function",
                    "name": "set_emotion",
                    "description": "Set StackChan face/emotion state.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "emotion": {
                                "type": "string",
                                "enum": [
                                    "neutral",
                                    "happy",
                                    "thinking",
                                    "looking",
                                    "speaking",
                                    "found",
                                    "error",
                                    "sleep"
                                ]
                            }
                        },
                        "required": [
                            "emotion"
                        ]
                    }
                },
                {
                    "type": "function",
                    "name": "head_motion",
                    "description": (
                        "Create a safe smooth StackChan head motion. "
                        "Coordinates are relative to the current persistent "
                        "look anchor. x_deg negative is robot left and "
                        "positive is robot right. y_deg positive is up and "
                        "negative is down. Use 1 to 6 keyframes."
                    ),
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "motion_name": {
                                "type": "string"
                            },
                            "steps": {
                                "type": "array",
                                "minItems": 1,
                                "maxItems": 6,
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "x_deg": {
                                            "type": "number",
                                            "minimum": -65,
                                            "maximum": 65
                                        },
                                        "y_deg": {
                                            "type": "number",
                                            "minimum": -6,
                                            "maximum": 40
                                        },
                                        "speed": {
                                            "type": "integer",
                                            "minimum": 100,
                                            "maximum": 700
                                        },
                                        "hold_ms": {
                                            "type": "integer",
                                            "minimum": 100,
                                            "maximum": 1500
                                        }
                                    },
                                    "required": [
                                        "x_deg",
                                        "y_deg",
                                        "speed",
                                        "hold_ms"
                                    ]
                                }
                            }
                        },
                        "required": [
                            "steps"
                        ]
                    }
                },
                {
                    "type": "function",
                    "name": "remember_fact",
                    "description": (
                        "Save a useful non-sensitive fact the user clearly wants "
                        "remembered across future sessions, such as preferences or "
                        "favorite things. Never store passwords, API keys, secrets, "
                        "addresses, financial data, health data, or other sensitive information."
                    ),
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "fact": {
                                "type": "string"
                            }
                        },
                        "required": [
                            "fact"
                        ]
                    }
                }
            ],

            "tool_choice": "auto",

            "instructions": (
                "You are StackChan, a compact embodied desktop robot. "
                "Always reply in Hebrew by default. Switch to another language only when the user explicitly asks you to speak that language. "
                "Your head has a persistent look anchor: after a deliberate "
                "look or turn, normal nods, tilts, and speaking micro-motions "
                "are relative to that anchor. "
                "Do not return to center after every answer. "
                "For simple factual or short replies, speak immediately without a motion tool. "
                "For expressive or conversational replies, you may call at most one brief natural motion tool first. "
                "Use servo_gesture for small nods, tilts, and glances, or "
                "head_motion for a custom smooth relative motion. "
                "Vary the motion naturally. "
                "Do not call motion tools repeatedly or during speech. "
                "When the user explicitly asks you to show or set an emotion, "
                "call set_emotion with the matching StackChan emotion before speaking. "
                "Behave like a compact embodied desktop robot, not like a "
                "disembodied chatbot. "
                "Speak naturally and conversationally. "
                "Speak at a calm, slightly slower pace with clear pauses. "
                "When the user explicitly asks you to remember a durable non-sensitive preference or fact, call remember_fact before replying. Keep ordinary spoken replies concise and natural. "
                "Usually answer in one short sentence. "
                "Never give more than two short sentences unless "
                "the user explicitly asks for a longer explanation."
                + memory_text
            ),

            "audio": {
                "input": {
                    "format": {
                        "type": "audio/pcm",
                        "rate": OPENAI_INPUT_RATE
                    },

                    "turn_detection": {
                        "type": "semantic_vad"
                    }
                },

                "output": {
                    "format": {
                        "type": "audio/pcm",
                        "rate": OPENAI_OUTPUT_RATE
                    },

                    "voice": "marin"
                }
            }
        }
    }


# ============================================================
# STACKCHAN CONNECTION
# ============================================================

async def handle_stackchan(stackchan_ws):
    global active_stackchan_ws

    remote = stackchan_ws.remote_address

    previous_ws = active_stackchan_ws
    active_stackchan_ws = stackchan_ws

    if (
        previous_ws is not None
        and previous_ws is not stackchan_ws
    ):
        print(
            "Replacing previous StackChan connection:",
            previous_ws.remote_address
        )

        try:
            await previous_ws.close(
                code=1012,
                reason="Replaced by newer StackChan connection"
            )

        except Exception as e:
            print(
                "Previous StackChan close error:",
                repr(e)
            )

    print()
    print("==============================")
    print(
        "StackChan connected:",
        remote
    )
    print("==============================")

    input_queue = asyncio.Queue(
        maxsize=INPUT_QUEUE_MAX
    )

    output_queue = asyncio.Queue(
        maxsize=OUTPUT_QUEUE_MAX
    )

    input_chunks = 0
    input_bytes = 0
    dropped_input_chunks = 0

    openai_sent_chunks = 0
    openai_sent_bytes = 0

    output_audio_chunks = 0
    output_audio_bytes = 0

    dropped_output_chunks = 0


    # --------------------------------------------------------
    # OUTPUT QUEUE HELPERS
    # --------------------------------------------------------

    async def queue_text(message):
        try:
            output_queue.put_nowait(
                (
                    "text",
                    json.dumps(message)
                )
            )

        except asyncio.QueueFull:
            print(
                "OUTPUT QUEUE FULL: "
                "text event dropped"
            )


    async def queue_binary(data):
        nonlocal dropped_output_chunks

        if not data:
            return

        # Split large OpenAI deltas before putting them
        # into the CoreS3 output queue.
        offset = 0

        while offset < len(data):
            block = data[
                offset:
                offset + AUDIO_SEND_CHUNK_BYTES
            ]

            try:
                output_queue.put_nowait(
                    (
                        "audio",
                        bytes(block)
                    )
                )

            except asyncio.QueueFull:
                dropped_output_chunks += 1

                print(
                    "OUTPUT QUEUE FULL "
                    f"dropped_audio="
                    f"{dropped_output_chunks}"
                )

                return

            offset += len(block)


    # --------------------------------------------------------
    # PACED STACKCHAN SENDER
    # --------------------------------------------------------

    async def stackchan_sender():
        audio_deadline = (
            asyncio.get_running_loop().time()
        )

        while True:
            kind, payload = (
                await output_queue.get()
            )

            try:
                if kind == "text":
                    await stackchan_ws.send(
                        payload
                    )

                    continue


                # AUDIO:
                # Send no faster than actual playback speed.

                now = (
                    asyncio.get_running_loop()
                    .time()
                )

                if audio_deadline < now:
                    audio_deadline = now

                wait_seconds = (
                    audio_deadline - now
                )

                if wait_seconds > 0:
                    await asyncio.sleep(
                        wait_seconds
                    )

                await stackchan_ws.send(
                    payload
                )

                duration = (
                    len(payload)
                    / AUDIO_BYTES_PER_SECOND
                )

                audio_deadline += duration

            finally:
                output_queue.task_done()


    sender_task = asyncio.create_task(
        stackchan_sender()
    )


    try:
        api_key = os.environ.get(
            "OPENAI_API_KEY"
        )

        if not api_key:
            raise RuntimeError(
                "OPENAI_API_KEY is missing"
            )


        # ----------------------------------------------------
        # CONNECT OPENAI
        # ----------------------------------------------------

        async with connect(
            REALTIME_URL,

            additional_headers={
                "Authorization":
                    f"Bearer {api_key}"
            },

            max_size=None,
            open_timeout=30,
            ping_interval=20,
            ping_timeout=20
        ) as openai_ws:

            print(
                "Connected to OpenAI Realtime"
            )


            # ------------------------------------------------
            # WAIT FOR session.created
            # ------------------------------------------------

            first_message = (
                await openai_ws.recv()
            )

            first_event = json.loads(
                first_message
            )

            print(
                "OpenAI:",
                first_event.get(
                    "type",
                    "?"
                )
            )


            # ------------------------------------------------
            # SESSION UPDATE
            # ------------------------------------------------

            await openai_ws.send(
                json.dumps(
                    make_session_update()
                )
            )


            while True:
                message = (
                    await openai_ws.recv()
                )

                event = json.loads(
                    message
                )

                event_type = event.get(
                    "type",
                    ""
                )

                print(
                    "OpenAI:",
                    event_type
                )

                if (
                    event_type
                    == "session.updated"
                ):
                    break

                if event_type == "error":
                    print(
                        json.dumps(
                            event,
                            ensure_ascii=False
                        )
                    )


            print(
                "OpenAI session ready"
            )


            await queue_text({
                "type": "ready",
                "server": "realtime",
                "port": PORT,
                "output_audio": "pcm16",
                "output_rate":
                    OPENAI_OUTPUT_RATE
            })


            # ------------------------------------------------
            # SEND AUDIO TO OPENAI
            # ------------------------------------------------

            async def openai_audio_sender():
                nonlocal openai_sent_chunks
                nonlocal openai_sent_bytes

                while True:
                    data16 = (
                        await input_queue.get()
                    )

                    try:
                        data24 = (
                            resample_pcm16_16k_to_24k(
                                data16
                            )
                        )

                        encoded = (
                            base64.b64encode(
                                data24
                            ).decode(
                                "ascii"
                            )
                        )

                        await openai_ws.send(
                            json.dumps({
                                "type":
                                    "input_audio_buffer.append",

                                "audio":
                                    encoded
                            })
                        )

                        openai_sent_chunks += 1
                        openai_sent_bytes += (
                            len(data24)
                        )

                        if (
                            openai_sent_chunks % 8
                            == 0
                        ):
                            print(
                                "OPENAI SEND "
                                f"chunks="
                                f"{openai_sent_chunks} "
                                f"bytes24="
                                f"{openai_sent_bytes} "
                                f"queue="
                                f"{input_queue.qsize()}"
                            )

                    finally:
                        input_queue.task_done()


            # ------------------------------------------------
            # RECEIVE EVENTS FROM OPENAI
            # ------------------------------------------------

            async def receive_openai_events():
                nonlocal output_audio_chunks
                nonlocal output_audio_bytes

                pending_tool_calls = []
                response_audio_started = False

                async for message in openai_ws:
                    event = json.loads(
                        message
                    )

                    event_type = event.get(
                        "type",
                        ""
                    )


                    if (
                        event_type
                        == "input_audio_buffer.speech_started"
                    ):
                        print(
                            ">>> SPEECH STARTED"
                        )

                        await queue_text({
                            "type":
                                "speech_started"
                        })

                        continue


                    if (
                        event_type
                        == "input_audio_buffer.speech_stopped"
                    ):
                        print(
                            ">>> SPEECH STOPPED"
                        )

                        await queue_text({
                            "type":
                                "speech_stopped"
                        })

                        continue


                    if (
                        event_type
                        == "response.function_call_arguments.done"
                    ):
                        tool_name = event.get(
                            "name",
                            ""
                        )

                        call_id = event.get(
                            "call_id",
                            ""
                        )

                        raw_arguments = event.get(
                            "arguments",
                            "{}"
                        )

                        try:
                            tool_args = json.loads(
                                raw_arguments
                            )
                        except Exception:
                            tool_args = {}

                        tool_result = {
                            "ok": False,
                            "tool": tool_name
                        }

                        if tool_name == "remember_fact":
                            fact = str(
                                tool_args.get(
                                    "fact",
                                    ""
                                )
                            ).strip()

                            memory = load_memory()

                            if fact and fact not in memory:
                                memory.append(fact)

                            saved = save_memory(memory)

                            tool_result = {
                                "ok": saved,
                                "fact": fact
                            }

                            print(
                                ">>> TARA TOOL "
                                f"remember_fact "
                                f"{fact}"
                            )

                        elif tool_name == "set_emotion":
                            emotion_name = str(
                                tool_args.get(
                                    "emotion",
                                    "neutral"
                                )
                            )

                            await queue_text({
                                "type": "set_emotion",
                                "emotion": emotion_name
                            })

                            tool_result = {
                                "ok": True,
                                "emotion": emotion_name
                            }

                            print(
                                ">>> TARA TOOL "
                                f"set_emotion "
                                f"{emotion_name}"
                            )

                        elif tool_name == "servo_gesture":
                            gesture = str(
                                tool_args.get(
                                    "gesture",
                                    ""
                                )
                            )

                            await queue_text({
                                "type":
                                    "servo_gesture",

                                "gesture":
                                    gesture
                            })

                            tool_result = {
                                "ok": True,
                                "queued": gesture,
                                "async": True
                            }

                            print(
                                ">>> TARA TOOL "
                                f"servo_gesture "
                                f"{gesture}"
                            )

                        elif tool_name == "head_motion":
                            motion_name = str(
                                tool_args.get(
                                    "motion_name",
                                    "custom_motion"
                                )
                            )

                            steps = tool_args.get(
                                "steps",
                                []
                            )

                            if not isinstance(
                                steps,
                                list
                            ):
                                steps = []

                            steps = steps[:6]

                            await queue_text({
                                "type":
                                    "head_motion",

                                "motion_name":
                                    motion_name,

                                "steps":
                                    steps
                            })

                            tool_result = {
                                "ok": True,
                                "queued": motion_name,
                                "step_count":
                                    len(steps),
                                "async": True
                            }

                            print(
                                ">>> TARA TOOL "
                                f"head_motion "
                                f"{motion_name} "
                                f"steps={len(steps)}"
                            )

                        else:
                            tool_result = {
                                "ok": False,
                                "error":
                                    "unknown_tool",
                                "tool":
                                    tool_name
                            }

                            print(
                                ">>> UNKNOWN TOOL:",
                                tool_name
                            )

                        pending_tool_calls.append({
                            "call_id":
                                call_id,

                            "output":
                                json.dumps(
                                    tool_result,
                                    ensure_ascii=False
                                )
                        })

                        continue


                    if (
                        event_type
                        == "response.created"
                    ):
                        print(
                            ">>> RESPONSE CREATED"
                        )

                        response_audio_started = False

                        continue


                    if event_type in (
                        "response.output_audio.delta",
                        "response.audio.delta"
                    ):
                        delta = event.get(
                            "delta",
                            ""
                        )

                        if delta:
                            if not response_audio_started:
                                await queue_text({
                                    "type":
                                        "response_started"
                                })

                                response_audio_started = True

                                print(
                                    ">>> AUDIO RESPONSE STARTED"
                                )

                            audio_data = (
                                base64.b64decode(
                                    delta
                                )
                            )

                            output_audio_chunks += 1
                            output_audio_bytes += (
                                len(audio_data)
                            )

                            await queue_binary(
                                audio_data
                            )

                            if (
                                output_audio_chunks
                                % 10 == 0
                            ):
                                print(
                                    "AUDIO OUT "
                                    f"chunks="
                                    f"{output_audio_chunks} "
                                    f"bytes="
                                    f"{output_audio_bytes} "
                                    f"queue="
                                    f"{output_queue.qsize()}"
                                )

                        continue


                    if event_type in (
                        "response.output_audio.done",
                        "response.audio.done"
                    ):
                        print(
                            ">>> AUDIO DONE"
                        )

                        await queue_text({
                            "type":
                                "audio_done"
                        })

                        continue


                    if (
                        event_type
                        == "response.done"
                    ):
                        if pending_tool_calls:
                            print(
                                ">>> TOOL RESPONSE DONE"
                            )

                            completed_tools = (
                                pending_tool_calls[:]
                            )

                            pending_tool_calls.clear()

                            for tool_call in completed_tools:
                                await openai_ws.send(
                                    json.dumps({
                                        "type":
                                            "conversation.item.create",

                                        "item": {
                                            "type":
                                                "function_call_output",

                                            "call_id":
                                                tool_call[
                                                    "call_id"
                                                ],

                                            "output":
                                                tool_call[
                                                    "output"
                                                ]
                                        }
                                    })
                                )

                            await openai_ws.send(
                                json.dumps({
                                    "type":
                                        "response.create"
                                })
                            )

                            print(
                                ">>> TOOL OUTPUT SENT; "
                                "CONTINUING RESPONSE"
                            )

                            continue

                        print(
                            ">>> RESPONSE DONE"
                        )

                        await queue_text({
                            "type":
                                "response_done",

                            "audio_chunks":
                                output_audio_chunks,

                            "audio_bytes":
                                output_audio_bytes
                        })

                        continue


                    if event_type == "error":
                        print(
                            "OPENAI ERROR:"
                        )

                        print(
                            json.dumps(
                                event,
                                ensure_ascii=False
                            )
                        )

                        error_info = event.get(
                            "error",
                            {}
                        )

                        error_code = error_info.get(
                            "code",
                            ""
                        )

                        if error_code == "session_expired":
                            print(
                                "OPENAI SESSION EXPIRED - "
                                "FORCING RECONNECT"
                            )

                            await stackchan_ws.close(
                                code=1012,
                                reason="OpenAI session expired"
                            )

                            raise RuntimeError(
                                "OpenAI session expired"
                            )


            openai_sender_task = (
                asyncio.create_task(
                    openai_audio_sender()
                )
            )

            openai_receiver_task = (
                asyncio.create_task(
                    receive_openai_events()
                )
            )


            # ------------------------------------------------
            # RECEIVE AUDIO FROM CORES3
            # ------------------------------------------------

            try:
                async for message in stackchan_ws:

                    if isinstance(
                        message,
                        str
                    ):
                        continue


                    input_chunks += 1
                    input_bytes += len(
                        message
                    )


                    if (
                        openai_sender_task.done()
                        or openai_receiver_task.done()
                    ):
                        raise RuntimeError(
                            "OpenAI task stopped"
                        )

                    try:
                        input_queue.put_nowait(
                            bytes(message)
                        )

                    except asyncio.QueueFull:
                        dropped_input_chunks += 1

                        print(
                            "INPUT QUEUE FULL "
                            f"dropped="
                            f"{dropped_input_chunks}"
                        )


                    if (
                        input_chunks % 8
                        == 0
                    ):
                        print(
                            "CORE RX "
                            f"chunks="
                            f"{input_chunks} "
                            f"bytes16="
                            f"{input_bytes} "
                            f"queue="
                            f"{input_queue.qsize()} "
                            f"dropped="
                            f"{dropped_input_chunks}"
                        )

                        await queue_text({
                            "type":
                                "stats",

                            "chunks":
                                input_chunks,

                            "bytes":
                                input_bytes,

                            "queue":
                                input_queue.qsize(),

                            "dropped":
                                dropped_input_chunks
                        })


            finally:
                openai_sender_task.cancel()
                openai_receiver_task.cancel()

                try:
                    await openai_sender_task

                except asyncio.CancelledError:
                    pass


                try:
                    await openai_receiver_task

                except asyncio.CancelledError:
                    pass


    except Exception as e:
        print(
            "Connection error:",
            repr(e)
        )


    sender_task.cancel()

    try:
        await sender_task

    except asyncio.CancelledError:
        pass

    except Exception as e:
        print(
            "Sender stopped:",
            repr(e)
        )


    print()
    print("--------------------------------")
    print("STREAM SUMMARY")

    print(
        "peer:",
        remote[0]
        if remote
        else "?"
    )

    print(
        "input_chunks:",
        input_chunks
    )

    print(
        "input_bytes:",
        input_bytes
    )

    print(
        "dropped_input_chunks:",
        dropped_input_chunks
    )

    print(
        "openai_sent_chunks:",
        openai_sent_chunks
    )

    print(
        "openai_sent_bytes:",
        openai_sent_bytes
    )

    print(
        "output_audio_chunks:",
        output_audio_chunks
    )

    print(
        "output_audio_bytes:",
        output_audio_bytes
    )

    print(
        "dropped_output_chunks:",
        dropped_output_chunks
    )

    print("--------------------------------")

    if active_stackchan_ws is stackchan_ws:
        active_stackchan_ws = None


# ============================================================
# MAIN
# ============================================================

async def main():
    load_env_file(
        ENV_FILE
    )

    print("==============================")
    print(
        "STACKCHAN OPENAI REALTIME PROXY"
    )
    print(
        "PACED AUDIO OUTPUT ENABLED"
    )
    print(
        "ORIGINAL TARA BEHAVIOR ENABLED"
    )
    print("==============================")

    print(
        "Listening:",
        HOST,
        PORT
    )

    print(
        "Model:",
        MODEL
    )

    print(
        "CoreS3 input: PCM16 / 16000 Hz"
    )

    print(
        "OpenAI input: PCM16 / 24000 Hz"
    )

    print(
        "OpenAI output: PCM16 / 24000 Hz"
    )

    print(
        "Audio pacing:",
        AUDIO_BYTES_PER_SECOND,
        "bytes/sec"
    )

    print(
        "Audio packet:",
        AUDIO_SEND_CHUNK_BYTES,
        "bytes"
    )

    print(
        "Input queue:",
        INPUT_QUEUE_MAX
    )

    print("==============================")


    async with serve(
        handle_stackchan,
        HOST,
        PORT,
        max_size=None,

        # IMPORTANT:
        # Arduino WebSockets client caused
        # keepalive ping timeout before.
        ping_interval=None,
        ping_timeout=None
    ):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(
        main()
    )






