const MotionEvent = {
    ACTION_DOWN: 0x00,
    ACTION_UP: 0x01,
    ACTION_MOVE: 0x02,
    BUTTON_PRIMARY: 1 << 0
};
const ControlMessage = {
    TYPE_INJECT_KEYCODE: 0x00,
    TYPE_INJECT_TOUCH_EVENT: 0x02,
    TYPE_START_APP: 16,
    TYPE_START_APP_WITH_INTENT: 18,
};

class DataOutputStream {
    constructor(dataSize) {
        this.buffer = new ArrayBuffer(dataSize);
        this.view = new DataView(this.buffer);
        this.offset = 0;
    }

    writeByte(b) {
        this.view.setUint8(this.offset++, b);
    }

    writeLong(v) {
        this.view.setBigInt64(this.offset, BigInt(v), false); // false = big-endian like Java
        this.offset += 8
    }

    writeInt(x) {
        this.view.setInt32(this.offset, x | 0, false);
        this.offset += 4
    }

    writeShort(s) {
        this.view.setInt16(this.offset, s | 0, false);
        this.offset += 2
    }

    toUint8Array() {
        return new Uint8Array(this.buffer);
    }

    write(bytes) {
        new Uint8Array(this.buffer).set(bytes, this.offset);
        this.offset += bytes.length;
    }
}

function createLaunchRobloxPacket() {
    const name = new TextEncoder().encode("com.roblox.client");
    let dos = new DataOutputStream(1 + 1 + name.length);
    dos.writeByte(ControlMessage.TYPE_START_APP);
    dos.writeByte(name.length);
    dos.write(name);
    return dos.toUint8Array();
}

function createLaunchGamePacket() {
    const name = new TextEncoder().encode("com.roblox.client");
    const url = new TextEncoder().encode(`robloxmobile://placeID=${parseInt(placeid)}`);
    let dos = new DataOutputStream(1 + 1 + name.length + 2 + url.length);
    dos.writeByte(ControlMessage.TYPE_START_APP_WITH_INTENT);
    dos.writeByte(name.length);
    dos.write(name);
    dos.writeShort(url.length);
    dos.write(url);
    return dos.toUint8Array();
}

function createTouchEventPacket(x, y, w, h, pressure, actionButton, buttons, action, pointerId) {
    const dos = new DataOutputStream(32)
    dos.writeByte(ControlMessage.TYPE_INJECT_TOUCH_EVENT);
    dos.writeByte(action);
    dos.writeLong(pointerId);
    dos.writeInt(x);
    dos.writeInt(y);
    dos.writeShort(w);
    dos.writeShort(h);
    dos.writeShort((pressure * 65536) | 0);
    dos.writeInt(actionButton);
    dos.writeInt(buttons);
    return dos.toUint8Array();
}

