#!/usr/bin/env python3
"""Loopback-only browser editor for the local wallpaper library."""

from __future__ import annotations

from concurrent.futures import Future, TimeoutError as FutureTimeoutError
import json
from http import HTTPStatus
from ota import MAX_FIRMWARE_BYTES, OtaError

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import sys
import threading
from typing import Any, BinaryIO, Callable
from urllib.parse import parse_qs, urlsplit

from wallpaper_library import (
    DEFAULT_FIT_MODE,
    DEFAULT_FRAME_RATE,
    MAX_UPLOAD_BYTES,
    WALLPAPER_ID_PATTERN,
    WallpaperError,
    WallpaperLibrary,
    WallpaperNotFoundError,
    WallpaperStorageError,
    copy_bounded_upload,
)


JSON_LIMIT = 4096
CONTROL_TIMEOUT_SECONDS = 10
CLAUDE_BODY_LIMIT = 16384
CLAUDE_REQUEST_PREFIX = "/api/claude/requests/"


class EditorServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        library: WallpaperLibrary,
        assets: Path,
        activate: Callable[[str], Future[dict[str, Any]]],
        status: Callable[[], Future[dict[str, Any]]],
        delete: Callable[[str], Future[dict[str, Any]]],
        event: Callable[[dict[str, Any]], Future[dict[str, Any]]] | None = None,
        claude: Callable[[str, Any], Future[dict[str, Any]]] | None = None,
        display: tuple[Callable[[], dict[str, Any]],
                       Callable[[Any], Future[dict[str, Any]]]] | None = None,
        ota: Callable[[bytes], Future[dict[str, Any]]] | None = None,
        codex: Callable[[Any], Future[dict[str, Any]]] | None = None,
        hub: Callable[[str,str,Any], Future[dict[str,Any]]] | None = None,
    ) -> None:
        self.library = library
        self.ota_callback = ota
        self.claude_callback = claude
        self.codex_callback = codex
        self.hub_callback = hub
        self.display_callbacks = display
        self.assets = assets.resolve()
        self.activate_callback = activate
        self.status_callback = status
        self.delete_callback = delete
        self.event_callback = event
        self.upload_lock = threading.Lock()
        super().__init__(address, EditorRequestHandler)

    def handle_error(self, _request: object, _client_address: object) -> None:
        error = sys.exc_info()[1]
        if isinstance(error, (BrokenPipeError, ConnectionResetError, TimeoutError)):
            return
        # Do not let a URL query containing a local filename enter a traceback.
        print(
            f"Wallpaper editor request failed: {type(error).__name__}",
            file=sys.stderr,
            flush=True,
        )


