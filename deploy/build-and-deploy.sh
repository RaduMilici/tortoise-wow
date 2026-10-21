#!/usr/bin/env bash
# Builds tortoise-server:mine from the pushed tortoise-wow commit and deploys it to the Lightsail host.
# Run from Git Bash (Docker Desktop running):  bash deploy/build-and-deploy.sh [--build-only | --deploy-only] [--yes]
#
#   --build-only   checks, then builds the image locally; the server is not touched
#   --deploy-only  skips the build and deploys the local tortoise-server:mine
#   --yes          no confirmation before restarting the live server
#
# Settings can be overridden with environment variables (defaults below).
# One-time server setup (config, SOAP account, credentials file) is checked, never edited:
# see wow-forge/docs/server-character-operations.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="${CORE_DIR:-$(cd "$HERE/.." && pwd)}"
DEPLOY_DIR="${DEPLOY_DIR:-$(cd "$CORE_DIR/.." && pwd)/tortoise-deploy}"
FORGE_DIR="${FORGE_DIR:-$(cd "$CORE_DIR/.." && pwd)/wow-forge}"
HOST="${LIGHTSAIL_HOST:-ubuntu@63.184.170.236}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/lightsail.pem}"
REMOTE_DIR="${REMOTE_DIR:-tortoise-deploy}"   # relative to the remote home
CORE_URL="${CORE_URL:-https://github.com/RaduMilici/tortoise-wow.git}"
IMAGE="tortoise-server:mine"
WORK="${WORK:-${TMPDIR:-/tmp}/tortoise-server-deploy}"

build=1 deploy=1 assume_yes=0
for arg in "$@"; do
  case "$arg" in
    --build-only) deploy=0 ;;
    --deploy-only) build=0 ;;
    --yes) assume_yes=1 ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

mkdir -p "$WORK"
LOG="$WORK/deploy-$(date +%Y%m%d-%H%M%S).log"
START=$SECONDS
step=0
log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG"; }
phase() { step=$((step + 1)); printf '\n' | tee -a "$LOG"; log "===== $step. $* ====="; }
fail() { log "FAILED: $*"; log "Full log: $LOG"; exit 1; }
elapsed() { local s=$((SECONDS - ${1:-$START})); printf '%dm%02ds' $((s / 60)) $((s % 60)); }
SSH_OPTS=(-i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30)
# Runs a script on the host with `bash -s`; remote output is prefixed so it reads apart from local steps.
remote() { ssh "${SSH_OPTS[@]}" "$HOST" "bash -s -- $*" 2>&1 | sed -u 's/^/    server| /' | tee -a "$LOG"; }

log "Log file: $LOG"
log "Core: $CORE_DIR | deploy recipe: $DEPLOY_DIR | host: $HOST"

# ---------------------------------------------------------------------------
phase "Local checks"
for tool in git docker ssh scp gzip; do
  command -v "$tool" >/dev/null || fail "'$tool' is not on PATH."
done
docker info >/dev/null 2>&1 || fail "Docker is not running. Start Docker Desktop and retry."
[ -f "$SSH_KEY" ] || fail "SSH key not found: $SSH_KEY"
[ -f "$CORE_DIR/deploy/server-build.env" ] || fail "Missing $CORE_DIR/deploy/server-build.env"
# shellcheck disable=SC1090
source <(sed 's/\r$//' "$CORE_DIR/deploy/server-build.env")
for name in DEPLOY_REVISION MODULES MODULE_CONFIGS MODULE_SQL_MODULES MODULE_BUILD_PACKAGES; do
  [ -n "${!name:-}" ] || fail "server-build.env does not set $name."
done
log "Build settings loaded (deploy recipe pinned at ${DEPLOY_REVISION:0:12})."

