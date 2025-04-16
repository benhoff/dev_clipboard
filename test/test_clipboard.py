import os
import fcntl
import errno
import threading
import select
import pytest
import re

# ------------------------------------------------------------------
# Load CLIPBOARD_CLEAR, with fallback if header not found (for demonstration)
# ------------------------------------------------------------------
def _get_ioctl_clear():
    try:
        hdr = os.path.abspath(os.path.join(os.getcwd(), "clipboard.h"))
        text = open(hdr, "r").read()
        m_magic = re.search(r"#define\s+CLIPBOARD_MAGIC\s+'(.)'", text)
        m_clear = re.search(
            r"#define\s+CLIPBOARD_CLEAR\s+_IO\(\s*CLIPBOARD_MAGIC\s*,\s*(\d+)\)",
            text
        )
        if m_magic and m_clear:
            magic = m_magic.group(1)
            num = int(m_clear.group(1), 10)
            return (ord(magic) << 8) | num
    except Exception:
        pass
    # Fallback default
    return (ord('C') << 8) | 1

CLIPBOARD_CLEAR = _get_ioctl_clear()
DEV = "/dev/clipboard"

@pytest.fixture
def fd():
    """Opens and closes /dev/clipboard around each test."""
    fd = os.open(DEV, os.O_RDWR)
    try:
        yield fd
    finally:
        os.close(fd)

def test_basic_write_read(fd):
    msg = b"pytest-clipboard"
    assert os.write(fd, msg) == len(msg)
    os.lseek(fd, 0, os.SEEK_SET)
    assert os.read(fd, len(msg)) == msg

def test_truncate_and_eof():
    fd_trunc = os.open(DEV, os.O_RDWR | os.O_TRUNC)
    os.close(fd_trunc)
    fd_read = os.open(DEV, os.O_RDONLY)
    try:
        assert os.read(fd_read, 10) == b""
    finally:
        os.close(fd_read)

def test_append(fd):
    os.write(fd, b"foo")
    fd_app = os.open(DEV, os.O_WRONLY | os.O_APPEND)
    try:
        os.write(fd_app, b"bar")
    finally:
        os.close(fd_app)
    os.lseek(fd, 0, os.SEEK_SET)
    assert os.read(fd, 6) == b"foobar"

def test_ioctl_clear(fd):
    os.write(fd, b"12345")
    fcntl.ioctl(fd, CLIPBOARD_CLEAR)
    os.lseek(fd, 0, os.SEEK_SET)
    assert os.read(fd, 1) == b""

def test_bad_ioctl(fd):
    with pytest.raises(OSError) as exc:
        fcntl.ioctl(fd, 0xDEAD)
    assert exc.value.errno in (errno.ENOTTY, errno.EINVAL)

def test_seek_past_and_read(fd):
    os.write(fd, b"ABC")
    os.lseek(fd, 1000, os.SEEK_SET)
    assert os.read(fd, 1) == b""

def _writer(text):
    # Open in O_WRONLY|O_APPEND so each thread appends, not truncates
    fd = os.open(DEV, os.O_WRONLY | os.O_APPEND)
    try:
        os.write(fd, text)
    finally:
        os.close(fd)

def test_concurrent_writes_reads(fd):
    # clear first
    fcntl.ioctl(fd, CLIPBOARD_CLEAR)
    # Spawn 4 threads that each append a single byte
    threads = [threading.Thread(target=_writer, args=(f"{i}".encode(),))
               for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

def test_poll_and_sigio():
    fd_r = os.open(DEV, os.O_RDONLY | os.O_NONBLOCK)
    try:
        flags = fcntl.fcntl(fd_r, fcntl.F_GETFL)
        fcntl.fcntl(fd_r, fcntl.F_SETFL, flags | os.O_ASYNC)
        fcntl.fcntl(fd_r, fcntl.F_SETOWN, os.getpid())

        notified = threading.Event()
        import signal
        signal.signal(signal.SIGIO, lambda s, f: notified.set())

        poller = select.poll()
        poller.register(fd_r, select.POLLIN)

        # write a byte after 0.01s to speed up the test
        threading.Timer(0.01, lambda: open(DEV, "wb").write(b"X")).start()

        events = poller.poll(200)  # reduce timeout to 200ms
        assert events, "poll timed out"
        assert notified.wait(0.2), "SIGIO not delivered"
    finally:
        os.close(fd_r)

def test_buffer_expansion_via_write(fd):
    # Write beyond INITIAL_CLIPBOARD_CAPACITY (1024) to force expansion
    data = b"A" * 2048
    written = os.write(fd, data)
    assert written == len(data)
    # Buffer should contain our data
    os.lseek(fd, 0, os.SEEK_SET)
    assert os.read(fd, 10) == b"A" * 10


def test_buffer_max_capacity_error(fd):
    # Read the parameter to confirm MAX_CAP
    maxcap = int(open("/sys/module/clipboard/parameters/max_clipboard_capacity").read())
    # Attempt to write more than max capacity
    big = b"B" * (maxcap + 1)
    with pytest.raises(OSError) as exc:
        os.write(fd, big)
    assert exc.value.errno == errno.ENOMEM
