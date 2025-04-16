# tests/test_clipboard_write_branches.py
import os
import errno
import pytest
import fcntl

DEV = "/dev/clipboard"

@pytest.fixture
def fd_write():
    # Open for write only (truncate to reset state)
    fd = os.open(DEV, os.O_RDWR | os.O_TRUNC)
    try:
        yield fd
    finally:
        os.close(fd)

@pytest.fixture
def fd_readonly():
    # Read-only open
    fd = os.open(DEV, os.O_RDONLY)
    try:
        yield fd
    finally:
        os.close(fd)

# 1) count == 0 branch

def test_zero_length_write(fd_write):
    # Writing zero bytes should return 0 and leave buffer empty
    assert os.write(fd_write, b"") == 0
    os.lseek(fd_write, 0, os.SEEK_SET)
    assert os.read(fd_write, 1) == b""

# 2) write to read-only should EBADF

def test_write_readonly_ebadf(fd_readonly):
    with pytest.raises(OSError) as exc:
        os.write(fd_readonly, b"X")
    assert exc.value.errno == errno.EBADF

# 3) normal small write (within initial capacity)

def test_small_write(fd_write):
    data = b"hello"
    n = os.write(fd_write, data)
    assert n == len(data)
    # read back
    os.lseek(fd_write, 0, os.SEEK_SET)
    assert os.read(fd_write, len(data)) == data

# 4) O_APPEND branch

def test_append_flag(fd_write):
    # initial write
    os.write(fd_write, b"foo")
    # open append handle
    fd_app = os.open(DEV, os.O_WRONLY | os.O_APPEND)
    try:
        assert os.write(fd_app, b"bar") == 3
    finally:
        os.close(fd_app)
    os.lseek(fd_write, 0, os.SEEK_SET)
    assert os.read(fd_write, 6) == b"foobar"

# 5) buffer expansion success

def test_expand_success(fd_write):
    # write past initial 1024 bytes to force expansion
    buf = b"A" * 1500
    n = os.write(fd_write, buf)
    assert n == 1500
    # verify first and last bytes
    os.lseek(fd_write, 0, os.SEEK_SET)
    data = os.read(fd_write, 1500)
    assert data[0] == ord('A') and data[-1] == ord('A')

# 6) buffer expansion failure (beyond max)

def test_expand_failure(fd_write):
    # Read max from sysfs
    maxcap = int(open("/sys/module/clipboard/parameters/max_clipboard_capacity").read())
    # attempt write beyond max capacity
    bigbuf = b"B" * (maxcap + 1)
    with pytest.raises(OSError) as exc:
        os.write(fd_write, bigbuf)
    assert exc.value.errno == errno.ENOMEM

