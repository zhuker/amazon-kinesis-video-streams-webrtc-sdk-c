import argparse
import asyncio
import json
import logging
import math
import os
import platform
import ssl
import time
from typing import Optional

import av
import cv2
import numpy
import stat
import threading
from aiohttp import web
from av import VideoFrame

from aiortc import (
    MediaStreamTrack,
    RTCPeerConnection, RTCConfiguration,
    RTCRtpSender,
    RTCSessionDescription, VideoStreamTrack, RTCIceServer,
)
from aiortc.contrib.media import MediaPlayer, MediaRelay
from examples.webcam.input_events import parse_touch_packet, MotionEvent, KeyEvent, parse_keyevent_packet, \
    ControlMessage
from examples.webcam.x11_input import ANDROID_TO_X11_KEYCODE_MAP

try:
    from Xlib import X, display, XK
    from Xlib.ext import xtest
except ImportError:
    display = None
    xtest = None

ROOT = os.path.dirname(__file__)
X11_DISPLAY=":0"

pcs = set()
ice_servers = []
relay = None
webcam = None


class NoopInputInjector:

    def inject_touch_event(self, data: bytes):
        event = parse_touch_packet(data)
        print(event)

    def inject_key_event(self, data: bytes):
        event = parse_keyevent_packet(data)
        print(event)

    def handle_message(self, message: bytes):
        if not isinstance(message, bytes) or len(message) == 0:
            return

        msg_type = message[0]
        if msg_type == ControlMessage.TYPE_INJECT_TOUCH_EVENT:
            self.inject_touch_event(message)
        elif msg_type == ControlMessage.TYPE_INJECT_KEYCODE:
            self.inject_key_event(message)


class FifoInjector:
    def __init__(self, fifo_path):
        self.fifo_path = fifo_path
        self.fifo = None

        self.setup_fifo()
        self.open_fifo()

    def setup_fifo(self):
        """Create the FIFO if it doesn't exist."""
        if not os.path.exists(self.fifo_path):
            try:
                os.mkfifo(self.fifo_path)
                print(f"Created FIFO at: {self.fifo_path}")
            except OSError as e:
                print(f"Error creating FIFO: {e}")
        elif not stat.S_ISFIFO(os.stat(self.fifo_path).st_mode):
            print(f"Error: {self.fifo_path} exists but is not a FIFO.")

    def open_fifo(self):
        """Open FIFO for writing in a separate thread to avoid blocking GUI."""

        def open_in_thread():
            try:
                # Opening a FIFO for writing blocks until a reader opens it
                # So we do this in a separate thread
                self.fifo = open(self.fifo_path, 'wb')
                print("FIFO opened successfully")
            except Exception as e:
                print(f"Error opening FIFO: {e}")
                self.fifo = None

        # Start opening FIFO in background thread
        thread = threading.Thread(target=open_in_thread, daemon=True)
        thread.start()

    def inject_event(self, data: bytes):
        if self.fifo is None:
            print("FIFO not open yet, cannot send event")
            # Try to open again if it failed
            self.open_fifo()
            return

        try:
            self.fifo.write(data)
            self.fifo.flush()

        except Exception as e:
            print(f"Error sending touch event: {e}")
            self.fifo = None
            # Try to reopen
            self.open_fifo()

    def handle_message(self, message: bytes):
        if not isinstance(message, bytes) or len(message) == 0:
            return

        self.inject_event(message)


