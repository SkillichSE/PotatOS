import sys
import os

def add_nvidia_dlls_to_path():
    if sys.platform != 'win32':
        return
    try:
        import nvidia.cublas
        import nvidia.cudnn
        import nvidia.cuda_nvrtc
        for pkg in (nvidia.cublas, nvidia.cudnn, nvidia.cuda_nvrtc):
            bin_dir = os.path.join(list(pkg.__path__)[0], "bin")
            if os.path.isdir(bin_dir):
                os.add_dll_directory(bin_dir)
                os.environ["PATH"] = bin_dir + os.pathsep + os.environ["PATH"]
    except ImportError:
        pass

def fix_windows_console_encoding():
    if sys.platform != 'win32':
        return

    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', line_buffering=True, write_through=True)

    import locale
    locale.getpreferredencoding = lambda do_setlocale=True: 'utf-8'

    import builtins
    _orig_open = builtins.open

    def _utf8_open(file, mode="r", *args, **kwargs):
        if "b" not in mode and "encoding" not in kwargs:
            kwargs["encoding"] = "utf-8"
        return _orig_open(file, mode, *args, **kwargs)

    builtins.open = _utf8_open
