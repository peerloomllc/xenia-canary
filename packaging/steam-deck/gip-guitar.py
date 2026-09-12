#!/usr/bin/env python3
"""Drive a GIP (Xbox One protocol) guitar from userspace and republish it as an
ordinary controller through /dev/uinput.

Why this exists: on a Steam Deck (SteamOS, kernel 6.16 neptune) the CRKD Les
Paul attaches and never sends a single event. Its `xpad` never completes the
GIP start-up conversation, and Steam's own controller layer does not drive this
kind of device at all. Done properly from userspace the guitar reports fine, so
this speaks the protocol itself and presents the result as an input device
carrying the guitar's own USB ids, which is what SDL and Xenia already know how
to treat as a guitar (notes/77, notes/81).

Needs no kernel module and nothing installed: pure ctypes against libusb-1.0,
which SteamOS ships, and /dev/uinput, which the deck user can already write.

Packet layout follows medusalix/xone bus/protocol.c.
"""
import ctypes, fcntl, os, struct, sys, time

VENDOR = 0x0351
PRODUCTS = (0x4161, 0x1300)   # the guitar's mode dial decides which it is
EP_OUT, EP_IN = 0x01, 0x81

# ---- GIP ------------------------------------------------------------------
ACK, ANNOUNCE, STATUS, IDENTIFY, POWER, LED, INPUT = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x0a, 0x20)
OPT_ACK, OPT_INTERNAL, OPT_CHUNK_START, OPT_CHUNK = 0x10, 0x20, 0x40, 0x80

# ---- uinput ---------------------------------------------------------------
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_DEV_SETUP, UI_ABS_SETUP = 0x405C5503, 0x401C5504
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ = 0, 1, 2, 3, 4, 5
ABS_HAT0X, ABS_HAT0Y = 0x10, 0x11
BTN_A, BTN_B, BTN_X, BTN_Y = 0x130, 0x131, 0x133, 0x134
BTN_TL, BTN_TR = 0x136, 0x137
BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR = (
    0x13a, 0x13b, 0x13c, 0x13d, 0x13e)

# GIP button bits, in the order xpad reports them for an Xbox One pad.
BUTTON_BITS = [
    (2, BTN_START), (3, BTN_SELECT), (4, BTN_A), (5, BTN_B),
    (6, BTN_X), (7, BTN_Y), (12, BTN_TL), (13, BTN_TR),
    (14, BTN_THUMBL), (15, BTN_THUMBR),
]
DPAD_BITS = {8: "up", 9: "down", 10: "left", 11: "right"}

LIBUSB_ERROR_IO, LIBUSB_ERROR_NOT_FOUND = -1, -5
LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_PIPE = -4, -9


class GuitarGone(Exception):
    """The guitar disconnected, which is not a failure: wait for it back."""


lib = ctypes.CDLL("libusb-1.0.so.0")
lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16,
                                                ctypes.c_uint16]