if [ "$build" = 1 ]; then
  CORE_REVISION="$(git -C "$CORE_DIR" rev-parse HEAD)"
  log "tortoise-wow HEAD: $CORE_REVISION ($(git -C "$CORE_DIR" log -1 --format=%s))"
  [ -z "$(git -C "$CORE_DIR" status --porcelain --untracked-files=no)" ] ||
    log "WARNING: tortoise-wow has uncommitted changes. The image is built from the pushed commit only."
  log "Checking the commit is on GitHub (the build clones it from there)..."
  git -C "$CORE_DIR" fetch -q origin
  git -C "$CORE_DIR" branch -r --contains "$CORE_REVISION" | grep -q 'origin/' ||
    fail "Commit $CORE_REVISION is not pushed. Run: git -C \"$CORE_DIR\" push origin main"

  [ -d "$DEPLOY_DIR/.git" ] || fail "No tortoise-deploy clone at $DEPLOY_DIR"
  log "Preparing tortoise-deploy at the pinned revision with Linux line endings..."
  # A Windows checkout (core.autocrlf=true) gives the container scripts CRLF endings, which breaks them in the image.
  git -C "$DEPLOY_DIR" config core.autocrlf false
  # Line-ending differences are discarded; any real local edit stops the script instead.
  git -C "$DEPLOY_DIR" diff --ignore-cr-at-eol --quiet ||
    fail "tortoise-deploy has local edits beyond line endings. Commit or stash them in $DEPLOY_DIR first."
  [ -z "$(git -C "$DEPLOY_DIR" ls-files --others --exclude-standard)" ] ||
    log "Note: tortoise-deploy has untracked files; they are left alone (and are not copied into the image unless under docker/)."
  git -C "$DEPLOY_DIR" cat-file -e "$DEPLOY_REVISION^{commit}" 2>/dev/null || git -C "$DEPLOY_DIR" fetch -q origin
  git -C "$DEPLOY_DIR" checkout -q -f --detach "$DEPLOY_REVISION"
  git -C "$DEPLOY_DIR" rm -rq --cached .
  git -C "$DEPLOY_DIR" reset -q --hard HEAD
  if grep -rlI $'\r' "$DEPLOY_DIR/docker" >/dev/null 2>&1; then
    fail "tortoise-deploy/docker still has CRLF line endings."
  fi
  log "tortoise-deploy ready: $(git -C "$DEPLOY_DIR" log -1 --format='%h %s')"
fi
if [ "$deploy" = 1 ] && [ "$build" = 0 ]; then
  docker image inspect "$IMAGE" >/dev/null 2>&1 || fail "No local $IMAGE to deploy. Build it first."
fi
log "Local checks passed."

# ---------------------------------------------------------------------------
if [ "$deploy" = 1 ]; then
  phase "Server checks (read-only, before the long build)"
  ssh "${SSH_OPTS[@]}" "$HOST" true 2>/dev/null || fail "Cannot SSH to $HOST with $SSH_KEY."
  remote "$REMOTE_DIR" <<'REMOTE' || fail "Server setup is incomplete; fix the items above, then rerun."