const KeyEvent = {
    KEYCODE_UNKNOWN: 0,
    KEYCODE_SOFT_LEFT: 1,
    KEYCODE_SOFT_RIGHT: 2,
    KEYCODE_HOME: 3,
    KEYCODE_BACK: 4,
    KEYCODE_CALL: 5,
    KEYCODE_ENDCALL: 6,
    KEYCODE_0: 7,
    KEYCODE_1: 8,
    KEYCODE_2: 9,
    KEYCODE_3: 10,
    KEYCODE_4: 11,
    KEYCODE_5: 12,
    KEYCODE_6: 13,
    KEYCODE_7: 14,
    KEYCODE_8: 15,
    KEYCODE_9: 16,
    KEYCODE_STAR: 17,
    KEYCODE_POUND: 18,
    KEYCODE_DPAD_UP: 19,
    KEYCODE_DPAD_DOWN: 20,
    KEYCODE_DPAD_LEFT: 21,
    KEYCODE_DPAD_RIGHT: 22,
    KEYCODE_DPAD_CENTER: 23,
    KEYCODE_VOLUME_UP: 24,
    KEYCODE_VOLUME_DOWN: 25,
    KEYCODE_POWER: 26,
    KEYCODE_CAMERA: 27,
    KEYCODE_CLEAR: 28,
    KEYCODE_A: 29,
    KEYCODE_B: 30,
    KEYCODE_C: 31,
    KEYCODE_D: 32,
    KEYCODE_E: 33,
    KEYCODE_F: 34,
    KEYCODE_G: 35,
    KEYCODE_H: 36,
    KEYCODE_I: 37,
    KEYCODE_J: 38,
    KEYCODE_K: 39,
    KEYCODE_L: 40,
    KEYCODE_M: 41,
    KEYCODE_N: 42,
    KEYCODE_O: 43,
    KEYCODE_P: 44,
    KEYCODE_Q: 45,
    KEYCODE_R: 46,
    KEYCODE_S: 47,
    KEYCODE_T: 48,
    KEYCODE_U: 49,
    KEYCODE_V: 50,
    KEYCODE_W: 51,
    KEYCODE_X: 52,
    KEYCODE_Y: 53,
    KEYCODE_Z: 54,
    KEYCODE_COMMA: 55,
    KEYCODE_PERIOD: 56,
    KEYCODE_ALT_LEFT: 57,
    KEYCODE_ALT_RIGHT: 58,
    KEYCODE_SHIFT_LEFT: 59,
    KEYCODE_SHIFT_RIGHT: 60,
    KEYCODE_TAB: 61,
    KEYCODE_SPACE: 62,
    KEYCODE_SYM: 63,
    KEYCODE_EXPLORER: 64,
    KEYCODE_ENVELOPE: 65,
    KEYCODE_ENTER: 66,
    KEYCODE_DEL: 67,
    KEYCODE_GRAVE: 68,
    KEYCODE_MINUS: 69,
    KEYCODE_EQUALS: 70,
    KEYCODE_LEFT_BRACKET: 71,
    KEYCODE_RIGHT_BRACKET: 72,
    KEYCODE_BACKSLASH: 73,
    KEYCODE_SEMICOLON: 74,
    KEYCODE_APOSTROPHE: 75,
    KEYCODE_SLASH: 76,
    KEYCODE_AT: 77,
    KEYCODE_NUM: 78,
    KEYCODE_HEADSETHOOK: 79,
    KEYCODE_FOCUS: 80,
    KEYCODE_PLUS: 81,
    KEYCODE_MENU: 82,
    KEYCODE_NOTIFICATION: 83,
    KEYCODE_SEARCH: 84,
    KEYCODE_MEDIA_PLAY_PAUSE: 85,
    KEYCODE_MEDIA_STOP: 86,
    KEYCODE_MEDIA_NEXT: 87,
    KEYCODE_MEDIA_PREVIOUS: 88,
    KEYCODE_MEDIA_REWIND: 89,
    KEYCODE_MEDIA_FAST_FORWARD: 90,
    KEYCODE_MUTE: 91,
    KEYCODE_PAGE_UP: 92,
    KEYCODE_PAGE_DOWN: 93,
    KEYCODE_PICTSYMBOLS: 94,
    KEYCODE_SWITCH_CHARSET: 95,
    KEYCODE_BUTTON_A: 96,
    KEYCODE_BUTTON_B: 97,
    KEYCODE_BUTTON_C: 98,
    KEYCODE_BUTTON_X: 99,
    KEYCODE_BUTTON_Y: 100,
    KEYCODE_BUTTON_Z: 101,
    KEYCODE_BUTTON_L1: 102,
    KEYCODE_BUTTON_R1: 103,
    KEYCODE_BUTTON_L2: 104,
    KEYCODE_BUTTON_R2: 105,
    KEYCODE_BUTTON_THUMBL: 106,
    KEYCODE_BUTTON_THUMBR: 107,
    KEYCODE_BUTTON_START: 108,
    KEYCODE_BUTTON_SELECT: 109,
    KEYCODE_BUTTON_MODE: 110,
    KEYCODE_ESCAPE: 111,
    KEYCODE_FORWARD_DEL: 112,
    KEYCODE_CTRL_LEFT: 113,
    KEYCODE_CTRL_RIGHT: 114,
    KEYCODE_CAPS_LOCK: 115,
    KEYCODE_SCROLL_LOCK: 116,
    KEYCODE_META_LEFT: 117,
    KEYCODE_META_RIGHT: 118,
    KEYCODE_FUNCTION: 119,
    KEYCODE_SYSRQ: 120,
    KEYCODE_BREAK: 121,
    KEYCODE_MOVE_HOME: 122,
    KEYCODE_MOVE_END: 123,
    KEYCODE_INSERT: 124,
    KEYCODE_FORWARD: 125,
    KEYCODE_MEDIA_PLAY: 126,
    KEYCODE_MEDIA_PAUSE: 127,
    KEYCODE_MEDIA_CLOSE: 128,
    KEYCODE_MEDIA_EJECT: 129,
    KEYCODE_MEDIA_RECORD: 130,
    KEYCODE_F1: 131,
    KEYCODE_F2: 132,
    KEYCODE_F3: 133,
    KEYCODE_F4: 134,
    KEYCODE_F5: 135,
    KEYCODE_F6: 136,
    KEYCODE_F7: 137,
    KEYCODE_F8: 138,
    KEYCODE_F9: 139,
    KEYCODE_F10: 140,
    KEYCODE_F11: 141,
    KEYCODE_F12: 142,
    KEYCODE_NUM_LOCK: 143,
    KEYCODE_NUMPAD_0: 144,
    KEYCODE_NUMPAD_1: 145,
    KEYCODE_NUMPAD_2: 146,
    KEYCODE_NUMPAD_3: 147,
    KEYCODE_NUMPAD_4: 148,
    KEYCODE_NUMPAD_5: 149,
    KEYCODE_NUMPAD_6: 150,
    KEYCODE_NUMPAD_7: 151,
    KEYCODE_NUMPAD_8: 152,
    KEYCODE_NUMPAD_9: 153,
    KEYCODE_NUMPAD_DIVIDE: 154,
    KEYCODE_NUMPAD_MULTIPLY: 155,
    KEYCODE_NUMPAD_SUBTRACT: 156,
    KEYCODE_NUMPAD_ADD: 157,
    KEYCODE_NUMPAD_DOT: 158,
    KEYCODE_NUMPAD_COMMA: 159,
    KEYCODE_NUMPAD_ENTER: 160,
    KEYCODE_NUMPAD_EQUALS: 161,
    KEYCODE_NUMPAD_LEFT_PAREN: 162,
    KEYCODE_NUMPAD_RIGHT_PAREN: 163,
    KEYCODE_VOLUME_MUTE: 164,
    KEYCODE_INFO: 165,
    KEYCODE_CHANNEL_UP: 166,
    KEYCODE_CHANNEL_DOWN: 167,
    KEYCODE_ZOOM_IN: 168,
    KEYCODE_ZOOM_OUT: 169,
    KEYCODE_TV: 170,
    KEYCODE_WINDOW: 171,
    KEYCODE_GUIDE: 172,
    KEYCODE_DVR: 173,
    KEYCODE_BOOKMARK: 174,
    KEYCODE_CAPTIONS: 175,
    KEYCODE_SETTINGS: 176,
    KEYCODE_TV_POWER: 177,
    KEYCODE_TV_INPUT: 178,
    KEYCODE_STB_POWER: 179,
    KEYCODE_STB_INPUT: 180,
    KEYCODE_AVR_POWER: 181,
    KEYCODE_AVR_INPUT: 182,
    KEYCODE_PROG_RED: 183,
    KEYCODE_PROG_GREEN: 184,
    KEYCODE_PROG_YELLOW: 185,
    KEYCODE_PROG_BLUE: 186,
    KEYCODE_APP_SWITCH: 187,
    KEYCODE_BUTTON_1: 188,
    KEYCODE_BUTTON_2: 189,
    KEYCODE_BUTTON_3: 190,
    KEYCODE_BUTTON_4: 191,
    KEYCODE_BUTTON_5: 192,
    KEYCODE_BUTTON_6: 193,
    KEYCODE_BUTTON_7: 194,
    KEYCODE_BUTTON_8: 195,
    KEYCODE_BUTTON_9: 196,
    KEYCODE_BUTTON_10: 197,
    KEYCODE_BUTTON_11: 198,
    KEYCODE_BUTTON_12: 199,
    KEYCODE_BUTTON_13: 200,
    KEYCODE_BUTTON_14: 201,
    KEYCODE_BUTTON_15: 202,
    KEYCODE_BUTTON_16: 203,
    KEYCODE_LANGUAGE_SWITCH: 204,
    KEYCODE_MANNER_MODE: 205,
    KEYCODE_3D_MODE: 206,
    KEYCODE_CONTACTS: 207,
    KEYCODE_CALENDAR: 208,
    KEYCODE_MUSIC: 209,
    KEYCODE_CALCULATOR: 210,
    KEYCODE_ZENKAKU_HANKAKU: 211,
    KEYCODE_EISU: 212,
    KEYCODE_MUHENKAN: 213,
    KEYCODE_HENKAN: 214,
    KEYCODE_KATAKANA_HIRAGANA: 215,
    KEYCODE_YEN: 216,
    KEYCODE_RO: 217,
    KEYCODE_KANA: 218,
    KEYCODE_ASSIST: 219,
    KEYCODE_BRIGHTNESS_DOWN: 220,
    KEYCODE_BRIGHTNESS_UP: 221,
    KEYCODE_MEDIA_AUDIO_TRACK: 222,
    KEYCODE_SLEEP: 223,
    KEYCODE_WAKEUP: 224,
    KEYCODE_PAIRING: 225,
    KEYCODE_MEDIA_TOP_MENU: 226,
    KEYCODE_11: 227,
    KEYCODE_12: 228,
    KEYCODE_LAST_CHANNEL: 229,
    KEYCODE_TV_DATA_SERVICE: 230,
    KEYCODE_VOICE_ASSIST: 231,
    KEYCODE_TV_RADIO_SERVICE: 232,
    KEYCODE_TV_TELETEXT: 233,
    KEYCODE_TV_NUMBER_ENTRY: 234,
    KEYCODE_TV_TERRESTRIAL_ANALOG: 235,
    KEYCODE_TV_TERRESTRIAL_DIGITAL: 236,
    KEYCODE_TV_SATELLITE: 237,
    KEYCODE_TV_SATELLITE_BS: 238,
    KEYCODE_TV_SATELLITE_CS: 239,
    KEYCODE_TV_SATELLITE_SERVICE: 240,
    KEYCODE_TV_NETWORK: 241,
    KEYCODE_TV_ANTENNA_CABLE: 242,
    KEYCODE_TV_INPUT_HDMI_1: 243,
    KEYCODE_TV_INPUT_HDMI_2: 244,
    KEYCODE_TV_INPUT_HDMI_3: 245,
    KEYCODE_TV_INPUT_HDMI_4: 246,
    KEYCODE_TV_INPUT_COMPOSITE_1: 247,
    KEYCODE_TV_INPUT_COMPOSITE_2: 248,
    KEYCODE_TV_INPUT_COMPONENT_1: 249,
    KEYCODE_TV_INPUT_COMPONENT_2: 250,
    KEYCODE_TV_INPUT_VGA_1: 251,
    KEYCODE_TV_AUDIO_DESCRIPTION: 252,
    KEYCODE_TV_AUDIO_DESCRIPTION_MIX_UP: 253,
    KEYCODE_TV_AUDIO_DESCRIPTION_MIX_DOWN: 254,
    KEYCODE_TV_ZOOM_MODE: 255,
    KEYCODE_TV_CONTENTS_MENU: 256,
    KEYCODE_TV_MEDIA_CONTEXT_MENU: 257,
    KEYCODE_TV_TIMER_PROGRAMMING: 258,
    KEYCODE_HELP: 259,
    KEYCODE_NAVIGATE_PREVIOUS: 260,
    KEYCODE_NAVIGATE_NEXT: 261,
    KEYCODE_NAVIGATE_IN: 262,
    KEYCODE_NAVIGATE_OUT: 263,
    KEYCODE_STEM_PRIMARY: 264,
    KEYCODE_STEM_1: 265,
    KEYCODE_STEM_2: 266,
    KEYCODE_STEM_3: 267,
    KEYCODE_DPAD_UP_LEFT: 268,
    KEYCODE_DPAD_DOWN_LEFT: 269,
    KEYCODE_DPAD_UP_RIGHT: 270,
    KEYCODE_DPAD_DOWN_RIGHT: 271,
    KEYCODE_MEDIA_SKIP_FORWARD: 272,
    KEYCODE_MEDIA_SKIP_BACKWARD: 273,
    KEYCODE_MEDIA_STEP_FORWARD: 274,
    KEYCODE_MEDIA_STEP_BACKWARD: 275,
    KEYCODE_SOFT_SLEEP: 276,
    KEYCODE_CUT: 277,
    KEYCODE_COPY: 278,
    KEYCODE_PASTE: 279,
    KEYCODE_SYSTEM_NAVIGATION_UP: 280,
    KEYCODE_SYSTEM_NAVIGATION_DOWN: 281,
    KEYCODE_SYSTEM_NAVIGATION_LEFT: 282,
    KEYCODE_SYSTEM_NAVIGATION_RIGHT: 283,
    KEYCODE_ALL_APPS: 284,
    KEYCODE_REFRESH: 285,
    KEYCODE_THUMBS_UP: 286,
    KEYCODE_THUMBS_DOWN: 287,
    KEYCODE_PROFILE_SWITCH: 288,
    KEYCODE_VIDEO_APP_1: 289,
    KEYCODE_VIDEO_APP_2: 290,
    KEYCODE_VIDEO_APP_3: 291,
    KEYCODE_VIDEO_APP_4: 292,
    KEYCODE_VIDEO_APP_5: 293,
    KEYCODE_VIDEO_APP_6: 294,
    KEYCODE_VIDEO_APP_7: 295,
    KEYCODE_VIDEO_APP_8: 296,
    KEYCODE_FEATURED_APP_1: 297,
    KEYCODE_FEATURED_APP_2: 298,
    KEYCODE_FEATURED_APP_3: 299,
    KEYCODE_FEATURED_APP_4: 300,
    KEYCODE_DEMO_APP_1: 301,
    KEYCODE_DEMO_APP_2: 302,
    KEYCODE_DEMO_APP_3: 303,
    KEYCODE_DEMO_APP_4: 304,
    KEYCODE_KEYBOARD_BACKLIGHT_DOWN: 305,
    KEYCODE_KEYBOARD_BACKLIGHT_UP: 306,
    KEYCODE_KEYBOARD_BACKLIGHT_TOGGLE: 307,
    KEYCODE_STYLUS_BUTTON_PRIMARY: 308,
    KEYCODE_STYLUS_BUTTON_SECONDARY: 309,
    KEYCODE_STYLUS_BUTTON_TERTIARY: 310,
    KEYCODE_STYLUS_BUTTON_TAIL: 311,
    KEYCODE_RECENT_APPS: 312,
    KEYCODE_MACRO_1: 313,
    KEYCODE_MACRO_2: 314,
    KEYCODE_MACRO_3: 315,
    KEYCODE_MACRO_4: 316,
    KEYCODE_EMOJI_PICKER: 317,
    KEYCODE_SCREENSHOT: 318,
    MAX_KEYCODE: 84,
    ACTION_DOWN: 0,
    ACTION_UP: 1,
    ACTION_MULTIPLE: 2,
    META_CAP_LOCKED: 0x100,
    META_ALT_LOCKED: 0x200,
    META_SYM_LOCKED: 0x400,
    META_SELECTING: 0x800,
    META_ALT_ON: 0x02,
    META_ALT_LEFT_ON: 0x10,
    META_ALT_RIGHT_ON: 0x20,
    META_SHIFT_ON: 0x1,
    META_SHIFT_LEFT_ON: 0x40,
    META_SHIFT_RIGHT_ON: 0x80,
    META_SYM_ON: 0x4,
    META_FUNCTION_ON: 0x8,
    META_CTRL_ON: 0x1000,
    META_CTRL_LEFT_ON: 0x2000,
    META_CTRL_RIGHT_ON: 0x4000,
    META_META_ON: 0x10000,
    META_META_LEFT_ON: 0x20000,
    META_META_RIGHT_ON: 0x40000,
    META_CAPS_LOCK_ON: 0x100000,
    META_NUM_LOCK_ON: 0x200000,
    META_SCROLL_LOCK_ON: 0x400000,
    FLAG_WOKE_HERE: 0x1,
    FLAG_SOFT_KEYBOARD: 0x2,
    FLAG_KEEP_TOUCH_MODE: 0x4,
    FLAG_FROM_SYSTEM: 0x8,
    FLAG_EDITOR_ACTION: 0x10,
    FLAG_VIRTUAL_HARD_KEY: 0x40,
    FLAG_LONG_PRESS: 0x80,
    FLAG_CANCELED_LONG_PRESS: 0x100,
    FLAG_TRACKING: 0x200,
    FLAG_FALLBACK: 0x400,
    FLAG_PREDISPATCH: 0x20000000,
    FLAG_START_TRACKING: 0x40000000,
};