class X11Injector:
    def __init__(self, display_str=X11_DISPLAY):
        if not display:
            raise ImportError("python-xlib is not installed")
        self.d = display.Display(display_str)
        self.root = self.d.screen().root
        print(f"root {self.root}")
        screen = self.d.screen()
        self.x11_width = screen.width_in_pixels
        self.x11_height = screen.height_in_pixels
        print(f"X11 Logical Resolution: {self.x11_width}x{self.x11_height}")
        # Logical size (What X11 fake_input expects)
        print(f"Logical Width:  {screen.width_in_pixels}")
        print(f"Logical Height: {screen.height_in_pixels}")

        # Physical size (Millimeters)
        print(f"Physical Width (mm):  {screen.width_in_mms}")
        print(f"Physical Height (mm): {screen.height_in_mms}")

    def inject_touch_event(self, data: bytes):
        event = parse_touch_packet(data)
        if not event:
            return

        print("touch", event)

        # Move pointer
        xtest.fake_input(self.d, X.MotionNotify, x=event.x, y=event.y, root=self.root)

        # Map MotionEvent actions to X11 button events
        # ACTION_DOWN: 0x00 -> press
        # ACTION_UP: 0x01 -> release
        # ACTION_MOVE: 0x02 -> just move
        if event.action in (MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP):
            # We map the primary mouse button for now.
            # In JS, e.buttons === 1 is the primary button.
            button_code = 1  # Left mouse button
            press_ = X.ButtonPress if event.action == MotionEvent.ACTION_DOWN else X.ButtonRelease
            xtest.fake_input(self.d, press_, button_code)
            print(press_)
        self.d.sync()

    def inject_key_event(self, data: bytes):
        event = parse_keyevent_packet(data)
        if not event:
            return
        print("key", event)

        # The keycodes from InputEvents.js are Android keycodes.
        # We need to map them to X11 keysyms/keycodes. This is a complex task.
        # For this example, we'll just print them.
        # A real implementation would need a comprehensive map.
        # keycode_x11 = self.d.keysym_to_keycode(keycode_map.get(keycode))
        # print(f"Key event: action={event.action}, keycode={event.keycode}")

        keysym = ANDROID_TO_X11_KEYCODE_MAP.get(event.keycode, -1)
        if keysym != -1:
            press_ = X.KeyPress if event.action == KeyEvent.ACTION_DOWN else X.KeyRelease
            modifiers = []
            if  (event.metastate & KeyEvent.META_CTRL_ON) != 0:
                modifiers.append(self.d.keysym_to_keycode(XK.XK_Control_L))

            if  (event.metastate & KeyEvent.META_SHIFT_ON) != 0:
                modifiers.append(self.d.keysym_to_keycode(XK.XK_Shift_L))

            if  (event.metastate & KeyEvent.META_ALT_ON) != 0:
                modifiers.append(self.d.keysym_to_keycode(XK.XK_Alt_L))

            # if  (event.metastate & KeyEvent.META_ALT_ON) != 0:
            #     modifiers.append(self.d.keysym_to_keycode(XK.XK_Meta_L))

            for modifier_ in modifiers:
                xtest.fake_input(self.d, press_, modifier_)

            keycode_x11 = self.d.keysym_to_keycode(keysym)
            xtest.fake_input(
                self.d, press_, keycode_x11
            )
            self.d.sync()

    def handle_message(self, message: bytes):
        if not isinstance(message, bytes) or len(message) == 0:
            return

        msg_type = message[0]
        if msg_type == ControlMessage.TYPE_INJECT_TOUCH_EVENT:
            self.inject_touch_event(message)
        elif msg_type == ControlMessage.TYPE_INJECT_KEYCODE:  # TYPE_INJECT_KEYCODE
            self.inject_key_event(message)


class X11StreamTrack(VideoStreamTrack):
    """
    A VideoStreamTrack that reads from an X11 display using ffmpeg's x11grab.
    """
    kind = "video"

    def __init__(self, display=X11_DISPLAY, options=None):
        super().__init__()
        self.display = display

        # specific x11grab options are usually required
        self.options = options or {
            'framerate': '30',
            'video_size': '1920x1080',
            'draw_mouse': '1'  # Set to '0' to hide cursor
        }

        self.container = None
        self.stream = None
        self._start_time = None
        self._iterator = None

    def _ensure_open(self):
        """Opens the PyAV container if not already open."""
        if self.container is None:
            # format='x11grab' tells PyAV to use the X11 capture device
            self.container = av.open(
                file=self.display,
                format='x11grab',
                options=self.options
            )
            self.stream = self.container.streams.video[0]
            # Create a demuxing iterator
            self._iterator = self.container.demux(self.stream)

    def _read_next_frame(self):
        """
        Blocking function to pull the next frame from x11grab.
        Must run in an executor.
        """
        self._ensure_open()

        # Iterate through packets until we find a valid video frame
        for packet in self._iterator:
            for frame in packet.decode():
                # We found a frame, return it immediately
                return frame
        return None

    async def recv(self):
        """
        Called by aiortc to get the next frame.
        """
        # Run the blocking PyAV read in a separate thread to avoid freezing asyncio
        loop = asyncio.get_running_loop()
        frame = await loop.run_in_executor(None, self._read_next_frame)

        if frame is None:
            # End of stream (unlikely for x11grab, but good practice)
            self.stop()
            raise Exception("X11 Capture ended")

        # Handle Timing
        # x11grab provides frames with a PTS (Presentation Time Stamp).
        # We need to ensure the time_base aligns with what aiortc expects.
        # Usually passing the raw frame works, but resetting the clock
        # to the start of the stream is safer for WebRTC.

        if self._start_time is None:
            self._start_time = time.time()
            self._start_pts = frame.pts

        # Calculate a new PTS based on wall clock time to ensure smooth playback
        # (Optional: You can also just trust frame.pts if x11grab is stable)
        pts, time_base = await self.next_timestamp()
        # frame.pts is ok but not in 90000 rtp timescale
        # maybe i should convert timescale here instead of generating a new one
        # print(frame.pts, pts, frame.time_base, time_base)
        frame.pts = pts
        frame.time_base = time_base

        return frame

    def stop(self):
        super().stop()
        if self.container:
            self.container.close()


