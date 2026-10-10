#!/usr/bin/env sh
set -eu

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
STAMP="$(date +%s)"
USERNAME="smoke${STAMP}"
PASSWORD="PrismSmoke-${STAMP}-Pass"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

expect_status() {
    expected="$1"
    shift
    actual="$(curl -sS -o "$TMP_DIR/response" -w '%{http_code}' "$@")"
    if [ "$actual" != "$expected" ]; then
        printf 'Expected HTTP %s, got %s\n' "$expected" "$actual" >&2
        cat "$TMP_DIR/response" >&2
        exit 1
    fi
}

expect_status 401 "$BASE_URL/api/v1/auth/me"
expect_status 401 -X POST "$BASE_URL/api/v1/uploads" \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-Package-Id: unauthenticated' -H 'X-Package-Version: 1.0.0' \
    --data-binary @Packages/hello/1.0.0/hello.prpkg

curl -fsS -X POST "$BASE_URL/api/v1/auth/register" \
    -H 'Content-Type: application/json' \
    --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD\"}" \
    > "$TMP_DIR/register.json"
TOKEN="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["accessToken"])' < "$TMP_DIR/register.json")"

curl -fsS "$BASE_URL/api/v1/auth/me" -H "Authorization: Bearer $TOKEN" \
    | grep -q "\"username\":\"$USERNAME\""
expect_status 401 -X POST "$BASE_URL/api/v1/auth/login" \
    -H 'Content-Type: application/json' \
    --data "{\"username\":\"$USERNAME\",\"password\":\"wrong-password\"}"

curl -fsS -X POST "$BASE_URL/api/v1/auth/login" \
    -H 'Content-Type: application/json' \
    --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD\"}" \
    > "$TMP_DIR/login.json"
TOKEN="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["accessToken"])' < "$TMP_DIR/login.json")"

UPLOAD_STATUS="$(curl -sS -o "$TMP_DIR/upload.json" -w '%{http_code}' \
    -X POST "$BASE_URL/api/v1/uploads" \
    -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/octet-stream' \
    -H 'X-Package-Id: smoke' -H 'X-Package-Version: 1.0.0' \
    -H 'X-Package-Description: Authenticated upload smoke test' \
    --data-binary @Packages/hello/1.0.0/hello.prpkg)"
if [ "$UPLOAD_STATUS" != "201" ] && [ "$UPLOAD_STATUS" != "409" ]; then
    printf 'Expected upload status 201 or 409, got %s\n' "$UPLOAD_STATUS" >&2
    cat "$TMP_DIR/upload.json" >&2
    exit 1
fi

curl -fsS -X PUT "$BASE_URL/api/v1/users/me/favorites/smoke/1.0.0" \
    -H "Authorization: Bearer $TOKEN" -o /dev/null
curl -fsS "$BASE_URL/api/v1/users/me/favorites" \
    -H "Authorization: Bearer $TOKEN" | grep -q '"id":"smoke"'

curl -fsS "$BASE_URL/api/v1/packages/smoke/1.0.0/chunk?offset=0&length=64" \
    -H "Authorization: Bearer $TOKEN" -o "$TMP_DIR/chunk"
[ "$(wc -c < "$TMP_DIR/chunk" | tr -d ' ')" = "64" ]
curl -fsS "$BASE_URL/api/v1/users/me/downloads" \
    -H "Authorization: Bearer $TOKEN" | grep -q '"id":"smoke"'
curl -fsS "$BASE_URL/api/v1/stats/downloads" | grep -q '"id":"smoke"'

expect_status 401 -X POST "$BASE_URL/api/v1/auth/logout" \
    -H "Authorization: Bearer invalid-token"
curl -fsS -X POST "$BASE_URL/api/v1/auth/logout" \
    -H "Authorization: Bearer $TOKEN" -o /dev/null
expect_status 401 "$BASE_URL/api/v1/auth/me" -H "Authorization: Bearer $TOKEN"

printf 'Authentication, upload, favorites, and download-tracking smoke tests passed.\n'
