"""Tail serial logs from the device and optionally send commands."""
import argparse
import sys
import threading
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/cu.usbmodem2101', help='Serial port')
    parser.add_argument('--baud', type=int, default=115200, help='Baud rate')
    args = parser.parse_args()

    print(f"Connecting to {args.port} at {args.baud} baud...")
    print("Commands: 'c' (click button), 'vol 90' (set volume %), '+' (max vol), '-' (mid vol), 'diag' (wifi diagnostics).\n" + "-" * 70)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
        ser.dtr = True
        ser.rts = True
    except serial.SerialException as e:
        sys.exit(f"Could not open port {args.port}: {e}")

    def reader():
        while ser.is_open:
            try:
                line = ser.readline().decode('utf-8', errors='replace')
                if line:
                    sys.stdout.write(line)
                    sys.stdout.flush()
            except Exception:
                break

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    try:
        while True:
            cmd = sys.stdin.readline()
            if not cmd:
                break
            cmd = cmd.strip()
            if cmd in ('c', 'click'):
                ser.write(b'{"command":"click"}\n')
                print("[Sent click command]")
            elif cmd in ('diag', 'd'):
                ser.write(b'{"command":"wifi_diagnostics"}\n')
                print("[Sent diagnostics request]")
            elif cmd.startswith('vol ') or cmd.startswith('v '):
                parts = cmd.split()
                if len(parts) >= 2 and parts[1].isdigit():
                    v = int(parts[1])
                    ser.write(f'{{"command":"volume","value":{v}}}\n'.encode('utf-8'))
                    print(f"[Sent volume {v}%]")
            elif cmd == '+':
                ser.write(b'{"command":"volume","value":100}\n')
                print("[Sent volume 100%]")
            elif cmd == '-':
                ser.write(b'{"command":"volume","value":50}\n')
                print("[Sent volume 50%]")
            elif cmd:
                ser.write((cmd + '\n').encode('utf-8'))
    except KeyboardInterrupt:
        print("\nExiting.")
    finally:
        ser.close()


if __name__ == '__main__':
    main()