class FlagVideoStreamTrack(VideoStreamTrack):
    """
    A video track that returns an animated flag.
    """

    def __init__(self):
        super().__init__()  # don't forget this!
        self.counter = 0
        height, width = 480, 640

        # generate flag
        data_bgr = numpy.hstack(
            [
                self._create_rectangle(
                    width=213, height=480, color=(255, 0, 0)
                ),  # blue
                self._create_rectangle(
                    width=214, height=480, color=(255, 255, 255)
                ),  # white
                self._create_rectangle(width=213, height=480, color=(0, 0, 255)),  # red
            ]
        )

        # shrink and center it
        M = numpy.float32([[0.5, 0, width / 4], [0, 0.5, height / 4]])
        data_bgr = cv2.warpAffine(data_bgr, M, (width, height))

        # compute animation
        omega = 2 * math.pi / height
        id_x = numpy.tile(numpy.array(range(width), dtype=numpy.float32), (height, 1))
        id_y = numpy.tile(
            numpy.array(range(height), dtype=numpy.float32), (width, 1)
        ).transpose()

        self.frames = []
        for k in range(30):
            phase = 2 * k * math.pi / 30
            map_x = id_x + 10 * numpy.cos(omega * id_x + phase)
            map_y = id_y + 10 * numpy.sin(omega * id_x + phase)
            self.frames.append(
                VideoFrame.from_ndarray(
                    cv2.remap(data_bgr, map_x, map_y, cv2.INTER_LINEAR), format="bgr24"
                )
            )

    async def recv(self):
        pts, time_base = await self.next_timestamp()

        frame = self.frames[self.counter % 30]
        frame.pts = pts
        frame.time_base = time_base
        self.counter += 1
        return frame

    def _create_rectangle(self, width, height, color):
        data_bgr = numpy.zeros((height, width, 3), numpy.uint8)
        data_bgr[:, :] = color
        return data_bgr


def create_local_tracks(
        play_from: str, decode: bool
) -> tuple[Optional[MediaStreamTrack], Optional[MediaStreamTrack]]:
    global relay, webcam
    return None, X11StreamTrack()
    # return None, FlagVideoStreamTrack()

    if play_from:
        # If a file name was given, play from that file.
        player = MediaPlayer(play_from, decode=decode)
        return player.audio, player.video
    else:
        # Otherwise, play from the system's default webcam.
        #
        # In order to serve the same webcam to multiple users we make use of
        # a `MediaRelay`. The webcam will stay open, so it is our responsability
        # to stop the webcam when the application shuts down in `on_shutdown`.
        options = {"framerate": "30", "video_size": "640x480"}
        if relay is None:
            if platform.system() == "Darwin":
                webcam = MediaPlayer(
                    "default:none", format="avfoundation", options=options
                )
            elif platform.system() == "Windows":
                webcam = MediaPlayer(
                    "video=Integrated Camera", format="dshow", options=options
                )
            else:
                webcam = MediaPlayer("/dev/video0", format="v4l2", options=options)
            relay = MediaRelay()
        return None, relay.subscribe(webcam.video)


def force_codec(pc: RTCPeerConnection, sender: RTCRtpSender, forced_codec: str) -> None:
    kind = forced_codec.split("/")[0]
    codecs = RTCRtpSender.getCapabilities(kind).codecs
    print(codecs)
    transceiver = next(t for t in pc.getTransceivers() if t.sender == sender)
    transceiver.setCodecPreferences(
        [codec for codec in codecs if codec.mimeType == forced_codec]
    )


async def index(request: web.Request) -> web.Response:
    content = open(os.path.join(ROOT, "index.html"), "r").read()
    return web.Response(content_type="text/html", text=content)


async def javascript(request: web.Request) -> web.Response:
    with open(os.path.join(ROOT, request.path[1:]), "r") as fd:
        content = fd.read()
        return web.Response(content_type="application/javascript", text=content)

async def get_ice_servers(request: web.Request) -> web.Response:
    return web.Response(
        content_type="application/json",
        text=json.dumps(ice_servers),
    )

