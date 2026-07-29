#!/bin/bash
# Start MediaMTX for RTSP media handling.
# This script is used when MediaMTX is run externally (not auto-launched by the server).

MTX_PORT=${1:-8554}
LOG_DIR=${2:-/tmp/rtsp-server}

mkdir -p "$LOG_DIR"

# Generate minimal mediamtx config
cat > /tmp/rtsp-server/mediamtx.yml <<EOF
rtspAddress: :${MTX_PORT}
rtspDisable: no
rtspsDisable: yes
rtmpDisable: yes
hlsDisable: yes
webrtcDisable: yes
srtDisable: yes
logLevel: info
logDestinations: [stdout]
paths:
  robot_audio:
    source: publisher
    sourceProtocol: automatic
  tts_audio:
    source: publisher
    sourceProtocol: automatic
EOF

echo "Starting MediaMTX on port ${MTX_PORT}..."
exec mediamtx /tmp/rtsp-server/mediamtx.yml 2>&1 | tee "$LOG_DIR/mediamtx.log"
