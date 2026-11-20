# SPDX-License-Identifier: GPL-3.0-only
"""Windows desktop process and workspace ownership."""
from contextlib import contextmanager
import ctypes
from ctypes import wintypes as w
import os
import subprocess


@contextmanager
def workspace_lock(root):
    import msvcrt
    root.mkdir(parents=True, exist_ok=True)
    with (root/'.instance.lock').open('a+b') as lock:
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as exc:
            if exc.errno == 13:
                raise RuntimeError('프로그램이 이미 실행 중입니다.') from exc
            raise
        yield


@contextmanager
def launch_engine(args, log, flags=0):
    if os.name != 'nt':
        with subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT) as process:
            yield process
        return

    import _winapi
    import msvcrt
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    pointer = ctypes.c_void_p
    size = ctypes.c_size_t

    def api(name, result, *parameters):
        function = getattr(k, name)
        function.restype = result
        function.argtypes = parameters
        return function

    def checked(value):
        if not value:
            raise ctypes.WinError(ctypes.get_last_error())
        return value

    class Startup(ctypes.Structure):
        _fields_ = [('cb', w.DWORD), ('reserved', w.LPWSTR), ('desktop', w.LPWSTR),
                    ('title', w.LPWSTR)] + [(key, w.DWORD) for key in
                    ('x', 'y', 'width', 'height', 'columns', 'rows', 'fill', 'flags')] + [
                    ('show', w.WORD), ('reservedSize', w.WORD), ('reservedBytes', pointer),
                    ('stdin', w.HANDLE), ('stdout', w.HANDLE), ('stderr', w.HANDLE)]

    class ExtendedStartup(ctypes.Structure):
        _fields_ = [('startup', Startup), ('attributes', pointer)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [('process', w.HANDLE), ('thread', w.HANDLE),
                    ('pid', w.DWORD), ('tid', w.DWORD)]

    class JobLimits(ctypes.Structure):
        _fields_ = [('processTime', ctypes.c_int64), ('jobTime', ctypes.c_int64),
                    ('flags', w.DWORD), ('workingMin', size), ('workingMax', size),
                    ('active', w.DWORD), ('affinity', size), ('priority', w.DWORD),
                    ('scheduling', w.DWORD)]

    class ExtendedJobLimits(ctypes.Structure):
        _fields_ = [('basic', JobLimits), ('io', ctypes.c_uint64 * 6),
                    ('processMemory', size), ('jobMemory', size),
                    ('peakProcessMemory', size), ('peakJobMemory', size)]

    class Process:
        returncode = None

        def __init__(self, info):
            self._handle = info.process
            self.pid = info.pid

        def poll(self):
            if self.returncode is None and _winapi.WaitForSingleObject(self._handle, 0) == 0:
                self.returncode = _winapi.GetExitCodeProcess(self._handle)
            return self.returncode

        def wait(self, timeout=None):
            wait = _winapi.INFINITE if timeout is None else int(timeout * 1000)
            if _winapi.WaitForSingleObject(self._handle, wait) == _winapi.WAIT_TIMEOUT:
                raise subprocess.TimeoutExpired(args, timeout)
            return self.poll()

        def terminate(self):
            checked(terminate_job(job, 1))

        kill = terminate

    create_job = api('CreateJobObjectW', w.HANDLE, pointer, w.LPCWSTR)
    set_job = api('SetInformationJobObject', w.BOOL, w.HANDLE, ctypes.c_int, pointer, w.DWORD)
    terminate_job = api('TerminateJobObject', w.BOOL, w.HANDLE, w.UINT)
    initialize = api('InitializeProcThreadAttributeList', w.BOOL, pointer, w.DWORD, w.DWORD, ctypes.POINTER(size))
    update = api('UpdateProcThreadAttribute', w.BOOL, pointer, w.DWORD, size, pointer, size, pointer, pointer)
    delete = api('DeleteProcThreadAttributeList', None, pointer)
    create = api('CreateProcessW', w.BOOL, w.LPCWSTR, w.LPWSTR, pointer, pointer,
                 w.BOOL, w.DWORD, pointer, w.LPCWSTR, pointer, ctypes.POINTER(ProcessInfo))

    from contextlib import ExitStack
    with ExitStack() as cleanup:
        job = checked(create_job(None, None))
        # The engine must never inherit the lifetime handle.
        cleanup.callback(_winapi.CloseHandle, job)
        limits = ExtendedJobLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        checked(set_job(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)))

        stdin = cleanup.enter_context(open(os.devnull, 'rb'))
        current = _winapi.GetCurrentProcess()
        handles = []
        for stream in (stdin, log):
            handle = _winapi.DuplicateHandle(current, msvcrt.get_osfhandle(stream.fileno()),
                                             current, 0, True, _winapi.DUPLICATE_SAME_ACCESS)
            cleanup.callback(_winapi.CloseHandle, handle)
            handles.append(handle)
        inherited = (w.HANDLE * 2)(*handles)
        jobs = (w.HANDLE * 1)(job)
        length = size()
        initialize(None, 2, 0, ctypes.byref(length))
        attributes = ctypes.create_string_buffer(length.value)
        checked(initialize(attributes, 2, 0, ctypes.byref(length)))
        cleanup.callback(delete, attributes)
        checked(update(attributes, 0, 0x20002, inherited, ctypes.sizeof(inherited), None, None))
        checked(update(attributes, 0, 0x2000D, jobs, ctypes.sizeof(jobs), None, None))
        startup = ExtendedStartup()
        startup.startup.cb = ctypes.sizeof(startup)
        startup.startup.flags = subprocess.STARTF_USESTDHANDLES
        startup.startup.stdin, startup.startup.stdout = handles
        startup.startup.stderr = handles[1]
        startup.attributes = ctypes.addressof(attributes)
        info = ProcessInfo()
        command = ctypes.create_unicode_buffer(subprocess.list2cmdline(args))
        checked(create(args[0], command, None, None, True, flags | 0x80000, None, None,
                       ctypes.byref(startup), ctypes.byref(info)))
        cleanup.callback(_winapi.CloseHandle, info.process)
        _winapi.CloseHandle(info.thread)
        yield Process(info)
