import os
import select
import time

DEV_PATH = "/dev/clipboard"

#
# test that POLLIN wakes only after new data arrive
#
def test_poll_read_ready(clipboard_module):
    #
    # open a non‑blocking read FD and arm a poll() watcher
    #
    rd_fd = os.open(DEV_PATH, os.O_RDONLY | os.O_NONBLOCK)
    poller = select.poll()
    poller.register(rd_fd, select.POLLIN)

    # No data yet → should time‑out
    assert poller.poll(100) == []

    #
    # writer: same UID, same process – write a short blob
    #
    wr_fd = os.open(DEV_PATH, os.O_WRONLY)
    os.write(wr_fd, b"hello-world")    # plain ASCII hyphen here
    os.close(wr_fd)

    # The wait‑queue in the driver should wake us up
    events = poller.poll(1000)          # 1 s max
    assert events and events[0][1] & select.POLLIN

    os.close(rd_fd)

#
# test that the device is always POLLOUT‑ready (never “full”)
#
def test_poll_write_always_ready(clipboard_module):
    fd = os.open(DEV_PATH, os.O_WRONLY | os.O_NONBLOCK)
    poller = select.poll()
    poller.register(fd, select.POLLOUT)

    events = poller.poll(100)           # should fire immediately
    assert events and events[0][1] & select.POLLOUT

    os.close(fd)