async def offer(request: web.Request) -> web.Response:
    params = await request.json()
    offer = RTCSessionDescription(sdp=params["sdp"], type=params["type"])

    typed_ice_servers = []
    for ice_server in ice_servers:
        s = RTCIceServer(urls=ice_server["urls"], username=ice_server.get("username"), credential=ice_server.get("credential"))
        typed_ice_servers.append(s)
        print("ice_server", s)

    config = RTCConfiguration(iceServers=typed_ice_servers)
    pc = RTCPeerConnection(config)
    pcs.add(pc)

    @pc.on("connectionstatechange")
    async def on_connectionstatechange() -> None:
        print("Connection state is %s" % pc.connectionState)
        if pc.connectionState == "failed":
            await pc.close()
            pcs.discard(pc)

    # Create an injector for the target display
    try:
        # injector = X11Injector(display_str=X11_DISPLAY)
        # injector = NoopInputInjector()
        injector = FifoInjector(fifo_path="/tmp/input.fifo")
    except Exception as e:
        print(f"Could not create X11 injector: {e}")
        injector = None

    @pc.on("datachannel")
    def on_datachannel(channel):
        @channel.on("message")
        def on_message(message):
            if injector and isinstance(message, bytes):
                injector.handle_message(message)
            elif isinstance(message, str) and message.startswith("ping"):
                channel.send("pong" + message[4:])

    # open media source
    audio, video = create_local_tracks(
        args.play_from, decode=not args.play_without_decoding
    )

    if audio:
        audio_sender = pc.addTrack(audio)
        if args.audio_codec:
            force_codec(pc, audio_sender, args.audio_codec)
        elif args.play_without_decoding:
            raise Exception("You must specify the audio codec using --audio-codec")

    if video:
        video_sender = pc.addTrack(video)
        if args.video_codec:
            force_codec(pc, video_sender, args.video_codec)
        elif args.play_without_decoding:
            raise Exception("You must specify the video codec using --video-codec")

    await pc.setRemoteDescription(offer)

    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)

    return web.Response(
        content_type="application/json",
        text=json.dumps(
            {"sdp": pc.localDescription.sdp, "type": pc.localDescription.type}
        ),
    )


async def on_shutdown(app: web.Application) -> None:
    # Close peer connections.
    coros = [pc.close() for pc in pcs]
    await asyncio.gather(*coros)
    pcs.clear()

    # If a shared webcam was opened, stop it.
    if webcam is not None:
        webcam.video.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="WebRTC webcam demo")
    parser.add_argument("--cert-file", help="SSL certificate file (for HTTPS)")
    parser.add_argument("--key-file", help="SSL key file (for HTTPS)")
    parser.add_argument("--play-from", help="Read the media from a file and sent it.")
    parser.add_argument(
        "--play-without-decoding",
        help=(
            "Read the media without decoding it (experimental). "
            "For now it only works with an MPEGTS container with only H.264 video."
        ),
        action="store_true",
    )
    parser.add_argument(
        "--host", default="0.0.0.0", help="Host for HTTP server (default: 0.0.0.0)"
    )
    parser.add_argument(
        "--port", type=int, default=8080, help="Port for HTTP server (default: 8080)"
    )
    parser.add_argument("--verbose", "-v", action="count")
    parser.add_argument(
        "--audio-codec", help="Force a specific audio codec (e.g. audio/opus)"
    )
    parser.add_argument(
        "--video-codec", help="Force a specific video codec (e.g. video/H264)"
    )
    parser.add_argument("--turn-url", help="TURN server URL (e.g. turn:turn.example.com:3478)")
    parser.add_argument("--turn-username", help="TURN server username")
    parser.add_argument("--turn-password", help="TURN server password")

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    else:
        logging.basicConfig(level=logging.INFO)

    if args.cert_file:
        ssl_context = ssl.SSLContext()
        ssl_context.load_cert_chain(args.cert_file, args.key_file)
    else:
        ssl_context = None

    # Populate ICE servers
    ice_servers.append({"urls": "stun:stun.l.google.com:19302"})
    if args.turn_url:
        ice_servers.append({
            "urls": args.turn_url,
            "username": args.turn_username,
            "credential": args.turn_password,
        })

    app = web.Application()
    app.on_shutdown.append(on_shutdown)
    app.router.add_get("/", index)
    app.router.add_get("/client.js", javascript)
    app.router.add_get("/InputEvents.js", javascript)
    app.router.add_get("/ice-servers", get_ice_servers)
    app.router.add_post("/offer", offer)
    web.run_app(app, host=args.host, port=args.port, ssl_context=ssl_context)