class Pointers {
    constructor() {
        this.pointerStates = new Map()
        this.pointerMap = new Map()
        this.availableIds = [0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1];
        this.pointerUpdateCounter = 0;
    }

    static isPointerRawUpdateSupported() {
        return 'onpointerrawupdate' in window || 'onpointerrawupdate' in document.createElement('div');
    }


    updatePointer(pointerId) {
        const nowMsec = performance.now();
        this.pointerStates.set(pointerId, nowMsec);
        for (let k of this.pointerStates.keys()) {
            let lastUpdated = this.pointerStates.get(k);
            let threshold = nowMsec - 200;
            if (lastUpdated < threshold) {
                console.log(`Deleting pointer ${k} ${lastUpdated} < ${threshold}`);
                this.deletePointer(k)
            }
        }
        if (!this.pointerMap.has(pointerId)) {
            let assignedId = pointerId;
            for (let i = 2; i < 12; i++) {
                if (this.availableIds[i] === 1) {
                    this.availableIds[i] = 0;
                    assignedId = i;
                    break;
                }
            }
            console.log(pointerId, '->', assignedId);
            this.pointerMap.set(pointerId, assignedId);
        }
        return this.pointerMap.get(pointerId);
    }

    deletePointer(pointerId) {
        let assignedId = this.pointerMap.get(pointerId);
        if (assignedId < this.availableIds.length) {
            this.availableIds[assignedId] = 1;
        }
        this.pointerStates.delete(pointerId);
        this.pointerMap.delete(pointerId);
        return assignedId;
    }