class EditorRequestHandler(BaseHTTPRequestHandler):
    server: EditorServer
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args: object) -> None:
        # Uploaded names and request query strings must never leak to logs.
        return

    def version_string(self) -> str:
        return "DevCompanionEditor/1"

    def handle_expect_100(self) -> bool:
        try:
            length = self._content_length(MAX_UPLOAD_BYTES)
        except WallpaperError as error:
            self._send_error(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, str(error))
            return False
        if length <= 0:
            self._send_error(HTTPStatus.BAD_REQUEST, "请求正文不能为空")
            return False
        self.send_response_only(HTTPStatus.CONTINUE)
        self.end_headers()
        return True

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        split = urlsplit(self.path)
        path = split.path
        if path.startswith("/api/hub/"):
            self._hub(path, "GET", split.query)
            return
        if path in {"/", "/index.html"}:
            self._serve_asset("wallpaper_editor.html", "text/html; charset=utf-8")
            return
        if path == "/assets/editor.css":
            self._serve_asset("wallpaper_editor.css", "text/css; charset=utf-8")
            return
        if path == "/assets/editor.js":
            self._serve_asset("wallpaper_editor.js", "text/javascript; charset=utf-8")
            return
        if path == "/api/wallpapers":
            records, active_id = self.server.library.snapshot()
            records = [
                item.public_dict(active=item.wallpaper_id == active_id)
                for item in records
            ]
            self._send_json(HTTPStatus.OK, {"wallpapers": records, "active_id": active_id})
            return
        if path.startswith(CLAUDE_REQUEST_PREFIX):
            if self._local_caller(require_json=False) and self._claude_available():
                self._claude("poll", path.removeprefix(CLAUDE_REQUEST_PREFIX))
            return
        if path == "/api/display-settings":
            if self._local_caller(require_json=False):
                self._display_settings("GET")
            return
        if path == "/api/status":
            try:
                payload = self.server.status_callback().result(
                    timeout=CONTROL_TIMEOUT_SECONDS
                )
            except (FutureTimeoutError, Exception) as error:
                self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Bridge 状态暂不可用")
                return
            self._send_json(HTTPStatus.OK, payload)
            return
        if path.startswith("/thumb/"):
            wallpaper_id = path.removeprefix("/thumb/")
            try:
                handle = self.server.library.open_thumbnail(wallpaper_id)
            except WallpaperError as error:
                self._send_error(HTTPStatus.NOT_FOUND, str(error))
                return
            self._serve_open_file(
                handle, "image/jpeg", allow_ranges=False, cache=True
            )
            return
        if path.startswith("/media/"):
            wallpaper_id = path.removeprefix("/media/")
            try:
                record, handle = self.server.library.open_source(wallpaper_id)
            except WallpaperError as error:
                self._send_error(HTTPStatus.NOT_FOUND, str(error))
                return
            self._serve_open_file(
                handle,
                record.source_media_type,
                allow_ranges=True,
                cache=False,
            )
            return
        self._send_error(HTTPStatus.NOT_FOUND, "资源不存在")

    def do_POST(self) -> None:  # noqa: N802
        split = urlsplit(self.path)
        if split.path.startswith("/api/hub/"):
            self._hub(split.path,"POST",split.query)
            return
        if split.path == "/api/codex/status":
            if not self._local_caller(require_json=True):
                return
            if self.server.codex_callback is None:
                self._send_error(HTTPStatus.NOT_FOUND, "Codex status API unavailable")
                return
            try:
                if self.headers.get("Transfer-Encoding"):
                    raise ValueError("Chunked bodies are unsupported")
                length = self._content_length(CLAUDE_BODY_LIMIT)
                raw = self.rfile.read(length)
                if len(raw) != length:
                    raise ValueError("Incomplete body")
                result = self.server.codex_callback(json.loads(raw)).result(timeout=CONTROL_TIMEOUT_SECONDS)
            except (ValueError, WallpaperError, RecursionError, UnicodeError):
                self.close_connection = True
                self._send_error(HTTPStatus.BAD_REQUEST, "Malformed Codex status payload")
                return
            except Exception:
                self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Codex tracker unavailable")
                return
            self._send_json(HTTPStatus.OK, result)
            return
        if split.path == "/api/ota":
            self._ota_upload()
            return
        if split.path == "/api/events":
            self._publish_event()
            return
        if split.path in {"/api/claude/status", "/api/claude/requests"}:
            if not self._local_caller(require_json=True) or not self._claude_available():
                return
            try:
                if self.headers.get("Transfer-Encoding"):
                    raise ValueError("Chunked bodies are unsupported")
                length = self._content_length(CLAUDE_BODY_LIMIT)
                raw = self.rfile.read(length)
                if len(raw) != length:
                    raise ValueError("Incomplete body")
                payload = json.loads(raw)
            except (ValueError, WallpaperError, RecursionError):
                self.close_connection = True
                self._send_error(HTTPStatus.BAD_REQUEST, "Malformed Claude hook payload")
                return
            self._claude("status" if split.path.endswith("/status") else "open", payload)
            return
        if split.path != "/api/wallpapers":
            self._send_error(HTTPStatus.NOT_FOUND, "资源不存在")
            return
        if self.headers.get("Transfer-Encoding"):
            self._send_error(HTTPStatus.BAD_REQUEST, "不支持分块上传")
            return
        if not self.server.upload_lock.acquire(blocking=False):
            self._send_error(HTTPStatus.CONFLICT, "另一个视频正在导入，请稍后再试")
            return
        try:
            self._handle_wallpaper_upload(split.query)
        finally:
            self.server.upload_lock.release()

    def _hub(self,path,method,query):
        if not self._local_caller(require_json=method != "GET"):return
        if self.server.hub_callback is None:
            self._send_error(HTTPStatus.NOT_FOUND,"Desktop hub unavailable");return
        try:
            payload={k:v[0] for k,v in parse_qs(query).items()}
            if method != "GET":
                if self.headers.get("Transfer-Encoding"):raise ValueError("Chunked bodies unsupported")
                length=self._content_length(16384)
                raw=self.rfile.read(length)
                if len(raw)!=length:raise ValueError("Incomplete body")
                payload=json.loads(raw)
            result=self.server.hub_callback(path.removeprefix("/api/hub/"),method,payload).result(timeout=CONTROL_TIMEOUT_SECONDS)
            self._send_json(HTTPStatus.OK,result)
        except (ValueError,WallpaperError,RecursionError,UnicodeError) as error:
            self.close_connection=True
            self._send_error(HTTPStatus.BAD_REQUEST,str(error)[:160] or "Invalid hub request")
        except Exception:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE,"Bridge hub result not available")

    def _ota_upload(self) -> None:
        if not self._local_caller(require_json=False):
            return
        if self.headers.get_content_type() != "application/octet-stream" or self.headers.get("Transfer-Encoding"):
            self.close_connection = True
            self._send_error(HTTPStatus.BAD_REQUEST, "Expected a bounded firmware binary")
            return
        if self.server.ota_callback is None:
            self._send_error(HTTPStatus.NOT_FOUND, "OTA unavailable")
            return
        if not self.server.upload_lock.acquire(blocking=False):
            self._send_error(HTTPStatus.CONFLICT, "Another upload is in progress")
            return
        try:
            length = self._content_length(MAX_FIRMWARE_BYTES)
            self.connection.settimeout(15)
            data = self.rfile.read(length)
            if len(data) != length:
                raise OtaError("Incomplete firmware body")
            result = self.server.ota_callback(data).result(timeout=20)
            self._send_json(HTTPStatus.ACCEPTED, result)
        except (OtaError, WallpaperError) as error:
            self.close_connection = True
            self._send_error(HTTPStatus.BAD_REQUEST, str(error))
        except (FutureTimeoutError, TimeoutError):
            self.close_connection = True
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Request timed out; check update status before retrying")
        except Exception:
            self.close_connection = True
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "OTA unavailable")
        finally:
            self.server.upload_lock.release()

    def _handle_wallpaper_upload(self, query_string: str) -> None:
        try:
            length = self._content_length(MAX_UPLOAD_BYTES)
            query = parse_qs(query_string, keep_blank_values=True)
            name = query.get("name", ["未命名动态壁纸"])[0]
            fit = query.get("fit", [DEFAULT_FIT_MODE])[0]
            fps = query.get("fps", [str(DEFAULT_FRAME_RATE)])[0]
            activate = query.get("activate", ["1"])[0] != "0"
        except WallpaperError as error:
            self._send_error(HTTPStatus.BAD_REQUEST, str(error))
            return

        upload = self.server.library.new_upload_path()
        try:
            copy_bounded_upload(self.rfile, upload, length)
            record = self.server.library.import_upload(
                upload, display_name=name, fit=fit, fps=fps
            )
            if activate:
                self.server.activate_callback(record.wallpaper_id).result(
                    timeout=CONTROL_TIMEOUT_SECONDS
                )
            payload = record.public_dict(active=activate)
            self._send_json(HTTPStatus.CREATED, {"wallpaper": payload})
        except WallpaperError as error:
            self.server.library.discard_upload(upload)
            self._send_error(HTTPStatus.UNPROCESSABLE_ENTITY, str(error))
        except FutureTimeoutError:
            self.server.library.discard_upload(upload)
            self._send_error(
                HTTPStatus.SERVICE_UNAVAILABLE,
                "视频已导入，但 Bridge 未能及时应用；请在壁纸库中重试",
            )
        except Exception:
            self.server.library.discard_upload(upload)
            self._send_error(HTTPStatus.INTERNAL_SERVER_ERROR, "动态壁纸处理失败")

    def _local_caller(self, *, require_json: bool) -> bool:
        """Hook calls come from processes on this Mac. Refuse anything a web
        page could forge: foreign Host (DNS rebinding), cross-site fetches,
        foreign Origin, and non-JSON bodies for state-changing requests."""
        port = self.server.server_address[1]
        host = self.headers.get("Host", "")
        origin = self.headers.get("Origin")
        if (host not in {f"127.0.0.1:{port}", f"localhost:{port}"} or
                (require_json and self.headers.get_content_type() != "application/json") or
                (origin is not None and origin != f"http://{host}") or
                self.headers.get("Sec-Fetch-Site") == "cross-site"):
            self.close_connection = True
            self._send_error(HTTPStatus.FORBIDDEN, "This API is local only")
            return False
        return True

    def _claude_available(self) -> bool:
        if self.server.claude_callback is None:
            self._send_error(HTTPStatus.NOT_FOUND, "Claude hook API unavailable")
            return False
        return True

    def _display_settings(self, method: str) -> None:
        if self.server.display_callbacks is None:
            self._send_error(HTTPStatus.NOT_FOUND, "Display settings unavailable")
            return
        read, write = self.server.display_callbacks
        if method == "GET":
            message = read()
            self._send_json(HTTPStatus.OK, {k: v for k, v in message.items()
                                            if k not in {"v", "type"}})
            return
        try:
            if self.headers.get("Transfer-Encoding"):
                raise ValueError("Chunked bodies are unsupported")
            length = self._content_length(JSON_LIMIT)
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("Incomplete body")
            result = write(json.loads(raw)).result(timeout=CONTROL_TIMEOUT_SECONDS)
        except (ValueError, WallpaperError, RecursionError) as error:
            self.close_connection = True
            self._send_error(HTTPStatus.BAD_REQUEST, str(error) or "Invalid display settings")
            return
        except FutureTimeoutError:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Bridge did not answer in time")
            return
        self._send_json(HTTPStatus.OK, result)

    def _claude(self, operation: str, payload: Any) -> None:
        try:
            result = self.server.claude_callback(operation, payload).result(
                timeout=CONTROL_TIMEOUT_SECONDS)
        except ValueError:
            self._send_error(HTTPStatus.BAD_REQUEST, "Unsupported Claude hook payload")
            return
        except FutureTimeoutError:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Bridge did not answer in time")
            return
        except Exception:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Claude broker unavailable")
            return
        self._send_json(HTTPStatus.OK, result)

    def _publish_event(self) -> None:
        # A local browser page cannot publish unsolicited LAN events through
        # a cross-origin form/fetch. CLI publishers use application/json.
        expected_host = f"127.0.0.1:{self.server.server_address[1]}"
        host = self.headers.get("Host", "")
        origin = self.headers.get("Origin")
        if (host not in {expected_host, f"localhost:{self.server.server_address[1]}"} or
                self.headers.get_content_type() != "application/json" or
                (origin is not None and origin != f"http://{host}") or
                self.headers.get("Sec-Fetch-Site") == "cross-site"):
            self.close_connection = True
            self._send_error(HTTPStatus.FORBIDDEN, "Event publisher must be local and same-origin")
            return
        if self.server.event_callback is None:
            self._send_error(HTTPStatus.NOT_FOUND, "Event publisher unavailable")
            return
        try:
            if self.headers.get("Transfer-Encoding"):
                raise ValueError("Chunked event bodies are unsupported")
            length = self._content_length(2048)
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("Incomplete event body")
            packet = json.loads(raw)
            result = self.server.event_callback(packet).result(timeout=CONTROL_TIMEOUT_SECONDS)
            self._send_json(HTTPStatus.OK, result)
        except (ValueError, WallpaperError, RecursionError):
            self.close_connection = True
            self._send_error(HTTPStatus.BAD_REQUEST, "Malformed or unsupported event packet")
        except FutureTimeoutError:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Event delivery not confirmed")
        except Exception:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Event publisher unavailable")

    def do_PUT(self) -> None:  # noqa: N802
        if urlsplit(self.path).path == "/api/display-settings":
            if self._local_caller(require_json=True):
                self._display_settings("PUT")
            return
        if urlsplit(self.path).path != "/api/active":
            self._send_error(HTTPStatus.NOT_FOUND, "资源不存在")
            return
        if self.headers.get("Transfer-Encoding"):
            self._send_error(HTTPStatus.BAD_REQUEST, "不支持分块请求")
            return
        try:
            length = self._content_length(JSON_LIMIT)
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise WallpaperError("请求正文不完整")
            payload = json.loads(raw)
            wallpaper_id = payload.get("id") if isinstance(payload, dict) else None
            if not isinstance(wallpaper_id, str):
                raise WallpaperError("动态壁纸 ID 无效")
            result = self.server.activate_callback(wallpaper_id).result(
                timeout=CONTROL_TIMEOUT_SECONDS
            )
            self._send_json(HTTPStatus.OK, result)
        except (WallpaperError, json.JSONDecodeError) as error:
            self._send_error(HTTPStatus.BAD_REQUEST, str(error))
        except FutureTimeoutError:
            self._send_error(HTTPStatus.SERVICE_UNAVAILABLE, "Bridge 应用动态壁纸超时")
        except Exception:
            self._send_error(HTTPStatus.INTERNAL_SERVER_ERROR, "无法应用动态壁纸")

    def do_DELETE(self) -> None:  # noqa: N802
        split = urlsplit(self.path)
        prefix = "/api/wallpapers/"
        if not split.path.startswith(prefix):
            self._send_error(HTTPStatus.NOT_FOUND, "资源不存在", code="not_found")
            return
        wallpaper_id = split.path.removeprefix(prefix)
        if split.query or WALLPAPER_ID_PATTERN.fullmatch(wallpaper_id) is None:
            self._send_error(
                HTTPStatus.BAD_REQUEST,
                "动态壁纸 ID 无效",
                code="invalid_wallpaper_id",
            )
            return
        if self.headers.get("Transfer-Encoding") or not self._has_empty_body():
            self._send_error(
                HTTPStatus.BAD_REQUEST,
                "删除请求不能包含正文",
                code="invalid_request_body",
            )
            return
        try:
            result = self.server.delete_callback(wallpaper_id).result(
                timeout=CONTROL_TIMEOUT_SECONDS
            )
            self._send_json(HTTPStatus.OK, result)
        except WallpaperNotFoundError as error:
            self._send_error(
                HTTPStatus.NOT_FOUND, str(error), code="not_found"
            )
        except WallpaperStorageError as error:
            self._send_error(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                str(error),
                code="delete_failed",
            )
        except FutureTimeoutError:
            self._send_error(
                HTTPStatus.SERVICE_UNAVAILABLE,
                "无法确认删除结果；请刷新壁纸库后再重试",
                code="outcome_unknown",
            )
        except Exception:
            self._send_error(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                "无法删除动态壁纸",
                code="delete_failed",
            )

    def do_HEAD(self) -> None:  # noqa: N802
        split = urlsplit(self.path)
        if split.path.startswith("/media/"):
            wallpaper_id = split.path.removeprefix("/media/")
            try:
                record, handle = self.server.library.open_source(wallpaper_id)
            except WallpaperError as error:
                self._send_error(HTTPStatus.NOT_FOUND, str(error), include_body=False)
                return
            self._serve_open_file(
                handle,
                record.source_media_type,
                allow_ranges=True,
                cache=False,
                include_body=False,
            )
            return
        self._send_error(HTTPStatus.NOT_FOUND, "资源不存在", include_body=False)

    def _content_length(self, maximum: int) -> int:
        values = self.headers.get_all("Content-Length", failobj=[])
        if len(values) != 1:
            raise WallpaperError("必须提供唯一的 Content-Length")
        try:
            length = int(values[0], 10)
        except ValueError as error:
            raise WallpaperError("Content-Length 无效") from error
        if length <= 0 or length > maximum:
            raise WallpaperError(f"请求正文必须在 1 到 {maximum} 字节之间")
        return length

    def _has_empty_body(self) -> bool:
        values = self.headers.get_all("Content-Length", failobj=[])
        if len(values) > 1:
            return False
        if not values:
            return True
        try:
            return int(values[0], 10) == 0
        except ValueError:
            return False

    def _serve_asset(self, filename: str, content_type: str) -> None:
        target = self.server.assets / filename
        try:
            target.relative_to(self.server.assets)
        except ValueError:
            self._send_error(HTTPStatus.NOT_FOUND, "资源不存在")
            return
        self._serve_file(target, content_type, allow_ranges=False, cache=False)

    def _serve_file(
        self,
        target: Path,
        content_type: str,
        *,
        allow_ranges: bool,
        cache: bool,
        include_body: bool = True,
    ) -> None:
        try:
            handle = target.open("rb")
        except OSError:
            self._send_error(HTTPStatus.NOT_FOUND, "资源不存在", include_body=include_body)
            return
        self._serve_open_file(
            handle,
            content_type,
            allow_ranges=allow_ranges,
            cache=cache,
            include_body=include_body,
        )

    def _serve_open_file(
        self,
        handle: BinaryIO,
        content_type: str,
        *,
        allow_ranges: bool,
        cache: bool,
        include_body: bool = True,
    ) -> None:
        try:
            size = os.fstat(handle.fileno()).st_size
        except OSError:
            handle.close()
            self._send_error(
                HTTPStatus.NOT_FOUND, "资源不存在", include_body=include_body
            )
            return
        start = 0
        end = size - 1
        status = HTTPStatus.OK
        range_header = self.headers.get("Range") if allow_ranges else None
        if range_header:
            parsed = self._parse_range(range_header, size)
            if parsed is None:
                with handle:
                    self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                    self._security_headers()
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                return
            start, end = parsed
            status = HTTPStatus.PARTIAL_CONTENT
        length = max(0, end - start + 1)
        self.send_response(status)
        self._security_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "private, max-age=86400" if cache else "no-store")
        if allow_ranges:
            self.send_header("Accept-Ranges", "bytes")
        if status == HTTPStatus.PARTIAL_CONTENT:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        with handle:
            if not include_body or length == 0:
                return
            try:
                handle.seek(start)
                remaining = length
                while remaining:
                    chunk = handle.read(min(1024 * 1024, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError, OSError):
                return

    @staticmethod
    def _parse_range(value: str, size: int) -> tuple[int, int] | None:
        if not value.startswith("bytes=") or "," in value or size <= 0:
            return None
        spec = value[6:].strip()
        if "-" not in spec:
            return None
        left, right = spec.split("-", 1)
        try:
            if left:
                start = int(left, 10)
                end = int(right, 10) if right else size - 1
                if start < 0 or end < start or start >= size:
                    return None
                return start, min(end, size - 1)
            suffix = int(right, 10)
            if suffix <= 0:
                return None
            suffix = min(suffix, size)
            return size - suffix, size - 1
        except ValueError:
            return None

    def _send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        )
        self.send_response(status)
        self._security_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def _send_error(
        self,
        status: HTTPStatus,
        message: str,
        *,
        include_body: bool = True,
        code: str | None = None,
    ) -> None:
        payload = {"error": message}
        if code is not None:
            payload["code"] = code
        encoded = (
            json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode(
                "utf-8"
            )
            if include_body
            else b""
        )
        self.send_response(status)
        self._security_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        if encoded:
            self.wfile.write(encoded)

    def _security_headers(self) -> None:
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'none'; script-src 'self'; style-src 'self'; "
            "img-src 'self' blob: data:; media-src 'self' blob:; "
            "connect-src 'self'; base-uri 'none'; form-action 'none'; "
            "frame-ancestors 'none'",
        )


class WallpaperEditorService:
    def __init__(
        self,
        library: WallpaperLibrary,
        assets: Path,
        port: int,
        activate: Callable[[str], Future[dict[str, Any]]],
        status: Callable[[], Future[dict[str, Any]]],
        delete: Callable[[str], Future[dict[str, Any]]],
        event: Callable[[dict[str, Any]], Future[dict[str, Any]]] | None = None,
        claude: Callable[[str, Any], Future[dict[str, Any]]] | None = None,
        display: tuple[Callable[[], dict[str, Any]],
                       Callable[[Any], Future[dict[str, Any]]]] | None = None,
        ota: Callable[[bytes], Future[dict[str, Any]]] | None = None,
        codex: Callable[[Any], Future[dict[str, Any]]] | None = None,
        hub: Callable[[str,str,Any], Future[dict[str,Any]]] | None = None,
    ) -> None:
        self._server = EditorServer(
            ("127.0.0.1", port), library, assets, activate, status, delete, event, claude,
            display, ota, codex, hub,
        )
        self._thread = threading.Thread(
            target=self._server.serve_forever,
            name="wallpaper-editor-http",
            daemon=True,
        )

    @property
    def port(self) -> int:
        return int(self._server.server_address[1])

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=5)
