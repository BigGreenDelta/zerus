import argparse
import logging
import os
import signal
from pathlib import Path
import threading

from .config import default_config, load
from .service import Server


def main():
    parser = argparse.ArgumentParser(description="Private Zerus speech host service")
    parser.add_argument("--config", type=Path, default=default_config())
    parser.add_argument("--test-mode", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    os.umask(0o077)
    logging.basicConfig(level=logging.INFO)
    with Server(load(args.config), test_mode=args.test_mode) as server:
        def stop(_number, _frame):
            threading.Thread(target=server.shutdown, daemon=True).start()
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        server.serve_forever(poll_interval=0.2)


if __name__ == "__main__":
    main()
