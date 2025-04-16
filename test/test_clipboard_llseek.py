# tests/test_clipboard_llseek.py
import os
import errno
import pytest

DEV = "/dev/clipboard"

@pytest.fixture
def fd():
    # Open read/write to get private_data set
    fd = os.open(DEV, os.O_RDWR)
    try:
        yield fd
    finally:
        os.close(fd)

# 1: SEEK_SET with positive offset
def test_llseek_set_positive(fd):
    new = os.lseek(fd, 10, os.SEEK_SET)
    assert new == 10

# 2: SEEK_SET with zero offset
def test_llseek_set_zero(fd):
    new = os.lseek(fd, 0, os.SEEK_SET)
    assert new == 0

# 3: SEEK_SET with negative offset -> EINVAL
def test_llseek_set_negative(fd):
    with pytest.raises(OSError) as exc:
        os.lseek(fd, -1, os.SEEK_SET)
    assert exc.value.errno == errno.EINVAL

# 4: SEEK_CUR with positive offset
def test_llseek_cur_positive(fd):
    os.lseek(fd, 5, os.SEEK_SET)
    new = os.lseek(fd, 3, os.SEEK_CUR)
    assert new == 8

# 5: SEEK_CUR with negative offset but within bounds
def test_llseek_cur_negative_within(fd):
    os.lseek(fd, 8, os.SEEK_SET)
    new = os.lseek(fd, -5, os.SEEK_CUR)
    assert new == 3

# 6: SEEK_CUR with negative offset beyond start -> EINVAL
def test_llseek_cur_negative_beyond(fd):
    os.lseek(fd, 2, os.SEEK_SET)
    with pytest.raises(OSError) as exc:
        os.lseek(fd, -5, os.SEEK_CUR)
    assert exc.value.errno == errno.EINVAL

# 7: SEEK_END always returns EINVAL
def test_llseek_end(fd):
    with pytest.raises(OSError) as exc:
        os.lseek(fd, 0, os.SEEK_END)
    assert exc.value.errno == errno.EINVAL

# 8: Invalid whence -> EINVAL
def test_llseek_invalid_whence(fd):
    with pytest.raises(OSError) as exc:
        os.lseek(fd, 0, 999)
    assert exc.value.errno == errno.EINVAL

