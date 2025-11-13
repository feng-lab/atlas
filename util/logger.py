# Copyright (c) Facebook, Inc. and its affiliates.
import functools
import io
import logging
import os
import sys
import traceback
from typing import Iterable, Literal

__all__ = ["setup_logger"]


class _ColorfulFormatter(logging.Formatter):
    def __init__(self, *args, **kwargs):
        super(_ColorfulFormatter, self).__init__(*args, **kwargs)

    def formatMessage(self, record):
        log = super(_ColorfulFormatter, self).formatMessage(record)
        if record.levelno == logging.WARNING:
            prefix = colored("WARNING", "red")
        elif record.levelno == logging.ERROR:
            prefix = colored("ERROR", "red", attrs=["underline"])
        elif record.levelno == logging.CRITICAL:
            prefix = colored("CRITICAL", "red", attrs=["underline"])
        else:
            return log
        return prefix + " " + log


class CriticalExitStreamHandler(logging.StreamHandler):
    def emit(self, record):
        # Process the log message normally first
        super().emit(record)

        # Check if the log level is CRITICAL
        if record.levelno >= logging.CRITICAL:
            # If there's exception info, print the exception traceback
            if record.exc_info:
                traceback.print_exception(*record.exc_info)
            else:
                # Otherwise, print the current stack trace
                print('Stack trace before exit:', file=sys.stderr)
                traceback.print_stack(file=sys.stderr)

            # Flush the stream to ensure all log messages are outputted
            self.flush()

            # Exit the program
            sys.exit('Exiting due to critical log: ' + record.getMessage())


@functools.lru_cache()  # so that calling setup_logger multiple times won't add many handlers
def setup_logger(
        output=None,
        distributed_rank=0,
        *,
        color=True,
        name="",
        enable_propagation: bool = False,
        configure_stdout: bool = True
):
    """
    Initialize the detectron2 logger and set its verbosity level to "DEBUG".

    Args:
        output (str): a file name or a directory to save log. If None, will not save log file.
            If ends with ".txt" or ".log", assumed to be a file name.
            Otherwise, logs will be saved to `output/log.txt`.
        name (str): the root module name of this logger
        enable_propagation (bool): whether to propagate logs to the parent logger.
        configure_stdout (bool): whether to configure logging to stdout.


    Returns:
        logging.Logger: a logger
    """
    logger = logging.getLogger(name)
    logger.setLevel(logging.DEBUG)
    logger.propagate = enable_propagation

    plain_formatter = logging.Formatter(
        # "[%(asctime)s] %(name)s %(levelname)s: %(message)s", datefmt="%m/%d %H:%M:%S"
        "[%(levelname)s %(asctime)s %(process)d %(name)s:%(lineno)d]: %(message)s",
    )
    # stdout logging: master only
    if configure_stdout and distributed_rank == 0:
        ch = CriticalExitStreamHandler(stream=sys.stdout)
        ch.setLevel(logging.DEBUG)
        if color and _can_do_colour():
            formatter = _ColorfulFormatter(
                # colored("[%(asctime)s %(name)s]: ", "green") + "%(message)s",
                # datefmt="%m/%d %H:%M:%S",
                colored("[%(asctime)s %(process)d %(name)s:%(lineno)d]:", "green") + " %(message)s",
            )
        else:
            formatter = plain_formatter
        ch.setFormatter(formatter)
        logger.addHandler(ch)

    # file logging: all workers
    if output is not None:
        if output.endswith(".txt") or output.endswith(".log"):
            filename = output
        else:
            filename = os.path.join(output, "log.txt")
        if distributed_rank > 0:
            filename = filename + ".rank{}".format(distributed_rank)

        os.makedirs(os.path.dirname(filename), exist_ok=True)

        fh = logging.FileHandler(filename)
        fh.setLevel(logging.DEBUG)
        fh.setFormatter(plain_formatter)
        logger.addHandler(fh)

    return logger


"""
Below are some other convenient logging methods.
They are mainly adopted from
https://github.com/termcolor/termcolor/blob/main/src/termcolor/termcolor.py
"""

Attribute = Literal[
    "bold",
    "dark",
    "underline",
    "blink",
    "reverse",
    "concealed",
    "strike",
]

