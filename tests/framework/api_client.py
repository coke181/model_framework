"""Small HTTP test client with project-specific response assertions."""

from __future__ import annotations

from typing import Any, Dict, Iterable, Optional, Union

import requests


class ApiContractError(AssertionError):
    """Raised when an HTTP response violates the service API contract."""


class ApiClient:
    """Wrap requests.Session and validate the service's JSON envelope."""

    def __init__(
        self,
        base_url: str,
        *,
        token: Optional[str] = None,
        timeout: float = 10,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()
        self.session.headers.update({"Accept": "application/json"})
        if token:
            self.session.headers.update({"Authorization": f"Bearer {token}"})

    def close(self) -> None:
        self.session.close()

    def request(
        self,
        method: str,
        path: str,
        *,
        expected: Optional[Union[int, Iterable[int]]] = None,
        **kwargs: Any,
    ) -> requests.Response:
        timeout = kwargs.pop("timeout", self.timeout)
        response = self.session.request(
            method,
            self.base_url + path,
            timeout=timeout,
            **kwargs,
        )
        if expected is not None:
            statuses = {expected} if isinstance(expected, int) else set(expected)
            if response.status_code not in statuses:
                raise ApiContractError(
                    f"{method.upper()} {path}: expected HTTP {sorted(statuses)}, "
                    f"got {response.status_code}; body={response.text[:2000]}"
                )
        return response

    def get(self, path: str, **kwargs: Any) -> requests.Response:
        return self.request("GET", path, **kwargs)

    def post(self, path: str, **kwargs: Any) -> requests.Response:
        return self.request("POST", path, **kwargs)

    def put(self, path: str, **kwargs: Any) -> requests.Response:
        return self.request("PUT", path, **kwargs)

    def delete(self, path: str, **kwargs: Any) -> requests.Response:
        return self.request("DELETE", path, **kwargs)

    @staticmethod
    def json(response: requests.Response) -> Dict[str, Any]:
        try:
            body = response.json()
        except ValueError as exc:
            raise ApiContractError(
                f"response is not JSON; content-type={response.headers.get('content-type')!r}; "
                f"body={response.text[:1000]}"
            ) from exc
        if not isinstance(body, dict):
            raise ApiContractError(f"response JSON must be an object, got {type(body).__name__}")
        return body

    @classmethod
    def data(cls, response: requests.Response) -> Any:
        body = cls.json(response)
        if body.get("ok") is not True:
            raise ApiContractError(f"expected ok=true response, got {body!r}")
        if "data" not in body:
            raise ApiContractError(f"successful response has no data field: {body!r}")
        return body["data"]

    @classmethod
    def error_code(cls, response: requests.Response) -> str:
        body = cls.json(response)
        if body.get("ok") is not False:
            raise ApiContractError(f"expected ok=false response, got {body!r}")
        error = body.get("error")
        if not isinstance(error, dict) or not isinstance(error.get("code"), str):
            raise ApiContractError(f"error response has no error.code: {body!r}")
        return error["code"]

    def with_token(self, token: str) -> "ApiClient":
        return ApiClient(self.base_url, token=token, timeout=self.timeout)