    packetFromEvent(e) {
        // console.log(e.buttons, e.offsetX, e.offsetY, e.pressure, e.pointerType, e);
        // if (!dataChannel || e.buttons === 0)
        //     return;
        this.pointerUpdateCounter = this.pointerUpdateCounter | 0;
        this.pointerUpdateCounter++;
        if ((this.pointerUpdateCounter & 1) === 0) {
            return null;
        }

        let pointerId = e.pointerId;
        if (e.pointerType === "touch") {
            pointerId = this.updatePointer(e.pointerId);
        }
        // console.log("pointer update sent");
        return createTouchEventPacketFromEvent(MotionEvent.ACTION_MOVE, pointerId, e);
    }

}

function getIntrinsicXY(video, event) {
    const rect = video.getBoundingClientRect();
    const videoWidth = rect.width;
    const videoHeight = rect.height;

    // Get video's intrinsic dimensions
    const intrinsicWidth = video.videoWidth;
    const intrinsicHeight = video.videoHeight;

    // Prevent clicks if video dimensions aren't available (e.g., video not loaded)
    if (intrinsicWidth === 0 || intrinsicHeight === 0) {
        // console.warn('Video dimensions not available yet.');
        return [0, 0];
    }

    // Calculate aspect ratio and scaling factors
    const aspectRatio = intrinsicWidth / intrinsicHeight;
    const containerAspectRatio = videoWidth / videoHeight;

    let scaledWidth, scaledHeight, offsetX, offsetY;

    if (aspectRatio > containerAspectRatio) {
        // Video is wider than container: letterboxed vertically
        scaledWidth = videoWidth;
        scaledHeight = videoWidth / aspectRatio;
        offsetX = 0;
        offsetY = (videoHeight - scaledHeight) / 2; // Center vertically
    } else {
        // Video is taller than container: letterboxed horizontally
        scaledHeight = videoHeight;
        scaledWidth = videoHeight * aspectRatio;
        offsetX = (videoWidth - scaledWidth) / 2; // Center horizontally
        offsetY = 0;
    }

    // Get click coordinates relative to video element
    const clickX = event.clientX - rect.left;
    const clickY = event.clientY - rect.top;

    // Check if click is within the actual video content (not letterbox)
    if (
        clickX < offsetX ||
        clickX > offsetX + scaledWidth ||
        clickY < offsetY ||
        clickY > offsetY + scaledHeight
    ) {
        // console.warn('Click is in letterbox area, not on video content.');
        return [0, 0];
    }

    // Convert click coordinates to intrinsic video coordinates
    const videoClickX = ((clickX - offsetX) / scaledWidth) * intrinsicWidth;
    const videoClickY = ((clickY - offsetY) / scaledHeight) * intrinsicHeight;

    // Ensure coordinates are within bounds
    const finalX = Math.max(0, Math.min(intrinsicWidth, Math.round(videoClickX)));
    const finalY = Math.max(0, Math.min(intrinsicHeight, Math.round(videoClickY)));

    // console.log(`Click at intrinsic coordinates: (${finalX}, ${finalY})`);
    // Use finalX and finalY as needed (e.g., for annotations, hotspots, etc.)
    return [finalX, finalY];
}

