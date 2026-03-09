#!/bin/bash

# ==============================================================================
# Sump Pump Monitor - Notification Script
# ==============================================================================

# Run aplay in the background (&) so the script continues immediately
[[ -f /usr/local/share/sound/siren.wav ]] && aplay /usr/local/share/sound/siren.wav &

# old-school SMTP :-) this example uses a gmail via a google app password
SMTPHOST="smtp.gmail.com"
SMTPPORT="587"
SMTPFROM="your_email@gmail.com"
SMTPUSER="$SMTPFROM"
SMTPPASS="your_16_char_google_app_password" 

sendmail()
{
  if [ "$#" -ne 3 ]; then
    echo "Usage: sendmail <to> <subject> <body>"
    return 1
  fi

  local TO="$1"
  local SUBJECT="$2"
  local BODY="$3"

  # Use cURL to handle the SMTP transaction natively
  # The --upload-file - flag tells cURL to read the email payload from stdin
  echo -e "Subject: ${SUBJECT}\n\n${BODY}" | curl --ssl-reqd \
    --url "smtp://${SMTPHOST}:${SMTPPORT}" \
    --user "${SMTPUSER}:${SMTPPASS}" \
    --mail-from "${SMTPFROM}" \
    --mail-rcpt "${TO}" \
    --upload-file -
}

# NTFY app -- get this from apple/google stores
NTFY_TOPIC="your_unique_sump_topic_name_here"

ntfy()
{
  curl \
    -H "Title: SUMP PUMP ALARM" \
    -H "Priority: urgent" \
    -H "Tags: warning,rotating_light" \
    -d "CRITICAL: Water detected bridging the sensor pins in the basement!" \
    "https://ntfy.sh/${NTFY_TOPIC}"
}

# TWILIO_SID="your_account_sid_here"
# TWILIO_AUTH="your_auth_token_here"
# TWILIO_FROM="+1234567890"  # Your Twilio Number
# TWILIO_TO="+0987654321"    # Your Cell Phone Number

# curl -X POST "https://api.twilio.com/2010-04-01/Accounts/${TWILIO_SID}/Messages.json" \
#   --data-urlencode "Body=CRITICAL: Sump pit water level is HIGH." \
#   --data-urlencode "From=${TWILIO_FROM}" \
#   --data-urlencode "To=${TWILIO_TO}" \
#   -u "${TWILIO_SID}:${TWILIO_AUTH}"

CURRENT_TIME=$(date)
ALERT_SUBJECT="CRITICAL: Sump Water Detected"
ALERT_BODY="The sump pump monitor detected water bridging the sensor pins at ${CURRENT_TIME}."

# Call the function to send the email
# of course you may want to send more than one email
# perhaps to an SMS gateway service
sendmail "destination_email@gmail.com" "$ALERT_SUBJECT" "$ALERT_BODY"
ntfy

# make this show up in journald
echo $ALERT_BODY

exit 0