set -uo pipefail
cd "$HOME/$1" || { echo "No ~/$1 on the server."; exit 1; }
problems=0
need() { echo "MISSING: $1"; echo "    fix: $2"; problems=$((problems + 1)); }
# Last value of `Key = value` in a config file, quotes and CR removed.
value() { sed -n -E "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"?([^\"]*)\"?[[:space:]]*$/\1/p" "$2" 2>/dev/null | tail -1 | tr -d '\r'; }
module=config/modules/mod-creator-test.conf
[ "$(value 'ForgeCharacters\.Enable' "$module")" = 1 ] ||
  need "ForgeCharacters.Enable = 1 in $module" "add the line 'ForgeCharacters.Enable = 1'"
[ "$(value 'ForgeCharacters\.AuditDirectory' "$module")" = /opt/tortoise/storage/forge-characters ] ||
  need "ForgeCharacters.AuditDirectory in $module" "add 'ForgeCharacters.AuditDirectory = \"/opt/tortoise/storage/forge-characters\"'"
[ "$(value 'SOAP\.Enabled' config/mangosd.conf)" = 1 ] || need "SOAP.Enabled = 1 in config/mangosd.conf" "set SOAP.Enabled = 1"
[ "$(value 'SOAP\.IP' config/mangosd.conf)" = 0.0.0.0 ] || need "SOAP.IP = 0.0.0.0 in config/mangosd.conf" "set SOAP.IP = \"0.0.0.0\" (inside the container; the port below keeps it private)"
[ "$(value 'SOAP\.Port' config/mangosd.conf)" = 7878 ] || need "SOAP.Port = 7878 in config/mangosd.conf" "set SOAP.Port = 7878"
command -v python3 >/dev/null || need "python3 on the host" "sudo apt install -y python3"
if docker compose config --format json >/tmp/forge-compose.json 2>/dev/null; then
  verdict="$(python3 - /tmp/forge-compose.json <<'PY'
import json, sys
service = json.load(open(sys.argv[1]))["services"]["mangosd"]
ports = [p for p in service.get("ports", []) if str(p.get("target")) == "7878"]
volumes = [v for v in service.get("volumes", []) if v.get("target") == "/opt/tortoise/storage/forge-characters"]
if not ports: print("noport")
elif any(p.get("host_ip") != "127.0.0.1" for p in ports): print("public")
elif not volumes: print("novolume")
else: print("ok " + volumes[0]["source"])
PY
)"
  case "$verdict" in
    noport) need "SOAP port mapping for mangosd" "in compose.override.yaml under mangosd: ports: [\"127.0.0.1:7878:7878\"]" ;;
    public) need "a PRIVATE SOAP port: 7878 is published beyond 127.0.0.1" "use exactly 127.0.0.1:7878:7878 and remove any other 7878 mapping" ;;
    novolume) need "audit folder mount for mangosd" "in compose.override.yaml under mangosd volumes: - ./storage/mangosd/forge-characters:/opt/tortoise/storage/forge-characters" ;;
    ok*) audit="${verdict#ok }"
      if [ ! -d "$audit" ]; then mkdir -p "$audit" && chmod 700 "$audit" && echo "Created audit folder $audit"; fi ;;
    *) need "a readable compose configuration" "run 'docker compose config' on the server" ;;
  esac
  rm -f /tmp/forge-compose.json
else
  need "a valid compose configuration" "run 'docker compose config' in ~/$1 and fix the error it prints"
fi
[ -r /etc/forge-character-command.env ] ||
  echo "NOTE: /etc/forge-character-command.env is missing; the server will deploy, but Forge's character command will not work until you add it."
free="$(df -Pk "$HOME" | awk 'NR==2 {print int($4 / 1048576)}')"
[ "$free" -ge 5 ] || need "5 GB free disk (have ${free} GB)" "free some space"
echo "Running now: $(docker compose ps --format '{{.Service}} {{.State}}' | tr '\n' ' ')"
[ "$problems" = 0 ] && echo "Server setup looks complete."
exit "$problems"
REMOTE
fi

# ---------------------------------------------------------------------------
if [ "$build" = 1 ]; then
  phase "Build $IMAGE from ${CORE_REVISION:0:12} (long: compiles the whole server)"
  BUILD_LOG="$WORK/build-${CORE_REVISION:0:12}.log"
  log "Compiler output: $BUILD_LOG (also streamed here)"
  build_start=$SECONDS
  (
    cd "$DEPLOY_DIR"
    # The Dockerfile uses COPY --exclude, which older built-in Dockerfile parsers (Docker 26) reject.
    MSYS_NO_PATHCONV=1 docker build --progress=plain --platform linux/amd64 \
      --build-arg BUILDKIT_SYNTAX=docker/dockerfile:1.7-labs \
      -f docker/server/Dockerfile \
      --build-arg TORTOISE_REPOSITORY_URL="$CORE_URL" \
      --build-arg TORTOISE_REVISION="$CORE_REVISION" \
      --build-arg TORTOISE_MODULES="$MODULES" \
      --build-arg TORTOISE_MODULE_CONFIGS="$MODULE_CONFIGS" \
      --build-arg TORTOISE_MODULE_SQL_MODULES="$MODULE_SQL_MODULES" \
      --build-arg TORTOISE_MODULE_BUILD_PACKAGES="$MODULE_BUILD_PACKAGES" \
      --label "tortoise-wow.revision=$CORE_REVISION" \
      -t "$IMAGE" -t "tortoise-server:${CORE_REVISION:0:12}" .
  ) 2>&1 | tee "$BUILD_LOG" || {
    log "Last compiler errors:"
    grep -E 'error:|ERROR|Error [0-9]|FAILED' "$BUILD_LOG" | tail -20 | tee -a "$LOG"
    fail "Docker build failed after $(elapsed "$build_start"). Nothing was deployed."
  }
  log "Built $IMAGE in $(elapsed "$build_start") ($(docker image inspect -f '{{.Size}}' "$IMAGE" | awk '{printf "%.0f MB", $1 / 1048576}'))."
