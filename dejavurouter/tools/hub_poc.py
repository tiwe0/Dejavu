#!/usr/bin/env python3
"""TCP hub PoC for dejavurouter.

The caller talks to a normal Python object, never to a socket:

    hub = RouterHub(port=39010)
    hub.start()
    hub.wait_connected(10)
    result = hub.call("1875541871079874560", "token")

The hub itself is the server side of the dejavurouter TCP protocol. It accepts
one router connection, reads the router hello, sends JSON request frames, and
correlates response frames back to blocking or async callers.

Protocol (newline-delimited JSON, both directions):

    router -> hub: {"type":"hello","device_id":"...","version":1}
    hub -> router: {"type":"request","id":"...","route":"...","method":"...",
                    "params":{...},"deadline_ms":30000}
    router -> hub: {"type":"response","id":"...","route":"...","ok":true,
                    "result":{...}}
                   {"type":"response","id":"...","route":"...","ok":false,
                    "error":{"code":"...","message":"...","retryable":true}}

This is deliberately small: one TCP listener, one reader thread, no per-call
threads. It is a test/PoC server, not the production hub.
"""
from __future__ import annotations

import argparse
import itertools
import json
import socket
import threading
import time
from dataclasses import dataclass
from typing import Any, Optional

__all__ = [
    "RouterHub",
    "PendingCall",
    "HubError",
    "NotConnectedError",
    "CallTimeoutError",
    "CallError",
    "AccountUnknownError",
    "DeviceOfflineError",
    "RouteOfflineError",
    "AccountBinding",
]


class HubError(RuntimeError):
    """Base class for hub-side errors."""


class NotConnectedError(HubError):
    """No router is currently connected."""


class CallTimeoutError(HubError):
    """The router/plugin did not answer before the deadline."""


class AccountUnknownError(HubError):
    """The account has no persistent device binding in the server table."""


class DeviceOfflineError(HubError):
    """The device that owns the account is not connected."""


class RouteOfflineError(HubError):
    """The account route is not currently announced by the device."""


class CallError(HubError):
    """The router answered with ok=false."""

    def __init__(self, response: dict[str, Any]):
        self.response = response
        error = response.get("error") if isinstance(response.get("error"), dict) else {}
        self.code = str(error.get("code", "call_error"))
        self.message = str(error.get("message", "router returned an error"))
        self.retryable = bool(error.get("retryable", False))
        super().__init__(f"{self.code}: {self.message}")

    def to_dict(self) -> dict[str, Any]:
        return {
            "code": self.code,
            "message": self.message,
            "retryable": self.retryable,
            "response": self.response,
        }


@dataclass(frozen=True)
class AccountBinding:
    """Persistent server-side binding: account -> device -> route."""

    account_id: str
    device_id: str
    route: str


@dataclass
class _Response:
    response: dict[str, Any]


class PendingCall:
    """Handle returned by RouterHub.call_async()."""

    def __init__(self, hub: "RouterHub", request_id: str, route: str, method: str,
                 deadline: float):
        self._hub = hub
        self.request_id = request_id
        self.route = route
        self.method = method
        self._deadline = deadline
        self._event = threading.Event()
        self._response: Optional[dict[str, Any]] = None
        self._error: Optional[BaseException] = None

    @property
    def done(self) -> bool:
        return self._event.is_set()

    def result(self, timeout: Optional[float] = None) -> Any:
        """Wait for the raw response envelope or raise HubError."""
        if timeout is None:
            timeout = max(0.0, self._deadline - time.monotonic())
        if not self._event.wait(timeout):
            self._hub._abandon(self.request_id)
            raise CallTimeoutError(
                f"no response for {self.method} route={self.route} id={self.request_id}"
            )
        if self._error is not None:
            raise self._error
        return self._response

    def result_value(self, timeout: Optional[float] = None) -> Any:
        """Wait and return only the result payload, raising CallError on ok=false."""
        response = self.result(timeout)
        return _unwrap_response(response)


def _unwrap_response(response: dict[str, Any]) -> Any:
    if not response.get("ok"):
        raise CallError(response)
    return response.get("result", {})


