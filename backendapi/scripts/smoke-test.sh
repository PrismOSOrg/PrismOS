#!/usr/bin/env sh
set -eu

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
TMP_FILE="$(mktemp)"
trap 'rm -f "$TMP_FILE"' EXIT

expect_status() {
    expected="$1"
    url="$2"
    actual="$(curl -sS -o "$TMP_FILE" -w '%{http_code}' "$url")"
    if [ "$actual" != "$expected" ]; then
        printf 'Expected HTTP %s, got %s for %s\n' "$expected" "$actual" "$url" >&2
        cat "$TMP_FILE" >&2
        exit 1
    fi
}

curl -fsS "$BASE_URL/healthz" | grep -q '^ok'
curl -fsS "$BASE_URL/api/v1/catalog" | grep -q '^PRISMPKG-CATALOG/1'
curl -fsS "$BASE_URL/api/v1/catalog" | grep -q 'hello[[:space:]]1.0.0'
curl -fsS "$BASE_URL/api/v1/packages/hello/1.0.0/manifest" | grep -q '^PRISMPKG-MANIFEST/1'

expect_status 200 "$BASE_URL/api/v1/packages/hello/1.0.0/chunk?offset=0&length=64"
[ "$(wc -c < "$TMP_FILE" | tr -d ' ')" = "64" ]
expect_status 400 "$BASE_URL/api/v1/packages/hello/1.0.0/chunk?offset=0&length=4097"
expect_status 416 "$BASE_URL/api/v1/packages/hello/1.0.0/chunk?offset=999999&length=1"
expect_status 404 "$BASE_URL/api/v1/packages/not-installed/1.0.0/manifest"

printf 'Package repository smoke tests passed.\n'