fi
[ "$deploy" = 1 ] || { log "Build only: done in $(elapsed). Deploy later with --deploy-only."; exit 0; }

# ---------------------------------------------------------------------------
phase "Confirm"
revision="$(docker image inspect -f '{{index .Config.Labels "tortoise-wow.revision"}}' "$IMAGE" 2>/dev/null || true)"
log "About to deploy $IMAGE (tortoise-wow ${revision:-unknown revision}) to $HOST."
log "This backs up the database, then restarts mangosd and realmd: players are disconnected."
if [ "$assume_yes" != 1 ]; then
  read -r -p "Type DEPLOY to continue: " answer
  [ "$answer" = DEPLOY ] || fail "Cancelled; the server was not touched."
fi

# ---------------------------------------------------------------------------
phase "Package the image"
archive="$WORK/tortoise-server.tar.gz"
rm -f "$archive"
pack_start=$SECONDS
log "Saving and compressing $IMAGE..."
docker save "$IMAGE" | gzip -1 > "$archive"
log "Packed $(du -m "$archive" | cut -f1) MB in $(elapsed "$pack_start")."

phase "Upload"
upload_start=$SECONDS
scp "${SSH_OPTS[@]}" "$archive" "$HOST:/tmp/tortoise-server.tar.gz" || fail "Upload failed; the server was not touched."
log "Uploaded in $(elapsed "$upload_start")."
if [ -f "$FORGE_DIR/scripts/forge-character-command.py" ]; then
  scp -q "${SSH_OPTS[@]}" "$FORGE_DIR/scripts/forge-character-command.py" "$HOST:/tmp/forge-character-command.py"
  log "Uploaded the Forge character helper."
fi

# ---------------------------------------------------------------------------
phase "Back up the database"
remote "$REMOTE_DIR" <<'REMOTE' || fail "Backup failed; nothing was changed. The uploaded image is in /tmp on the server."
set -euo pipefail
cd "$HOME/$1"
mkdir -p "$HOME/backups"
file="$HOME/backups/tortoise-$(date +%Y%m%d-%H%M%S).sql.gz"
echo "Dumping all databases to $file ..."
docker compose exec -T database sh -c 'mariadb-dump -u root -p"$MARIADB_ROOT_PASSWORD" --all-databases --single-transaction' | gzip > "$file"
gzip -t "$file"
size="$(du -m "$file" | cut -f1)"
[ "$(stat -c %s "$file")" -gt 100000 ] || { echo "Backup is suspiciously small."; exit 1; }
echo "Backup OK: $file (${size} MB)"
REMOTE

# ---------------------------------------------------------------------------
phase "Load the image and restart"
remote "$REMOTE_DIR" <<'REMOTE' || fail "Deploy failed. See the server lines above; roll back with the command at the end of the log."
set -euo pipefail
cd "$HOME/$1"
if docker image inspect tortoise-server:mine >/dev/null 2>&1; then
  stamp="$(date +%Y%m%d-%H%M%S)"
  docker tag tortoise-server:mine tortoise-server:rollback
  docker tag tortoise-server:mine "tortoise-server:rollback-$stamp"
  echo "Kept the running image as tortoise-server:rollback (and rollback-$stamp)."
