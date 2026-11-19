"""Provision a TRMNL device key over USB. The key is never echoed."""

import argparse
import getpass
import re
import time

import serial


def provision(port, key):
    if not re.fullmatch(r"[A-Za-z0-9_.~+/=-]{8,160}", key):
        raise ValueError("Invalid TRMNL device key format")

    connection = serial.Serial(baudrate=115200, timeout=0.25, write_timeout=2)
    connection.dtr = connection.rts = False
    connection.port = port
    with connection:
        deadline = time.monotonic() + 20
        next_ping = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_ping:
                connection.write(b"\nTRMNL PING\n")
                next_ping = time.monotonic() + 1
            if connection.readline().strip() == b"TRMNL READY":
                break
        else:
            raise TimeoutError("TRMNL mirror firmware did not answer on USB")

        connection.write(b"TRMNL KEY " + key.encode("ascii") + b"\n")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            reply = connection.readline().strip()
            if reply == b"TRMNL KEY SAVED":
                return
            if reply == b"TRMNL KEY REJECTED":
                raise RuntimeError("The ESP32 could not save the key")
        raise TimeoutError("The ESP32 did not acknowledge the key")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Serial port such as COM6 or /dev/ttyUSB0")
    args = parser.parse_args()
    provision(args.port, getpass.getpass("TRMNL Device API Key: ").strip())
    print("Device key saved on the ESP32.")
