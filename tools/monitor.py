#!/usr/bin/env python3
"""串口监视器。macOS 上 >230400 的波特率必须用 IOSSIOSPEED，
Python 的 termios 没有对应的 Bxxx 常量。

用法: monitor.py [秒数] [--reset]
"""
import fcntl, os, select, struct, sys, termios, time

PORT = "/dev/cu.usbmodem5B910325971"
BAUD = 115200
IOSSIOSPEED = 0x80085402  # _IOW('T', 2, speed_t)，Darwin 上 speed_t 是 8 字节
TIOCMSET = 0x8004746D
RTS = 0x0004


def open_port(baud=BAUD, configure=True):
    """configure=False 时只读、不碰 termios。

    守护进程持有同一端口在写时，这里再调 tcsetattr 会打断它的写入，
    表现为板子侧丢帧——那是测量干扰，不是真实故障。
    只观察时一律用 configure=False。
    """
    flags = os.O_RDONLY if not configure else os.O_RDWR
    fd = os.open(PORT, flags | os.O_NOCTTY | os.O_NONBLOCK)
    if not configure:
        return fd
    a = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = a
    cflag |= termios.CLOCAL | termios.CREAD
    cflag &= ~termios.CSIZE
    cflag |= termios.CS8
    cflag &= ~(termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    iflag &= ~(termios.IXON | termios.IXOFF | termios.IXANY
               | termios.INLCR | termios.ICRNL)
    oflag &= ~termios.OPOST
    lflag &= ~(termios.ICANON | termios.ECHO | termios.ECHOE | termios.ISIG)
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
    # 115200 是标准速率，直接用 termios 常量；
    # 更高速率在 macOS 上要走 IOSSIOSPEED，实测不可靠，故不使用。
    speed = getattr(termios, "B%d" % baud)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = speed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def reset(fd):
    def lines(bits):
        fcntl.ioctl(fd, TIOCMSET, struct.pack("I", bits))
    lines(0); time.sleep(0.05)
    lines(RTS); time.sleep(0.15)
    lines(0)


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    # 默认只读观察；只有要复位时才接管端口设置
    fd = open_port(configure="--reset" in sys.argv)
    if "--reset" in sys.argv:
        reset(fd)
    buf = b""
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                buf += os.read(fd, 8192)
            except OSError:
                pass
    os.close(fd)
    sys.stdout.write(buf.decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