fi
echo "Loading the new image (a minute or two)..."
gunzip -c /tmp/tortoise-server.tar.gz | docker load
rm -f /tmp/tortoise-server.tar.gz
# The helper is optional for the deploy itself: a sudo problem must not stop the restart.
if [ -f /tmp/forge-character-command.py ]; then
  if sudo -n install -m 755 /tmp/forge-character-command.py /usr/local/lib/forge-character-command.py; then
    if [ ! -x /usr/local/bin/forge-character-command ]; then
      printf '#!/bin/sh\nset -eu\nset -a\n. /etc/forge-character-command.env\nset +a\nexec python3 /usr/local/lib/forge-character-command.py\n' |
        sudo -n tee /usr/local/bin/forge-character-command >/dev/null &&
        sudo -n chmod 755 /usr/local/bin/forge-character-command &&
        echo "Installed /usr/local/bin/forge-character-command."
    fi
    echo "Updated the Forge character helper."
  else
    echo "WARNING: could not install the Forge character helper (sudo needs a password?)."
  fi
  rm -f /tmp/forge-character-command.py
fi
since="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "$since" > /tmp/tortoise-deploy-since
echo "Restarting with docker compose up -d ..."
docker compose up -d
REMOTE

phase "Wait for the world server"
remote "$REMOTE_DIR" <<'REMOTE' || fail "mangosd did not come up cleanly. Roll back with the command below if players cannot log in."
set -uo pipefail
cd "$HOME/$1"
since="$(cat /tmp/tortoise-deploy-since)"
for i in $(seq 1 90); do
  state="$(docker compose ps -a --format '{{.State}}' mangosd)"
  logs="$(docker compose logs --no-color --since "$since" mangosd 2>&1)"
  if printf '%s' "$logs" | grep -q 'SOAP: remote command interface bound'; then
    echo "World server is up after $((i * 10)) s: $(printf '%s' "$logs" | grep 'SOAP: remote command' | tail -1 | sed 's/^.*| //')"
    break
  fi
  if printf '%s' "$logs" | grep -q 'SOAP: could not bind'; then echo "SOAP could not bind."; exit 1; fi
  if [ "$state" != running ]; then
    echo "mangosd is '$state'. Last log lines:"; printf '%s\n' "$logs" | tail -40; exit 1
  fi
  [ "$i" = 90 ] && { echo "Still not up after 15 minutes. Last log lines:"; printf '%s\n' "$logs" | tail -40; exit 1; }
  printf '%3ds  %s\n' $((i * 10)) "$(printf '%s' "$logs" | tr -d '\r' | grep -v '^\s*$' | tail -1 | sed 's/^[^|]*| //' | cut -c1-110)"
  sleep 10
done
listeners="$(ss -ltnH 'sport = :7878' | awk '{print $4}' | sort -u | tr '\n' ' ')"
echo "Port 7878 listens on: ${listeners:-nothing}"
case "$listeners" in
  *127.0.0.1:7878*) ;;
  *) echo "SOAP is not published on 127.0.0.1:7878."; exit 1 ;;
esac
if printf '%s' "$listeners" | grep -qvE '^(127\.0\.0\.1:7878 ?)+$'; then
  echo "DANGER: SOAP is reachable beyond loopback. Fix compose.override.yaml and run 'docker compose up -d' now."; exit 1
fi
if [ -x /usr/local/bin/forge-character-command ] && [ -r /etc/forge-character-command.env ]; then
  # Account 0 is always refused by the module: proves SOAP login and the forgecharacter command without touching data.
  reply="$(echo '{"version":1,"operation":"inspect","account":0,"guid":0}' | /usr/local/bin/forge-character-command 2>&1 || true)"
  case "$reply" in
    *'Close every game client'*) echo "Forge character command answers correctly." ;;
    *) echo "Forge character command check failed: $reply"; exit 1 ;;
  esac
else
  echo "Skipped the Forge character check: add /etc/forge-character-command.env (see the doc)."
fi
docker compose ps --format '{{.Service}}: {{.State}} {{.Status}}'
REMOTE

phase "Done"
log "Deployed in $(elapsed). Full log: $LOG"
log "Roll back if needed:"
log "  ssh -i $SSH_KEY $HOST 'docker tag tortoise-server:rollback tortoise-server:mine && cd ~/$REMOTE_DIR && docker compose up -d'"
