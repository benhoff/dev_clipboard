# tests/test_clipboard_read_iter_branches.py
import os
import errno
import pytest

DEV = "/dev/clipboard"

@pytest.fixture
def fd():
    """Open read/write descriptor to /dev/clipboard and truncate it"""
    # Truncate the clipboard by opening with O_TRUNC
    fd = os.open(DEV, os.O_RDWR | os.O_TRUNC)
    os.close(fd)
    # Re-open for read/write
    fd = os.open(DEV, os.O_RDWR)
    try:
        yield fd
    finally:
        os.close(fd)

# 1) No clipboard yet -> readv returns 0 bytes

def test_read_iter_no_data(fd):
    buf = bytearray(10)
    n = os.readv(fd, [buf])
    assert n == 0
    assert buf == bytearray(10)

# 2) Write data and seek to start, then readv into two buffers

def test_read_iter_partial(fd):
    data = b"abcdef"
    assert os.write(fd, data) == len(data)
    # Reset file position to beginning for read_iter
    os.lseek(fd, 0, os.SEEK_SET)

    buf1 = bytearray(3)
    buf2 = bytearray(3)
    n = os.readv(fd, [buf1, buf2])
    assert n == 6
    assert bytes(buf1) + bytes(buf2) == data

    # Subsequent readv at end should return 0
    buf3 = bytearray(4)
    m = os.readv(fd, [buf3])
    assert m == 0
    assert buf3 == bytearray(4)

# 3) Read with zero-length iov -> returns 0

def test_read_iter_zero_iov(fd):
    os.write(fd, b"xyz")
    os.lseek(fd, 0, os.SEEK_SET)
    n = os.readv(fd, [])
    assert n == 0

# 4) Read with buffers but no available (ppos >= size)

def test_read_iter_ppos_equal_size(fd):
    os.write(fd, b"1234")
    os.lseek(fd, 0, os.SEEK_SET)
    _ = os.readv(fd, [bytearray(4)])
    # Now file position equals size
    buf = bytearray(2)
    n = os.readv(fd, [buf])
    assert n == 0
    assert buf == bytearray(2)

# 5) Invalid descriptor should raise EBADF on readv

def test_read_iter_bad_fd():
    bad_fd = 9999
    with pytest.raises(OSError) as exc:
        os.readv(bad_fd, [bytearray(1)])
    assert exc.value.errno == errno.EBADF