function createTouchEventPacketFromEvent(action, pointerId, e) {
    const [x, y] = getIntrinsicXY(e.target, e)
    const w = e.target.videoWidth;
    const h = e.target.videoHeight;
    const pressure = e.pressure;
    const actionButton = e.button;
    const buttons = e.buttons;
    return createTouchEventPacket(x, y, w, h, pressure, actionButton, buttons, action, pointerId);
}

const CodeToKeyEvent = {
    // Letters
    "KeyA": KeyEvent.KEYCODE_A,
    "KeyB": KeyEvent.KEYCODE_B,
    "KeyC": KeyEvent.KEYCODE_C,
    "KeyD": KeyEvent.KEYCODE_D,
    "KeyE": KeyEvent.KEYCODE_E,
    "KeyF": KeyEvent.KEYCODE_F,
    "KeyG": KeyEvent.KEYCODE_G,
    "KeyH": KeyEvent.KEYCODE_H,
    "KeyI": KeyEvent.KEYCODE_I,
    "KeyJ": KeyEvent.KEYCODE_J,
    "KeyK": KeyEvent.KEYCODE_K,
    "KeyL": KeyEvent.KEYCODE_L,
    "KeyM": KeyEvent.KEYCODE_M,
    "KeyN": KeyEvent.KEYCODE_N,
    "KeyO": KeyEvent.KEYCODE_O,
    "KeyP": KeyEvent.KEYCODE_P,
    "KeyQ": KeyEvent.KEYCODE_Q,
    "KeyR": KeyEvent.KEYCODE_R,
    "KeyS": KeyEvent.KEYCODE_S,
    "KeyT": KeyEvent.KEYCODE_T,
    "KeyU": KeyEvent.KEYCODE_U,
    "KeyV": KeyEvent.KEYCODE_V,
    "KeyW": KeyEvent.KEYCODE_W,
    "KeyX": KeyEvent.KEYCODE_X,
    "KeyY": KeyEvent.KEYCODE_Y,
    "KeyZ": KeyEvent.KEYCODE_Z,

    // Digits
    "Digit0": KeyEvent.KEYCODE_0,
    "Digit1": KeyEvent.KEYCODE_1,
    "Digit2": KeyEvent.KEYCODE_2,
    "Digit3": KeyEvent.KEYCODE_3,
    "Digit4": KeyEvent.KEYCODE_4,
    "Digit5": KeyEvent.KEYCODE_5,
    "Digit6": KeyEvent.KEYCODE_6,
    "Digit7": KeyEvent.KEYCODE_7,
    "Digit8": KeyEvent.KEYCODE_8,
    "Digit9": KeyEvent.KEYCODE_9,

    // Function keys
    "F1": KeyEvent.KEYCODE_F1,
    "F2": KeyEvent.KEYCODE_F2,
    "F3": KeyEvent.KEYCODE_F3,
    "F4": KeyEvent.KEYCODE_F4,
    "F5": KeyEvent.KEYCODE_F5,
    "F6": KeyEvent.KEYCODE_F6,
    "F7": KeyEvent.KEYCODE_F7,
    "F8": KeyEvent.KEYCODE_F8,
    "F9": KeyEvent.KEYCODE_F9,
    "F10": KeyEvent.KEYCODE_F10,
    "F11": KeyEvent.KEYCODE_F11,
    "F12": KeyEvent.KEYCODE_F12,

    // Control keys
    "Enter": KeyEvent.KEYCODE_ENTER,
    "Escape": KeyEvent.KEYCODE_ESCAPE,
    "Backspace": KeyEvent.KEYCODE_DEL,
    "Tab": KeyEvent.KEYCODE_TAB,
    "CapsLock": KeyEvent.KEYCODE_CAPS_LOCK,
    "ShiftLeft": KeyEvent.KEYCODE_SHIFT_LEFT,
    "ShiftRight": KeyEvent.KEYCODE_SHIFT_RIGHT,
    "ControlLeft": KeyEvent.KEYCODE_CTRL_LEFT,
    "ControlRight": KeyEvent.KEYCODE_CTRL_RIGHT,
    "AltLeft": KeyEvent.KEYCODE_ALT_LEFT,
    "AltRight": KeyEvent.KEYCODE_ALT_RIGHT,
    "MetaLeft": KeyEvent.KEYCODE_META_LEFT,
    "MetaRight": KeyEvent.KEYCODE_META_RIGHT,
    "ContextMenu": KeyEvent.KEYCODE_MENU,

    // Navigation
    "ArrowUp": KeyEvent.KEYCODE_DPAD_UP,
    "ArrowDown": KeyEvent.KEYCODE_DPAD_DOWN,
    "ArrowLeft": KeyEvent.KEYCODE_DPAD_LEFT,
    "ArrowRight": KeyEvent.KEYCODE_DPAD_RIGHT,
    "Home": KeyEvent.KEYCODE_MOVE_HOME,
    "End": KeyEvent.KEYCODE_MOVE_END,
    "PageUp": KeyEvent.KEYCODE_PAGE_UP,
    "PageDown": KeyEvent.KEYCODE_PAGE_DOWN,
    "Insert": KeyEvent.KEYCODE_INSERT,
    "Delete": KeyEvent.KEYCODE_FORWARD_DEL,

    // Editing / punctuation
    "Space": KeyEvent.KEYCODE_SPACE,
    "Minus": KeyEvent.KEYCODE_MINUS,
    "Equal": KeyEvent.KEYCODE_EQUALS,
    "BracketLeft": KeyEvent.KEYCODE_LEFT_BRACKET,
    "BracketRight": KeyEvent.KEYCODE_RIGHT_BRACKET,
    "Backslash": KeyEvent.KEYCODE_BACKSLASH,
    "Semicolon": KeyEvent.KEYCODE_SEMICOLON,
    "Quote": KeyEvent.KEYCODE_APOSTROPHE,
    "Comma": KeyEvent.KEYCODE_COMMA,
    "Period": KeyEvent.KEYCODE_PERIOD,
    "Slash": KeyEvent.KEYCODE_SLASH,
    "Backquote": KeyEvent.KEYCODE_GRAVE,

    // Numpad
    "NumLock": KeyEvent.KEYCODE_NUM_LOCK,
    "Numpad0": KeyEvent.KEYCODE_NUMPAD_0,
    "Numpad1": KeyEvent.KEYCODE_NUMPAD_1,
    "Numpad2": KeyEvent.KEYCODE_NUMPAD_2,
    "Numpad3": KeyEvent.KEYCODE_NUMPAD_3,
    "Numpad4": KeyEvent.KEYCODE_NUMPAD_4,
    "Numpad5": KeyEvent.KEYCODE_NUMPAD_5,
    "Numpad6": KeyEvent.KEYCODE_NUMPAD_6,
    "Numpad7": KeyEvent.KEYCODE_NUMPAD_7,
    "Numpad8": KeyEvent.KEYCODE_NUMPAD_8,
    "Numpad9": KeyEvent.KEYCODE_NUMPAD_9,
    "NumpadAdd": KeyEvent.KEYCODE_NUMPAD_ADD,
    "NumpadSubtract": KeyEvent.KEYCODE_NUMPAD_SUBTRACT,
    "NumpadMultiply": KeyEvent.KEYCODE_NUMPAD_MULTIPLY,
    "NumpadDivide": KeyEvent.KEYCODE_NUMPAD_DIVIDE,
    "NumpadDecimal": KeyEvent.KEYCODE_NUMPAD_DOT,
    "NumpadEnter": KeyEvent.KEYCODE_NUMPAD_ENTER,

    // System / media
    "PrintScreen": KeyEvent.KEYCODE_SYSRQ,
    "ScrollLock": KeyEvent.KEYCODE_SCROLL_LOCK,
    "Pause": KeyEvent.KEYCODE_BREAK,
    "VolumeUp": KeyEvent.KEYCODE_VOLUME_UP,
    "VolumeDown": KeyEvent.KEYCODE_VOLUME_DOWN,
    "VolumeMute": KeyEvent.KEYCODE_VOLUME_MUTE,
    "MediaPlayPause": KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
    "MediaNextTrack": KeyEvent.KEYCODE_MEDIA_NEXT,
    "MediaPreviousTrack": KeyEvent.KEYCODE_MEDIA_PREVIOUS,
    "MediaStop": KeyEvent.KEYCODE_MEDIA_STOP,
    "BrowserHome": KeyEvent.KEYCODE_HOME,
};

function createKeyCodePacket(action, keycode, repeat, metastate) {
    const dos = new DataOutputStream(1 + 1 + 4 + 4 + 4);
    dos.writeByte(ControlMessage.TYPE_INJECT_KEYCODE);
    dos.writeByte(action); // KeyEvent.ACTION_UP);
    dos.writeInt(keycode); //KeyEvent.KEYCODE_ENTER);
    dos.writeInt(repeat); // repeat
    dos.writeInt(metastate); // KeyEvent.META_CTRL_ON
    return dos.toUint8Array();
}

function keyboardEventToPacket(e) {
    const action = e.type === "keydown" ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP;
    const keycode = CodeToKeyEvent[e.code] || KeyEvent.KEYCODE_UNKNOWN;
    const repeat = e.repeat;
    let metastate = 0;
    if (e.ctrlKey)
        metastate |= KeyEvent.META_CTRL_ON;
    if (e.shiftKey)
        metastate |= KeyEvent.META_SHIFT_ON;
    if (e.altKey)
        metastate |= KeyEvent.META_ALT_ON;
    // if (e.metaKey)
    //     metastate |= KeyEvent.META_FUNCTION_ON;
    return createKeyCodePacket(action, keycode, repeat, metastate);
}