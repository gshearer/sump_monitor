#!/bin/bash

# ==============================================================================
# Sump Pump Monitor - Notification Script
# ==============================================================================

# NTFY app -- get this from apple/google stores
# see https://ntfy.sh/
NTFY_TOPIC="changeme"

ALERT_SOUND="/usr/local/share/sound/sump_alert.wav"

# Grab the state passed by the C daemon
STATE=$1

if [ "$STATE" == "WET" ]; then
    # you may need to update the args below to reflect your alsa device
    # use "aplay -l" to see whats available on your rpi
    amixer -c 1 sset PCM '100%' &>/dev/null
    aplay -D plughw:1,0 "$ALERT_SOUND" & &>/dev/null
    disown %1
    
    curl \
      -H "Title: SUMP PUMP ALARM" \
      -H "Priority: urgent" \
      -H "Tags: warning,rotating_light" \
      -d "CRITICAL: Water detected bridging the sensor pins in the basement!" \
      "https://ntfy.sh/${NTFY_TOPIC}"
   
elif [ "$STATE" == "DRY" ]; then
    curl \
      -H "Title: SUMP PUMP ALARM" \
      -H "Tags: +1" \
      -d "NORMAL: No water detected" \
      "https://ntfy.sh/${NTFY_TOPIC}"
else
    echo "Usage: $0 [WET|DRY]"
    exit 1
fi

exit 0