class RouterHub:
    """In-process facade over a dejavurouter TCP connection.

    Public methods:
        start(), stop(), wait_connected(), call(), call_async(), close()

    No socket, file descriptor, or wire message is exposed to the caller.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 39010,
                 *, name: str = "hub-poc", accept_backlog: int = 4,
                 max_line: int = 4 * 1024 * 1024):
        self._host = host
        self._port = int(port)
        self._name = name
        self._accept_backlog = accept_backlog
        self._max_line = max_line

        self._listen: Optional[socket.socket] = None
        self._conn: Optional[socket.socket] = None
        self._reader: Optional[Any] = None
        self._accept_thread: Optional[threading.Thread] = None
        self._reader_thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._connected = threading.Event()
        self._write_lock = threading.Lock()
        self._state_lock = threading.Lock()
        self._ids = itertools.count(1)
        self._pending: dict[str, PendingCall] = {}
        self._hello: dict[str, Any] = {}
        self._routes: dict[str, dict[str, Any]] = {}
        self._accounts: dict[str, AccountBinding] = {}
        self._last_error: str = ""
        self._accepted_count = 0
        self._calls_total = 0
        self._calls_failed = 0

    # -- lifecycle ---------------------------------------------------------

    def start(self, timeout: float = 5.0) -> "RouterHub":
        if self._accept_thread is not None:
            return self
        self._listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen.settimeout(0.2)
        self._listen.bind((self._host, self._port))
        self._listen.listen(self._accept_backlog)
        self._accept_thread = threading.Thread(
            target=self._accept_loop, name=f"{self._name}-accept", daemon=True
        )
        self._accept_thread.start()
        return self

    def __enter__(self) -> "RouterHub":
        return self.start()

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()

    def stop(self) -> None:
        self._stop.set()
        self._connected.clear()
        with self._write_lock:
            self._close_conn_locked()
        if self._listen is not None:
            try:
                self._listen.close()
            except OSError:
                pass
            self._listen = None
        if self._accept_thread is not None:
            self._accept_thread.join(timeout=2.0)
            self._accept_thread = None
        if self._reader_thread is not None:
            self._reader_thread.join(timeout=2.0)
            self._reader_thread = None
        self._fail_all_pending(NotConnectedError("hub stopped"))

    def close(self) -> None:
        self.stop()

    # -- ordinary call interface -------------------------------------------

    @property
    def connected(self) -> bool:
        return self._connected.is_set()

    @property
    def device(self) -> dict[str, Any]:
        with self._state_lock:
            return dict(self._hello)

    @property
    def routes(self) -> dict[str, dict[str, Any]]:
        """Routes currently announced by the connected router."""
        with self._state_lock:
            return {key: dict(value) for key, value in self._routes.items()}

    @property
    def accounts(self) -> dict[str, AccountBinding]:
        """Persistent account -> device/route rows, owned by the server."""
        with self._state_lock:
            return dict(self._accounts)

    @property
    def device_id(self) -> Optional[str]:
        with self._state_lock:
            value = self._hello.get("device_id")
            return value if isinstance(value, str) and value else None

    @property
    def last_error(self) -> str:
        return self._last_error

    @property
    def stats(self) -> dict[str, Any]:
        with self._state_lock:
            return {
                "connected": self._connected.is_set(),
                "accepted": self._accepted_count,
                "calls": self._calls_total,
                "failed": self._calls_failed,
                "pending": len(self._pending),
                "routes": len(self._routes),
                "device": dict(self._hello),
            }

    def wait_connected(self, timeout: float = 10.0) -> bool:
        return self._connected.wait(timeout)

    # -- persistent account table ------------------------------------------

    def bind_account(self, account_id: str, device_id: str,
                     route: Optional[str] = None) -> AccountBinding:
        """Create/update the persistent account -> device mapping.

        This is the server-side account table. It is not learned from a socket;
        route events only tell us whether the route is currently online.
        """
        if not account_id or not isinstance(account_id, str):
            raise ValueError("account_id must be a non-empty string")
        if not device_id or not isinstance(device_id, str):
            raise ValueError("device_id must be a non-empty string")
        binding = AccountBinding(account_id=account_id, device_id=device_id,
                                 route=route or account_id)
        with self._state_lock:
            self._accounts[account_id] = binding
        return binding

    def unbind_account(self, account_id: str) -> bool:
        with self._state_lock:
            return self._accounts.pop(account_id, None) is not None

    def account_for(self, account_id: str) -> AccountBinding:
        with self._state_lock:
            binding = self._accounts.get(account_id)
        if binding is None:
            raise AccountUnknownError(f"account {account_id!r} is not in the account table")
        return binding

    def call_account(self, account_id: str, method: str,
                     params: Optional[dict[str, Any]] = None, *,
                     timeout: float = 15.0,
                     deadline_ms: Optional[int] = None) -> Any:
        """Route one call using the persistent account table and live routes."""
        binding = self.account_for(account_id)
        if not self.connected:
            raise DeviceOfflineError(
                f"device {binding.device_id!r} for account {account_id!r} is offline"
            )
        with self._state_lock:
            route_info = self._routes.get(binding.route)
            current_device = self._hello.get("device_id")
        if route_info is None:
            raise RouteOfflineError(
                f"route {binding.route!r} for account {account_id!r} is not online"
            )
        if route_info.get("device_id") != binding.device_id:
            raise RouteOfflineError(
                f"route {binding.route!r} is announced by "
                f"{route_info.get('device_id')!r}, expected {binding.device_id!r}"
            )
        if current_device != binding.device_id:
            raise DeviceOfflineError(
                f"connected device is {current_device!r}, expected {binding.device_id!r}"
            )
        return self.call(binding.route, method, params, timeout=timeout,
                         deadline_ms=deadline_ms)

    def call_account_async(self, account_id: str, method: str,
                           params: Optional[dict[str, Any]] = None, *,
                           timeout: float = 15.0,
                           deadline_ms: Optional[int] = None) -> PendingCall:
        """Async variant of call_account()."""
        binding = self.account_for(account_id)
        if not self.connected:
            raise DeviceOfflineError(
                f"device {binding.device_id!r} for account {account_id!r} is offline"
            )
        with self._state_lock:
            route_info = self._routes.get(binding.route)
            current_device = self._hello.get("device_id")
        if route_info is None:
            raise RouteOfflineError(
                f"route {binding.route!r} for account {account_id!r} is not online"
            )
        if route_info.get("device_id") != binding.device_id:
            raise RouteOfflineError(
                f"route {binding.route!r} is announced by "
                f"{route_info.get('device_id')!r}, expected {binding.device_id!r}"
            )
        if current_device != binding.device_id:
            raise DeviceOfflineError(
                f"connected device is {current_device!r}, expected {binding.device_id!r}"
            )
        return self.call_async(binding.route, method, params, timeout=timeout,
                               deadline_ms=deadline_ms)

    def wake(self, route: str, *, timeout: float = 30.0,
             timeout_ms: Optional[int] = None) -> Any:
        """Ask the router to launch the app for a route and wait for registration."""
        if not route or not isinstance(route, str):
            raise ValueError("route must be a non-empty string")
        if not self.connected:
            raise NotConnectedError("no dejavurouter is connected")
        if timeout_ms is None:
            timeout_ms = max(1000, int(timeout * 1000))
        request_id = f"wake_{next(self._ids)}"
        deadline = time.monotonic() + max(1.0, min(timeout, timeout_ms / 1000.0)) + 2.0
        pending = PendingCall(self, request_id, route, "wake", deadline)
        with self._state_lock:
            self._pending[request_id] = pending
            self._calls_total += 1
        frame = {"type": "wake", "id": request_id, "route": route,
                 "timeout_ms": timeout_ms}
        try:
            self._send_frame(frame)
        except OSError as error:
            self._abandon(request_id, NotConnectedError(f"write failed: {error}"))
            raise NotConnectedError(f"write failed: {error}") from error
        return pending.result_value(timeout)

    def wake_account(self, account_id: str, *, timeout: float = 30.0,
                     timeout_ms: Optional[int] = None) -> Any:
        """Wake the device/app that owns the account, using the account table."""
        binding = self.account_for(account_id)
        if not self.connected:
            raise DeviceOfflineError(
                f"device {binding.device_id!r} for account {account_id!r} is offline"
            )
        current_device = self.device_id
        if current_device != binding.device_id:
            raise DeviceOfflineError(
                f"connected device is {current_device!r}, expected {binding.device_id!r}"
            )
        return self.wake(binding.route, timeout=timeout, timeout_ms=timeout_ms)

    def call(self, route: str, method: str, params: Optional[dict[str, Any]] = None,
             *, timeout: float = 15.0, deadline_ms: Optional[int] = None) -> Any:
        """Send one RPC and return its result payload.

        This is the normal upper-layer API: no socket, framing, id, or JSON.
        """
        pending = self.call_async(route, method, params, timeout=timeout,
                                  deadline_ms=deadline_ms)
        return pending.result_value(timeout)

    def call_raw(self, route: str, method: str, params: Optional[dict[str, Any]] = None,
                 *, timeout: float = 15.0, deadline_ms: Optional[int] = None) -> dict[str, Any]:
        """Like call(), but return the full response envelope."""
        return self.call_async(route, method, params, timeout=timeout,
                               deadline_ms=deadline_ms).result(timeout)

    def call_async(self, route: str, method: str,
                   params: Optional[dict[str, Any]] = None, *,
                   timeout: float = 15.0,
                   deadline_ms: Optional[int] = None) -> PendingCall:
        """Send one RPC and return a PendingCall immediately."""
        if not route or not isinstance(route, str):
            raise ValueError("route must be a non-empty string")
        if not method or not isinstance(method, str):
            raise ValueError("method must be a non-empty string")
        if not self.connected:
            raise NotConnectedError("no dejavurouter is connected")
        if deadline_ms is None:
            deadline_ms = max(1, int(timeout * 1000))
        request_id = f"poc_{next(self._ids)}"
        deadline = time.monotonic() + max(0.001, min(timeout, deadline_ms / 1000.0))
        pending = PendingCall(self, request_id, route, method, deadline)
        with self._state_lock:
            self._pending[request_id] = pending
            self._calls_total += 1
        frame = {
            "type": "request",
            "id": request_id,
            "route": route,
            "method": method,
            "params": params if params is not None else {},
            "deadline_ms": deadline_ms,
        }
        try:
            self._send_frame(frame)
        except OSError as error:
            self._abandon(request_id, NotConnectedError(f"write failed: {error}"))
            raise NotConnectedError(f"write failed: {error}") from error
        return pending

    # -- internals ---------------------------------------------------------

    def _send_frame(self, frame: dict[str, Any]) -> None:
        if not self.connected:
            raise NotConnectedError("no dejavurouter is connected")
        data = json.dumps(frame, separators=(",", ":")).encode("utf-8") + b"\n"
        with self._write_lock:
            conn = self._conn
            if conn is None:
                raise NotConnectedError("router disconnected")
            conn.sendall(data)

    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            try:
                assert self._listen is not None
                conn, peer = self._listen.accept()
            except socket.timeout:
                continue
            except OSError:
                if self._stop.is_set():
                    return
                continue
            conn.settimeout(None)
            with self._state_lock:
                self._accepted_count += 1
                self._hello = {}
                self._routes = {}
                self._last_error = ""
            with self._write_lock:
                self._close_conn_locked()
                self._conn = conn
            self._connected.clear()
            self._reader_thread = threading.Thread(
                target=self._read_loop, args=(conn,), name=f"{self._name}-reader",
                daemon=True,
            )
            self._reader_thread.start()

    def _read_loop(self, conn: socket.socket) -> None:
        reader = conn.makefile("rb")
        try:
            hello_line = reader.readline(self._max_line + 1)
            if not hello_line:
                return
            try:
                hello = json.loads(hello_line)
            except json.JSONDecodeError:
                hello = {"raw": hello_line.decode("utf-8", "replace").strip()[:200]}
            with self._state_lock:
                self._hello = hello if isinstance(hello, dict) else {"value": hello}
            self._connected.set()
            while not self._stop.is_set():
                line = reader.readline(self._max_line + 1)
                if not line:
                    break
                if len(line) > self._max_line:
                    self._last_error = "line too large"
                    break
                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if not isinstance(message, dict):
                    continue
                if message.get("type") == "response":
                    self._resolve(message)
                elif message.get("type") == "route":
                    self._route_event(message)
        except OSError as error:
            self._last_error = str(error)
        finally:
            try:
                reader.close()
            except OSError:
                pass
            self._connected.clear()
            with self._state_lock:
                self._routes.clear()
            self._last_error = self._last_error or "router disconnected"
            self._fail_all_pending(NotConnectedError("router disconnected"))
            with self._write_lock:
                if self._conn is conn:
                    self._close_conn_locked()

    def _route_event(self, message: dict[str, Any]) -> None:
        route = message.get("route")
        action = message.get("action")
        if not isinstance(route, str) or not route:
            return
        with self._state_lock:
            if action == "register":
                self._routes[route] = {
                    "route": route,
                    "app": message.get("app"),
                    "device_id": message.get("device_id"),
                }
            elif action == "unregister":
                self._routes.pop(route, None)

    def _resolve(self, response: dict[str, Any]) -> None:
        request_id = response.get("id")
        if not isinstance(request_id, str):
            return
        with self._state_lock:
            pending = self._pending.pop(request_id, None)
        if pending is None:
            return
        route = response.get("route")
        if route != pending.route:
            pending._error = CallError({
                "ok": False,
                "id": request_id,
                "route": route,
                "error": {"code": "route_mismatch",
                          "message": f"expected route {pending.route!r}, got {route!r}",
                          "retryable": False},
            })
        elif not response.get("ok"):
            with self._state_lock:
                self._calls_failed += 1
            pending._error = CallError(response)
        else:
            pending._response = response
        pending._event.set()

    def _abandon(self, request_id: str,
                 error: Optional[BaseException] = None) -> None:
        with self._state_lock:
            pending = self._pending.pop(request_id, None)
        if pending is not None and error is not None:
            pending._error = error
            pending._event.set()

    def _fail_all_pending(self, error: BaseException) -> None:
        with self._state_lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for item in pending:
            item._error = error
            item._event.set()

    def _close_conn_locked(self) -> None:
        conn = self._conn
        self._conn = None
        if conn is not None:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                conn.close()
            except OSError:
                pass


def _parse_params(text: str) -> dict[str, Any]:
    value = json.loads(text)
    if not isinstance(value, dict):
        raise argparse.ArgumentTypeError("params must be a JSON object")
    return value


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="dejavurouter TCP hub PoC")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=39010)
    parser.add_argument("--route", required=True, help="opaque route / X account id")
    parser.add_argument("--method", default="token")
    parser.add_argument("--params", type=_parse_params, default={})
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--wait", type=float, default=30.0,
                        help="seconds to wait for the router to connect")
    parser.add_argument("--raw", action="store_true", help="print the full response envelope")
    args = parser.parse_args(argv)

    hub = RouterHub(args.host, args.port)
    hub.start()
    print(f"hub listening on {args.host}:{args.port}", flush=True)
    try:
        if not hub.wait_connected(args.wait):
            print("no dejavurouter connected", flush=True)
            return 2
        print(f"connected: {json.dumps(hub.device, ensure_ascii=False)}", flush=True)
        if args.raw:
            response = hub.call_raw(args.route, args.method, args.params, timeout=args.timeout)
            print(json.dumps(response, ensure_ascii=False, indent=2))
        else:
            result = hub.call(args.route, args.method, args.params, timeout=args.timeout)
            print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except CallError as error:
        print(json.dumps(error.to_dict(), ensure_ascii=False, indent=2))
        return 1
    except HubError as error:
        print(f"{type(error).__name__}: {error}", flush=True)
        return 1
    finally:
        hub.stop()


if __name__ == "__main__":
    raise SystemExit(main())