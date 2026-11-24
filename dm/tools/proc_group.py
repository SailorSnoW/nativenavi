"""A subprocess in a process group and session of its own. No interrupt reaches that group by itself (Spyder's goes to its kernel, a front end's
SIGTERM to the script, Ctrl-C to the terminal's foreground group), so it is stopped here; members left after drain are stragglers, a failure
of the caller's stage. A process that leaves the group itself (setsid, setpgid) is out of reach."""
import codecs, ctypes, io, locale, os, selectors, signal, subprocess, sys, time

GRACE = 5.0   # seconds after the stopping signal, before SIGKILL
DRAIN = 5.0   # seconds members may outlive the direct child's normal exit
REAP = 2.0    # seconds; launchd reaps the orphans
TICK = 0.05

_libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True) if sys.platform == 'darwin' else None
if _libproc is not None:
    _libproc.proc_listpids.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_int]
    _libproc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]


def members(pgid):
    """Zombies are not members: they write nothing, and the caller or launchd reaps them"""
    if _libproc is None:
        try:
            os.killpg(pgid, 0)
            return [(pgid, '?')]
        except ProcessLookupError: return []
        except PermissionError: return [(pgid, '?')]
    buf = (ctypes.c_int * 1024)()
    n = _libproc.proc_listpids(2, pgid, buf, ctypes.sizeof(buf)) // ctypes.sizeof(ctypes.c_int)   # PROC_PGRP_ONLY
    out, info = [], ctypes.create_string_buffer(136)   # struct proc_bsdinfo
    for pid in buf[:max(n, 0)]:
        if pid <= 0: continue
        if _libproc.proc_pidinfo(pid, 3, 0, info, 136) != 136: continue   # PROC_PIDTBSDINFO; fails for a zombie or a process gone meanwhile
        status, pg = int.from_bytes(info.raw[4:8], 'little'), int.from_bytes(info.raw[100:104], 'little')
        if status == 5 or pg != pgid: continue   # SZOMB; a pid reused outside the group
        out.append((pid, info.raw[64:96].split(b'\0')[0].decode(errors='replace') or info.raw[48:64].split(b'\0')[0].decode(errors='replace')))
    return out


class Group:
    """`with Group(...) as g:` stops the group when the block raises, else finishes it. stragglers: [(pid, name)], pid None for a holder of the output outside the group"""

    def __init__(self, cmd, grace=None, drain=None, **popen):
        self.grace, self.drain = GRACE if grace is None else grace, DRAIN if drain is None else drain
        self.paused, self.stragglers, self.done = False, [], False
        self.p = subprocess.Popen([str(c) for c in cmd], start_new_session=True, **popen)
        self.pgid = self.p.pid

    def __enter__(self): return self

    def __exit__(self, kind, exc, tb):
        try:
            if kind is not None: self.stop(signal.SIGINT if issubclass(kind, KeyboardInterrupt) else signal.SIGTERM)
            elif not self.done: self.finish()
        finally:
            if self.p.stdout: self.p.stdout.close()   # after the stop: a member is not cut off by SIGPIPE before its signal
        return False

    def _signal(self, sig):
        if self.p.returncode is not None and not members(self.pgid): return   # the number may be another group's now
        try: os.killpg(self.pgid, sig)
        except (ProcessLookupError, PermissionError): pass   # macOS answers EPERM for zombies

    def pause(self):
        self._signal(signal.SIGSTOP)
        self.paused = True

    def resume(self):
        self._signal(signal.SIGCONT)
        self.paused = False

    def _gone(self, secs):
        end = time.monotonic() + secs
        while True:
            self.p.poll()
            if not members(self.pgid): return True
            if time.monotonic() >= end: return False
            time.sleep(TICK)

    def stop(self, sig=signal.SIGTERM):
        """Resumed first: a stopped process acts on no signal but SIGKILL. Returns once the child is reaped and no member is left, or REAP s after the SIGKILL (a member stuck
        in the kernel). Re-raises the last interrupt that came while it waited"""
        late, self.done = None, True
        try:
            self.resume()
            self._signal(sig)
            if self._gone(self.grace):
                self.p.wait()
                return
        except BaseException as e: late = e   # a second interrupt or SIGTERM: no more grace
        while True:   # an interrupt here only repeats a step
            try:
                self._signal(signal.SIGKILL)
                self.p.wait()
                self._gone(REAP)
                break
            except BaseException as e: late = e
        if late is not None: raise late

    def finish(self):
        self.p.wait()
        self.done = True
        if not self._gone(self.drain):
            self.stragglers += members(self.pgid)
            self.stop()
        return self.p.returncode

    def lines(self):
        """Lines as text=True would read them; ends at the output's end, or drain seconds after the child's exit while a member still holds the pipe (stopped then)"""
        out = self.p.stdout
        assert out is not None, 'lines() reads a group started with stdout=PIPE'
        fd = out.fileno()
        dec = io.IncrementalNewlineDecoder(codecs.getincrementaldecoder(locale.getpreferredencoding(False))(errors='replace'), translate=True)
        sel, buf, end, stopped = selectors.DefaultSelector(), '', None, False
        sel.register(fd, selectors.EVENT_READ)
        try:
            while True:
                if sel.select(0.1):
                    data = os.read(fd, 65536)
                    if not data: break
                    buf += dec.decode(data)
                    *done, buf = buf.split('\n')
                    for line in done: yield line + '\n'
                if end is None and self.p.poll() is not None: end = time.monotonic() + self.drain
                if end is not None and time.monotonic() >= end:
                    self.stragglers += members(self.pgid) if not stopped else []
                    if stopped or not self.stragglers:   # a holder outside the group: stop reading, and fail the stage all the same
                        self.stragglers.append((None, 'a process outside the group holding its output'))
                        break
                    self.stop()
                    stopped, end = True, time.monotonic() + REAP
        finally:
            sel.close()
        buf += dec.decode(b'', final=True)
        if buf: yield buf


def describe(stragglers):
    return ', '.join(name if pid is None else f'{name} (pid {pid})' for pid, name in stragglers)