for fn in ("libusb_claim_interface", "libusb_release_interface",
           "libusb_set_auto_detach_kernel_driver"):
    getattr(lib, fn).argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.libusb_interrupt_transfer.argtypes = [ctypes.c_void_p, ctypes.c_ubyte,
                                          ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int,
                                          ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
lib.libusb_close.argtypes = [ctypes.c_void_p]


def varint(val):
    out = []
    while True:
        b = val & 0x7f
        val >>= 7
        out.append(b | (0x80 if val else 0))
        if not val:
            return out


def read_varint(data, i):
    val = shift = 0
    while i < len(data):
        b = data[i]
        i += 1
        val |= (b & 0x7f) << shift
        shift += 7
        if not (b & 0x80):
            break
    return val, i


class Guitar:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.seq = 1
        self.chunks = {}
        ctx = ctypes.c_void_p()
        if lib.libusb_init(ctypes.byref(ctx)) != 0:
            raise RuntimeError("libusb_init failed")
        self.ctx = ctx
        self.handle = None
        for pid in PRODUCTS:
            h = lib.libusb_open_device_with_vid_pid(ctx, VENDOR, pid)
            if h:
                self.handle, self.product = h, pid
                break
        if not self.handle:
            raise RuntimeError("no guitar found (looked for %s)" %
                               ", ".join("%04x:%04x" % (VENDOR, p) for p in PRODUCTS))
        lib.libusb_set_auto_detach_kernel_driver(self.handle, 1)
        if lib.libusb_claim_interface(self.handle, 0) != 0:
            raise RuntimeError("could not claim the guitar's interface")
        self.buf = (ctypes.c_ubyte * 64)()
        self.n = ctypes.c_int(0)

    def _raw(self, data):
        buf = (ctypes.c_ubyte * len(data))(*data)
        return lib.libusb_interrupt_transfer(self.handle, EP_OUT, buf, len(data),
                                             ctypes.byref(self.n), 1000)

    def send(self, cmd, opts, payload):
        self._raw([cmd, opts, self.seq & 0xff] + varint(len(payload)) + list(payload))
        self.seq = (self.seq % 255) + 1

    def _ack(self, cmd, seq, received, remaining):
        pkt = [0x00, cmd, OPT_INTERNAL,
               received & 0xff, (received >> 8) & 0xff, 0x00, 0x00,
               remaining & 0xff, (remaining >> 8) & 0xff]
        self._raw([ACK, OPT_INTERNAL, seq] + varint(len(pkt)) + pkt)

    def read(self, timeout_ms=200):
        """One packet, as (command, payload), or None. Acknowledges what must
        be. Raises when the guitar has gone, which it does whenever its mode
        dial moves: it re-enumerates under a different id."""
        r = lib.libusb_interrupt_transfer(self.handle, EP_IN, self.buf, 64,
                                          ctypes.byref(self.n), timeout_ms)
        if r in (LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_IO,
                 LIBUSB_ERROR_NOT_FOUND, LIBUSB_ERROR_PIPE):
            raise GuitarGone("the guitar went away (libusb %d)" % r)
        if r != 0 or not self.n.value:
            return None
        pkt = bytes(self.buf[:self.n.value])
        cmd, opts, seq = pkt[0], pkt[1], pkt[2]
        length, i = read_varint(pkt, 3)
        offset = 0
        if opts & OPT_CHUNK:
            offset, i = read_varint(pkt, i)
        if opts & OPT_CHUNK:
            # A chunk start carries the total length where the offset goes.
            if opts & OPT_CHUNK_START:
                self.chunks[cmd] = [offset, length]
            else:
                st = self.chunks.setdefault(cmd, [offset + length, 0])
                st[1] = offset + length
            total, got = self.chunks[cmd]
            if opts & OPT_ACK:
                self._ack(cmd, seq, got, max(total - got, 0))
        elif opts & OPT_ACK:
            self._ack(cmd, seq, length, 0)
        return cmd, pkt[i:i + length]

    def start(self):
        """The conversation that makes it report. Every step is needed."""
        self.send(POWER, OPT_INTERNAL, [0x00])
        self._drain(1.0)
        self.send(IDENTIFY, OPT_INTERNAL, [])
        self._drain(3.0)
        self.send(POWER, OPT_INTERNAL, [0x00])
        self.send(LED, 0x00, [0x00, 0x01, 0x14])
        self._drain(1.0)

    def _drain(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.read(100)

    def close(self):
        if self.handle:
            lib.libusb_release_interface(self.handle, 0)
            lib.libusb_close(self.handle)
            self.handle = None


class VirtualPad:
    """A controller carrying the guitar's own USB ids, so SDL gives it the same
    identity it has on a machine where the kernel driver works."""

    def __init__(self, product):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        for _, code in BUTTON_BITS:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        for axis, lo, hi, flat in ((ABS_X, -32768, 32767, 128),
                                   (ABS_Y, -32768, 32767, 128),
                                   (ABS_RX, -32768, 32767, 128),
                                   (ABS_RY, -32768, 32767, 128),
                                   (ABS_Z, 0, 1023, 0),
                                   (ABS_RZ, 0, 1023, 0),
                                   (ABS_HAT0X, -1, 1, 0),
                                   (ABS_HAT0Y, -1, 1, 0)):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, axis)
            # struct uinput_abs_setup: u16 code, then input_absinfo
            fcntl.ioctl(self.fd, UI_ABS_SETUP,
                        struct.pack("Hxx6i", axis, 0, lo, hi, 0, flat, 0))
        name = b"CRKD Guitar (GIP)"
        fcntl.ioctl(self.fd, UI_DEV_SETUP,
                    struct.pack("HHHH80sI", 0x03, VENDOR, product, 0x0101,
                                name, 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.state = {}

    def _emit(self, etype, code, value):
        os.write(self.fd, struct.pack("qqHHi", 0, 0, etype, code, value))

    def send(self, etype, code, value):
        if self.state.get((etype, code)) == value:
            return
        self.state[(etype, code)] = value
        self._emit(etype, code, value)

    def sync(self):
        self._emit(EV_SYN, SYN_REPORT, 0)

    def close(self):
        try:
            fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        finally:
            os.close(self.fd)


def run_once(verbose):
    """One connection: hold it until the guitar goes, then return."""
    guitar = Guitar(verbose)
    print("guitar found: %04x:%04x" % (VENDOR, guitar.product), flush=True)
    guitar.start()
    pad = VirtualPad(guitar.product)
    print("presenting it as an input device; playing is now passed through",
          flush=True)
    reports = 0
    wide = [False]   # set once a value past 10 bits arrives
    try:
        while True:
            got = guitar.read(500)
            if not got:
                continue
            cmd, payload = got
            if cmd != INPUT or len(payload) < 14:
                continue
            # The two axes after the buttons are unsigned: this guitar sweeps
            # its whammy over the full 16 bits in steps of 256, which read as
            # nonsense taken as a signed trigger. Scaled to the 0..1023 a
            # trigger carries, so it arrives as it does on a machine whose
            # kernel driver corrects the range (notes/77, notes/81).
            buttons, tl, tr, lx, ly, rx, ry = struct.unpack("<HHHhhhh", payload[:14])
            for bit, code in BUTTON_BITS:
                pad.send(EV_KEY, code, 1 if buttons & (1 << bit) else 0)
            pad.send(EV_ABS, ABS_HAT0X,
                     (1 if buttons & (1 << 11) else 0) - (1 if buttons & (1 << 10) else 0))
            pad.send(EV_ABS, ABS_HAT0Y,
                     (1 if buttons & (1 << 9) else 0) - (1 if buttons & (1 << 8) else 0))
            # How wide the axis is depends on the guitar's mode dial: one
            # mode sweeps the whole 16 bits in steps of 256, another stays
            # inside 10 bits. Shifting the narrow one crushes the whole bar
            # into a sliver, which reaches a title as about 1.5% of its
            # travel, so work it out from what actually arrives.
            if tl > 1023 or tr > 1023:
                wide[0] = True
            shift = 6 if wide[0] else 0
            pad.send(EV_ABS, ABS_Z, min(tl >> shift, 1023))
            pad.send(EV_ABS, ABS_RZ, min(tr >> shift, 1023))
            pad.send(EV_ABS, ABS_X, lx)
            pad.send(EV_ABS, ABS_Y, -1 - ly if ly < 32767 else -32768)
            pad.send(EV_ABS, ABS_RX, rx)
            pad.send(EV_ABS, ABS_RY, -1 - ry if ry < 32767 else -32768)
            pad.sync()
            reports += 1
            if verbose and reports % 100 == 0:
                print("  %d reports, buttons=%04x tl=%d tr=%d" %
                      (reports, buttons, tl, tr), flush=True)
    finally:
        pad.close()
        guitar.close()
        print("connection ended after %d reports" % reports, flush=True)


def main():
    """Keep the guitar presented for as long as this runs. It disappears and
    comes back under a different id whenever its mode dial moves, and used to
    take the driver with it."""
    verbose = "-v" in sys.argv
    waiting = False
    try:
        while True:
            try:
                run_once(verbose)
                waiting = False
            except GuitarGone as e:
                print("%s; waiting for it to come back" % e, flush=True)
            except RuntimeError as e:
                if not waiting:
                    print("%s; waiting" % e, flush=True)
                    waiting = True
            time.sleep(2)
    except KeyboardInterrupt:
        print("stopped", flush=True)


if __name__ == "__main__":
    main()
