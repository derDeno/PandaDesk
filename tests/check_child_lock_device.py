"""Exercise a connected desk's lock API and restore its original lock state.

Run: python tests/check_child_lock_device.py http://pandadesk.local
"""

import json
import sys
import urllib.error
import urllib.request
import uuid
from concurrent.futures import ThreadPoolExecutor


base = sys.argv[1].rstrip("/")


def request(path, body=None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        base + path, data=data, headers={"Content-Type": "application/json"}
    )
    try:
        response = urllib.request.urlopen(req, timeout=10)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, json.load(response)


def state():
    status, value = request("/api/v1/state")
    assert status == 200 and value["ready"] is True, (status, value)
    return value["locked"]


def set_lock(wanted):
    status, result = request(
        "/api/v1/commands/child-lock",
        {"request_id": str(uuid.uuid4()), "locked": wanted},
    )
    assert status == 200 and result.get("locked") is wanted, (status, result)
    assert state() is wanted, "The state API must agree with the confirmed command"


status, capabilities = request("/api/v1/capabilities")
assert status == 200 and capabilities["child_lock"] is True, capabilities
original = state()
assert type(original) is bool, "A confirmed initial lock state is required"
try:
    for invalid in ({}, {"request_id": "test", "locked": "true"},
                    {"request_id": "test", "locked": 1}, {"locked": True}):
        status, result = request("/api/v1/commands/child-lock", invalid)
        assert status == 400, (invalid, status, result)
        assert state() is original, "Invalid requests must not change the lock"
    for wanted in (not original, not original, original, original):
        set_lock(wanted)
    set_lock(True)
    status, current = request('/api/v1/state')
    height = current['height']['value']
    assert status == 200 and isinstance(height, (int, float)), current
    for endpoint, command in (
        ('move', {'direction': 'up', 'expires_ms': 1}),
        ('move', {'direction': 'down', 'expires_ms': 1}),
        ('height', {'height_cm': height}),
    ):
        status, result = request('/api/v1/commands/' + endpoint,
                                 {'request_id': str(uuid.uuid4()), **command})
        if status != 409:
            request('/api/v1/commands/move', {'request_id': str(uuid.uuid4()), 'direction': 'stop'})
        assert status == 409 and result.get('error') == 'child_locked', (endpoint, status, result)
    status, result = request('/api/v1/commands/move',
                             {'request_id': str(uuid.uuid4()), 'direction': 'stop'})
    assert status == 202 and result.get('accepted') is True, 'STOP must remain available while locked'
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(request, "/api/v1/commands/child-lock",
                               {"request_id": str(uuid.uuid4()), "locked": not original})
                   for _ in range(2)]
        replies = [future.result() for future in futures]
    assert any(status == 200 for status, _ in replies), replies
    for status, result in replies:
        assert (status == 200 and result.get("locked") is (not original)) or (
            status == 409 and result.get("error") == "child_lock_busy"), (status, result)
    assert state() is (not original), "Concurrent requests must not double-toggle"
    set_lock(original)
    print("Desk checks passed: confirmed/idempotent lock changes, locked movement rejected, STOP available, invalid payloads rejected.")
finally:
    if state() is not original:
        set_lock(original)
