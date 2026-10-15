"""Start bounded competing HTTP requests with separate sessions per worker."""

from concurrent.futures import ThreadPoolExecutor
from threading import Barrier

from .api_client import ApiClient


def concurrent_posts(api, path, payloads):
    assert 1 <= len(payloads) <= 32, "Use 1-32 workers in functional concurrency tests"
    start = Barrier(len(payloads))
    authorization = api.session.headers.get("Authorization", "")
    token = authorization[len("Bearer "):] if authorization.startswith("Bearer ") else None

    def post(payload):
        client = ApiClient(api.base_url, token=token, timeout=30)
        try:
            start.wait(timeout=15)
            return client.post(path, json=payload)
        finally:
            client.close()

    with ThreadPoolExecutor(max_workers=len(payloads)) as executor:
        return list(executor.map(post, payloads))
