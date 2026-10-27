#!/usr/bin/env bash
# Runs a packaged release binary (binaries-native.yml): it starts and answers /api/v1/health, and it verifies TLS
# certificates when it fetches the JWKS (SYNC_AUTH_JWKS_URL) with its statically linked OpenSSL: a server with a
# publicly trusted certificate is accepted, a self-signed one refused, and SSL_CERT_FILE adds a CA (not on Windows,
# which uses the system certificate store only).
#
#   .github/scripts/smoke-test.sh dist/openvireo-sync-server-1.2.3-darwin-arm64.tar.gz
#
# Needs curl, openssl and Python 3 (PYTHON, default python3). The JWKS fetch is lazy: a request with a well-formed
# EdDSA token for an unknown key triggers it, and the server logs the outcome.
set -euo pipefail

archive=$1
python=${PYTHON:-python3}
work=$(mktemp -d)
pids=()
cleanup() {
    for pid in ${pids[@]+"${pids[@]}"}; do kill "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT

case "$archive" in
    *.zip) "$python" -m zipfile -e "$archive" "$work" ;;
    *) tar -xzf "$archive" -C "$work" ;;
esac
bin=$(ls "$work"/openvireo-sync-server-*/openvireo-sync-server "$work"/openvireo-sync-server-*/openvireo-sync-server.exe 2>/dev/null | head -n 1 || true)
[[ -n "$bin" ]] || { echo "No binary in $archive" >&2; exit 1; }
windows=false
[[ "$bin" == *.exe ]] && windows=true

fail() {
    echo "FAIL: $*" >&2
    for log in "$work"/*.log; do echo "--- $log" >&2; cat "$log" >&2; done
    exit 1
}

# start <name> <port> [VAR=value...]: runs the server in the background with its output in $work/<name>.log
start() {
    local name=$1 port=$2
    shift 2
    mkdir -p "$work/$name"
    (cd "$work/$name" && exec env SYNC_PORT="$port" SYNC_DATA_DIR="$work/$name/data" "$@" "$bin" >"$work/$name.log" 2>&1) &
    pids+=($!)
    for _ in $(seq 50); do
        if curl -fsS "http://127.0.0.1:$port/api/v1/health" >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    fail "$name: the server didn't answer on port $port"
}

stop() {
    kill "${pids[${#pids[@]} - 1]}" 2>/dev/null || true
    if $windows; then taskkill //F //IM "$(basename "$bin")" >/dev/null 2>&1 || true; fi
    sleep 0.5
}

echo "== $bin"
start health 18780
curl -fsS http://127.0.0.1:18780/api/v1/health
echo
stop

# {"alg":"EdDSA","kid":"x"}.{}.<64 zero bytes>: well-formed, so the server fetches the JWKS to find key "x"
token=$("$python" -c 'import base64; b = lambda s: base64.urlsafe_b64encode(s).rstrip(b"=").decode(); print(b(b"{\"alg\":\"EdDSA\",\"kid\":\"x\"}") + "." + b(b"{}") + "." + b(bytes(64)))')

# jwks <name> <port> <jwks url> [VAR=value...]: the server's log after a request that makes it fetch the JWKS
jwks() {
    local name=$1 port=$2 url=$3
    shift 3
    start "$name" "$port" SYNC_AUTH_MODE=jwt SYNC_AUTH_JWKS_URL="$url" SYNC_AUTH_ISSUER=https://accounts.example "$@"
    local status
    status=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $token" "http://127.0.0.1:$port/api/v1/vaults")
    [[ "$status" == 401 ]] || fail "$name: expected 401 for an unknown key, got $status"
    for _ in $(seq 75); do
        if grep -q "JWKS" "$work/$name.log"; then break; fi
        sleep 0.2
    done
    stop
}

# A public server whose certificate chains to a root in the system's store. The network can be slow on CI runners,
# so it gets three tries, each with a fresh server (one server waits before it fetches the JWKS again).
for attempt in 1 2 3; do
    jwks trusted 18781 https://token.actions.githubusercontent.com/.well-known/jwks
    if grep -q "The JWKS has no Ed25519 signing key" "$work/trusted.log"; then break; fi
    [[ $attempt == 3 ]] && fail "trusted: the JWKS from a publicly trusted server wasn't fetched"
    echo "trusted: attempt $attempt failed: $(grep "Cannot fetch" "$work/trusted.log" || echo "no fetch logged")"
done
echo "ok: fetched the JWKS from a server with a publicly trusted certificate"

# A local HTTPS server with a self-signed certificate, which logs every request it gets
# Git Bash on Windows rewrites arguments that look like paths; keep it off the subject only, not the file names
MSYS2_ARG_CONV_EXCL=/CN openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=127.0.0.1 \
    -addext subjectAltName=IP:127.0.0.1 -keyout "$work/key.pem" -out "$work/cert.pem" 2>"$work/openssl.log" \
    || fail "openssl couldn't make the test certificate"
cat >"$work/https.py" <<'EOF'
import http.server, ssl, sys
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'{"keys":[]}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, format, *args):
        print("GET", self.path, flush=True)
server = http.server.HTTPServer(("127.0.0.1", 18443), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[1], sys.argv[2])
server.socket = context.wrap_socket(server.socket, server_side=True)
# The handshake runs in accept(); a client that refuses the certificate without closing mustn't block the next one
server.socket.settimeout(5)
server.serve_forever()
EOF
"$python" -u "$work/https.py" "$work/cert.pem" "$work/key.pem" >>"$work/https.log" 2>&1 &
pids+=($!)
for _ in $(seq 75); do
    if curl -sk -o /dev/null https://127.0.0.1:18443/ready; then break; fi
    sleep 0.2
done
curl -sk -o /dev/null https://127.0.0.1:18443/ready || fail "the test HTTPS server didn't start"
# Only the binary's requests count from here on (the server appends, so emptying the log is safe)
: >"$work/https.log"

jwks untrusted 18782 https://127.0.0.1:18443/jwks.json
grep -q "Cannot fetch the JWKS" "$work/untrusted.log" || fail "untrusted: no failed fetch logged"
# A timeout means the test server never answered, which says nothing about certificate checks
if grep -q "Cannot fetch the JWKS.*Timeout" "$work/untrusted.log"; then fail "untrusted: timed out instead of refusing the certificate"; fi
if grep -q "^GET" "$work/https.log"; then fail "untrusted: the request reached a server with a self-signed certificate"; fi
echo "ok: refused a server with a self-signed certificate"

if ! $windows; then
    jwks custom-ca 18783 https://127.0.0.1:18443/jwks.json SSL_CERT_FILE="$work/cert.pem"
    grep -q "^GET /jwks.json" "$work/https.log" || fail "custom-ca: SSL_CERT_FILE didn't make the certificate trusted"
    grep -q "The JWKS has no Ed25519 signing key" "$work/custom-ca.log" || fail "custom-ca: the JWKS wasn't read"
    echo "ok: trusted a CA from SSL_CERT_FILE"
fi
echo "Smoke test passed"