Highlight = Literal[
    "on_black",
    "on_grey",
    "on_red",
    "on_green",
    "on_yellow",
    "on_blue",
    "on_magenta",
    "on_cyan",
    "on_light_grey",
    "on_dark_grey",
    "on_light_red",
    "on_light_green",
    "on_light_yellow",
    "on_light_blue",
    "on_light_magenta",
    "on_light_cyan",
    "on_white",
]

Color = Literal[
    "black",
    "grey",
    "red",
    "green",
    "yellow",
    "blue",
    "magenta",
    "cyan",
    "light_grey",
    "dark_grey",
    "light_red",
    "light_green",
    "light_yellow",
    "light_blue",
    "light_magenta",
    "light_cyan",
    "white",
]

ATTRIBUTES: dict[Attribute, int] = {
    "bold": 1,
    "dark": 2,
    "underline": 4,
    "blink": 5,
    "reverse": 7,
    "concealed": 8,
    "strike": 9,
}

HIGHLIGHTS: dict[Highlight, int] = {
    "on_black": 40,
    "on_grey": 40,  # Actually black but kept for backwards compatibility
    "on_red": 41,
    "on_green": 42,
    "on_yellow": 43,
    "on_blue": 44,
    "on_magenta": 45,
    "on_cyan": 46,
    "on_light_grey": 47,
    "on_dark_grey": 100,
    "on_light_red": 101,
    "on_light_green": 102,
    "on_light_yellow": 103,
    "on_light_blue": 104,
    "on_light_magenta": 105,
    "on_light_cyan": 106,
    "on_white": 107,
}

COLORS: dict[Color, int] = {
    "black": 30,
    "grey": 30,  # Actually black but kept for backwards compatibility
    "red": 31,
    "green": 32,
    "yellow": 33,
    "blue": 34,
    "magenta": 35,
    "cyan": 36,
    "light_grey": 37,
    "dark_grey": 90,
    "light_red": 91,
    "light_green": 92,
    "light_yellow": 93,
    "light_blue": 94,
    "light_magenta": 95,
    "light_cyan": 96,
    "white": 97,
}

RESET = "\033[0m"


def _can_do_colour(
        *, no_color: bool | None = None, force_color: bool | None = None
) -> bool:
    """Check env vars and for tty/dumb terminal"""
    # First check overrides:
    # "User-level configuration files and per-instance command-line arguments should
    # override $NO_COLOR. A user should be able to export $NO_COLOR in their shell
    # configuration file as a default, but configure a specific program in its
    # configuration file to specifically enable color."
    # https://no-color.org
    if no_color is not None and no_color:
        return False
    if force_color is not None and force_color:
        return True

    # Then check env vars:
    if "ANSI_COLORS_DISABLED" in os.environ:
        return False
    if "NO_COLOR" in os.environ:
        return False
    if "FORCE_COLOR" in os.environ:
        return True

    # Then check system:
    if os.environ.get("TERM") == "dumb":
        return False
    if not hasattr(sys.stdout, "fileno"):
        return False

    try:
        return os.isatty(sys.stdout.fileno())
    except io.UnsupportedOperation:
        return sys.stdout.isatty()


def colored(
        text: object,
        color: Color | None = None,
        on_color: Highlight | None = None,
        attrs: Iterable[Attribute] | None = None,
        *,
        no_color: bool | None = None,
        force_color: bool | None = None,
) -> str:
    """Colorize text.

    Available text colors:
        black, red, green, yellow, blue, magenta, cyan, white,
        light_grey, dark_grey, light_red, light_green, light_yellow, light_blue,
        light_magenta, light_cyan.

    Available text highlights:
        on_black, on_red, on_green, on_yellow, on_blue, on_magenta, on_cyan, on_white,
        on_light_grey, on_dark_grey, on_light_red, on_light_green, on_light_yellow,
        on_light_blue, on_light_magenta, on_light_cyan.

    Available attributes:
        bold, dark, underline, blink, reverse, concealed.

    Example:
        colored('Hello, World!', 'red', 'on_black', ['bold', 'blink'])
        colored('Hello, World!', 'green')
    """
    result = str(text)
    if not _can_do_colour(no_color=no_color, force_color=force_color):
        return result

    fmt_str = "\033[%dm%s"
    if color is not None:
        result = fmt_str % (COLORS[color], result)

    if on_color is not None:
        result = fmt_str % (HIGHLIGHTS[on_color], result)

    if attrs is not None:
        for attr in attrs:
            result = fmt_str % (ATTRIBUTES[attr], result)

    result += RESET

    return result
