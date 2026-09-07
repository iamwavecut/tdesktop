#!/usr/bin/env bash
# Regenerates Telegram/Resources/update/{root-public.pem,manifest.min.json,
# manifest.sig}, the trust anchors compiled into the binary.
#
#   Telegram/build/forkgram_update_keys.sh --root <root-private.pem> \
#       [--out <dir>] [--manifest-version <n>] [--issued <unix-seconds>] \
#       <key-id>=<key.pem> [<key-id>=<key.pem> ...]
#
# Every key id lands in one group per channel, so a rotation is this command
# with the new id appended: a channel's outer list is an AND over groups and
# only the inner one is an OR over keys. Keys are given as either half of the
# pair, no expiry is ever written, and OpenSSL 3 is required.

set -e

root=""
out="$(cd "$(dirname "$0")/../Resources/update" && pwd)"
manifest_version=1
issued=""
keys=()

usage() {
	sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
	case "$1" in
	--root) root="$2"; shift 2;;
	--out) out="$2"; shift 2;;
	--manifest-version) manifest_version="$2"; shift 2;;
	--issued) issued="$2"; shift 2;;
	-h|--help) usage; exit 0;;
	*=*) keys+=("$1"); shift;;
	*) echo "Unknown argument: $1"; usage; exit 1;;
	esac
done

if [ -z "$root" ] || [ ${#keys[@]} -eq 0 ]; then
	usage
	exit 1
fi
if [ ! -r "$root" ]; then
	echo "Cannot read the root private key: $root"
	exit 1
fi
if ! openssl genpkey -algorithm ed25519 2>/dev/null >/dev/null; then
	echo "This OpenSSL cannot do Ed25519, use OpenSSL 3."
	exit 1
fi

mkdir -p "$out"
openssl pkey -in "$root" -pubout -out "$out/root-public.pem"

python3 - "$out" "$manifest_version" "$issued" "${keys[@]}" << 'EOF'
import base64, json, re, subprocess, sys, time

out_dir, manifest_version, issued = sys.argv[1], int(sys.argv[2]), sys.argv[3]
specs = sys.argv[4:]

def public_der(path):
    for args in (['-in', path, '-pubout'], ['-pubin', '-in', path, '-pubout']):
        done = subprocess.run(['openssl', 'pkey'] + args + ['-outform', 'DER'],
                              capture_output=True)
        if done.returncode == 0:
            return done.stdout
    sys.exit(f'Cannot read a public key out of {path}.')

keys, ids = [], []
for spec in specs:
    key_id, _, path = spec.partition('=')
    if not re.fullmatch(r'[A-Za-z0-9._-]+', key_id):
        sys.exit(f'Bad key id: {key_id}')
    if key_id in ids:
        sys.exit(f'Duplicate key id: {key_id}')
    der = public_der(path)
    if len(der) != 44 or der[:12] != bytes.fromhex('302a300506032b6570032100'):
        sys.exit(f'{path} is not an Ed25519 key.')
    ids.append(key_id)
    keys.append({'id': key_id, 'alg': 'Ed25519',
                 'x': base64.urlsafe_b64encode(der[-32:]).rstrip(b'=').decode()})

group = [ids]
manifest = {
    'format': 1,
    'manifest_version': manifest_version,
    'issued': int(issued) if issued else int(time.time()),
    'keys': keys,
    'channels': {'stable': group, 'beta': group},
    'revoked': [],
}
with open(f'{out_dir}/manifest.min.json', 'wb') as f:
    f.write(json.dumps(manifest, separators=(',', ':')).encode())
print('Keys: ' + ', '.join(ids))
EOF

openssl pkeyutl -sign -inkey "$root" -rawin \
	-in "$out/manifest.min.json" -out "$out/manifest.sig"
openssl pkeyutl -verify -pubin -inkey "$out/root-public.pem" -rawin \
	-in "$out/manifest.min.json" -sigfile "$out/manifest.sig"

echo "Wrote $out/{root-public.pem,manifest.min.json,manifest.sig}"
