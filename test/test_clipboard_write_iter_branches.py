# tests/test_clipboard_write_iter_branches.py
import os
import errno
import pytest

DEV = "/dev/clipboard"

@pytest.fixture
def fd_rw():
    # Open read/write and truncate to reset state
    fd = os.open(DEV, os.O_RDWR | os.O_TRUNC)
    os.close(fd)
    fd = os.open(DEV, os.O_RDWR)
    try:
        yield fd
    finally:
        os.close(fd)

# 1) Small writev within initial capacity
def test_writev_small(fd_rw):
    iov = [b"foo", b"bar"]
    n = os.writev(fd_rw, iov)
    assert n == 6
    os.lseek(fd_rw, 0, os.SEEK_SET)
    assert os.read(fd_rw, 6) == b"foobar"

# 2) writev with O_APPEND
def test_writev_append(fd_rw):
    # initial content
    os.write(fd_rw, b"start")
    fd2 = os.open(DEV, os.O_WRONLY | os.O_APPEND)
    try:
        n = os.writev(fd2, [b"X", b"Y"]);
        assert n == 2
    finally:
        os.close(fd2)

    os.lseek(fd_rw, 0, os.SEEK_SET)
    assert os.read(fd_rw, 7) == b"startXY"

# 3) writev forcing expansion
def test_writev_expand(fd_rw):
    # initial capacity = 1024
    data = b'A' * 2000
    n = os.writev(fd_rw, [data])
    assert n == 2000
    os.lseek(fd_rw, 0, os.SEEK_SET)
    buf = os.read(fd_rw, 2000)
    assert buf[0] == ord('A') and buf[-1] == ord('A')

# 4) expand beyond max causes ENOMEM
def test_writev_expand_fail(fd_rw):
    maxcap = int(open(
        "/sys/module/clipboard/parameters/max_clipboard_capacity").read())
    big = b'B' * (maxcap + 1)
    with pytest.raises(OSError) as exc:
        os.writev(fd_rw, [big])
    assert exc.value.errno == errno.ENOMEM

# 5) no space left after buffer full -> ENOMEM due to expansion failure
def test_writev_enospc(fd_rw):
     maxcap = int(open(
         "/sys/module/clipboard/parameters/max_clipboard_capacity").read())
     # fill to capacity
     os.writev(fd_rw, [b'C' * maxcap])
     # next write should ENOMEM as expand fails
     with pytest.raises(OSError) as exc:
         os.writev(fd_rw, [b'Z'])
     assert exc.value.errno == errno.ENOMEM

