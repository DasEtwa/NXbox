"""Minimal Xbox Device Portal client; credentials never enter diagnostics."""

import base64
import http.cookiejar
import json
import os
import ssl
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid


class PortalError(RuntimeError):
    def __init__(self, method, endpoint, status=None):
        self.status = status
        super().__init__(f"Device Portal {method} {endpoint}: {status or 'connection failed'}")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def credentials():
    stored = {}
    if sys.platform == "darwin" and not all(
        name in os.environ
        for name in ("NXBOX_PORTAL_HOST", "NXBOX_PORTAL_USER", "NXBOX_PORTAL_PASS")
    ):
        result = subprocess.run(
            ["security", "find-generic-password", "-s", "claude-autonomous:XBDEV", "-w"],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            try:
                stored = json.loads(result.stdout)
            except ValueError:
                raise RuntimeError("Invalid Device Portal keychain record") from None
    host = os.environ.get("NXBOX_PORTAL_HOST", stored.get("host", ""))
    user = os.environ.get("NXBOX_PORTAL_USER", stored.get("user", ""))
    password = os.environ.get("NXBOX_PORTAL_PASS", stored.get("pass", ""))
    if not host or not user or not password:
        raise RuntimeError("Set NXBOX_PORTAL_HOST/USER/PASS or the Device Portal keychain record")
    return host, user, password


class Portal:
    def __init__(self, host, user, password):
        base = host if "://" in host else f"https://{host}"
        parsed = urllib.parse.urlsplit(base)
        if (
            parsed.scheme != "https"
            or not parsed.hostname
            or parsed.username
            or parsed.path
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("Device Portal host must be an HTTPS host without credentials or path")
        self.base = base.rstrip("/") if parsed.port else f"{base.rstrip('/')}:{11443}"
        self.auth = "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()
        self.cookies = http.cookiejar.CookieJar()
        # Dev Mode consoles present self-signed certificates. Do not follow redirects.
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPSHandler(context=ssl._create_unverified_context()),
            urllib.request.HTTPCookieProcessor(self.cookies),
            NoRedirect(),
        )
        self.csrf = None

    def request(self, method, endpoint, query=None, body=None, content_type=None):
        if method != "GET" and self.csrf is None:
            self.request("GET", "/api/os/machinename")
            self.csrf = next(
                (c.value for c in self.cookies if c.name.lower() == "csrf-token"), None
            )
            if self.csrf is None:
                raise RuntimeError("Device Portal did not provide a CSRF token")
        headers = {"Authorization": self.auth}
        if method != "GET":
            headers["X-CSRF-Token"] = self.csrf
        if content_type:
            headers["Content-Type"] = content_type
        url = self.base + endpoint
        if query:
            url += "?" + urllib.parse.urlencode(query)
        req = urllib.request.Request(url, data=body, headers=headers, method=method)
        try:
            with self.opener.open(req, timeout=30) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            raise PortalError(method, endpoint, error.code) from None
        except (OSError, urllib.error.URLError):
            raise PortalError(method, endpoint) from None

    def json(self, endpoint, query=None):
        try:
            return json.loads(self.request("GET", endpoint, query))
        except ValueError:
            raise RuntimeError("Device Portal returned invalid JSON") from None

    def files(self, package, directory="LocalState", name=None):
        query = {
            "knownfolderid": "LocalAppData",
            "packagefullname": package,
            "path": "/" + directory.replace("\\", "/").strip("/"),
        }
        if name is not None:
            query["filename"] = name
        return query

    def pull(self, package, name, directory="LocalState", optional=False):
        try:
            return self.request(
                "GET", "/api/filesystem/apps/file", self.files(package, directory, name)
            )
        except PortalError as error:
            if optional and error.status == 404:
                return None
            raise

    def delete(self, package, name, directory="LocalState"):
        try:
            self.request(
                "DELETE", "/api/filesystem/apps/file", self.files(package, directory, name)
            )
        except PortalError as error:
            if error.status != 404:
                raise

    def push(self, package, name, data, directory="LocalState"):
        if any(c in name for c in '\r\n"/\\'):
            raise ValueError("Invalid upload filename")
        boundary = uuid.uuid4().hex
        body = (
            (
                f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; '
                f'filename="{name}"\r\nContent-Type: application/octet-stream\r\n\r\n'
            ).encode()
            + data
            + f"\r\n--{boundary}--\r\n".encode()
        )
        self.request(
            "POST",
            "/api/filesystem/apps/file",
            self.files(package, directory),
            body,
            f"multipart/form-data; boundary={boundary}",
        )

    def launch(self, package):
        encode = lambda value: base64.b64encode(value.encode()).decode()
        self.request(
            "POST",
            "/api/taskmanager/app",
            {
                "appid": encode(package["PackageRelativeId"]),
                "package": encode(package["PackageFullName"]),
            },
        )

    def stop(self, package):
        # Process deletion also covers sessions whose app activation has already failed.
        for process in self.json("/api/resourcemanager/processes").get("Processes", []):
            if process.get("PackageFullName") == package:
                try:
                    self.request(
                        "DELETE", "/api/taskmanager/process", {"pid": process["ProcessId"]}
                    )
                except PortalError as error:
                    if error.status != 404:
                        raise

    def running(self, package):
        return any(
            p.get("PackageFullName") == package
            for p in self.json("/api/resourcemanager/processes").get("Processes", [])
        )

    def dump_names(self, package):
        payload = self.json("/api/debug/dump/usermode/dumps")
        names = set()

        def visit(node, belongs=False):
            if isinstance(node, dict):
                owner = node.get("PackageFullName", node.get("packageFullName"))
                belongs = owner == package if owner is not None else belongs
                name = node.get("FileName", node.get("Filename", node.get("fileName")))
                if belongs and isinstance(name, str):
                    names.add(name)
                for value in node.values():
                    visit(value, belongs)
            elif isinstance(node, list):
                for value in node:
                    visit(value, belongs)

        visit(payload)
        return names
